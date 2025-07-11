#include <cstdio>
#include <thread>
#include <base/MemorySanitizer.h>
#include <Compression/CompressionCodecDeflateQpl.h>
#include <Compression/CompressionFactory.h>
#include <Compression/CompressionInfo.h>
#include <Parsers/IAST.h>
#include <base/getPageSize.h>
#include <base/scope_guard.h>
#include <Poco/Logger.h>
#include <Common/logger_useful.h>
#include <Common/randomSeed.h>

#if USE_QPL

#include <libaccel_config.h>

#include <immintrin.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
}

DeflateQplJobHWPool & DeflateQplJobHWPool::instance()
{
    static DeflateQplJobHWPool pool;
    return pool;
}

DeflateQplJobHWPool::DeflateQplJobHWPool()
    : max_hw_jobs(0)
    , random_engine(randomSeed())
{
    LoggerPtr log = getLogger("DeflateQplJobHWPool");
    const char * qpl_version = qpl_get_library_version();

    // loop all configured workqueue size to get maximum job number.
    accfg_ctx * ctx_ptr = nullptr;
    auto ctx_status = accfg_new(&ctx_ptr);
    SCOPE_EXIT({ accfg_unref(ctx_ptr); });
    if (ctx_status == 0)
    {
        auto * dev_ptr = accfg_device_get_first(ctx_ptr);
        while (dev_ptr != nullptr)
        {
            for (auto * wq_ptr = accfg_wq_get_first(dev_ptr); wq_ptr != nullptr; wq_ptr = accfg_wq_get_next(wq_ptr))
                max_hw_jobs += accfg_wq_get_size(wq_ptr);
            dev_ptr = accfg_device_get_next(dev_ptr);
        }
    }
    else
    {
        job_pool_ready = false;
        LOG_WARNING(log, "Initialization of hardware-assisted DeflateQpl codec failed, falling back to software DeflateQpl codec. Failed to create new libaccel_config context -> status: {}, QPL Version: {}.", ctx_status, qpl_version);
        return;
    }

    if (max_hw_jobs == 0)
    {
        job_pool_ready = false;
        LOG_WARNING(log, "Initialization of hardware-assisted DeflateQpl codec failed, falling back to software DeflateQpl codec. Failed to get available workqueue size -> total_wq_size: {}, QPL Version: {}.", max_hw_jobs, qpl_version);
        return;
    }
    distribution = std::uniform_int_distribution<int>(0, max_hw_jobs - 1);
    /// Get size required for saving a single qpl job object
    qpl_get_job_size(qpl_path_hardware, &per_job_size);
    /// Allocate job buffer pool for storing all job objects
    hw_jobs_buffer = std::make_unique<uint8_t[]>(per_job_size * max_hw_jobs);
    hw_job_ptr_locks = std::make_unique<std::atomic_bool[]>(max_hw_jobs);
    /// Initialize all job objects in job buffer pool
    for (UInt32 index = 0; index < max_hw_jobs; ++index)
    {
        qpl_job * job_ptr = reinterpret_cast<qpl_job *>(hw_jobs_buffer.get() + index * per_job_size);
        if (auto status = qpl_init_job(qpl_path_hardware, job_ptr); status != QPL_STS_OK)
        {
            job_pool_ready = false;
            LOG_WARNING(log, "Initialization of hardware-assisted DeflateQpl codec failed, falling back to software DeflateQpl codec. Failed to Initialize qpl job -> status: {}, QPL Version: {}.", static_cast<UInt32>(status), qpl_version);
            return;
        }
        unLockJob(index);
    }

    job_pool_ready = true;
    LOG_DEBUG(log, "Hardware-assisted DeflateQpl codec is ready! QPL Version: {}, max_hw_jobs: {}",qpl_version, max_hw_jobs);
}

DeflateQplJobHWPool::~DeflateQplJobHWPool()
{
    for (UInt32 i = 0; i < max_hw_jobs; ++i)
    {
        qpl_job * job_ptr = reinterpret_cast<qpl_job *>(hw_jobs_buffer.get() + i * per_job_size);
        while (!tryLockJob(i));
        qpl_fini_job(job_ptr);
        unLockJob(i);
    }
    job_pool_ready = false;
}

qpl_job * DeflateQplJobHWPool::acquireJob(UInt32 & job_id)
{
    if (isJobPoolReady())
    {
        UInt32 retry = 0;
        UInt32 index = distribution(random_engine);
        while (!tryLockJob(index))
        {
            index = distribution(random_engine);
            retry++;
            if (retry > max_hw_jobs)
            {
                return nullptr;
            }
        }
        job_id = max_hw_jobs - index;
        assert(index < max_hw_jobs);
        return reinterpret_cast<qpl_job *>(hw_jobs_buffer.get() + index * per_job_size);
    }
    return nullptr;
}

void DeflateQplJobHWPool::releaseJob(UInt32 job_id)
{
    if (isJobPoolReady())
        unLockJob(max_hw_jobs - job_id);
}

bool DeflateQplJobHWPool::tryLockJob(UInt32 index)
{
    bool expected = false;
    assert(index < max_hw_jobs);
    return hw_job_ptr_locks[index].compare_exchange_strong(expected, true);
}

void DeflateQplJobHWPool::unLockJob(UInt32 index)
{
    assert(index < max_hw_jobs);
    hw_job_ptr_locks[index].store(false);
}

HardwareCodecDeflateQpl::HardwareCodecDeflateQpl(SoftwareCodecDeflateQpl & sw_codec_)
    : log(getLogger("HardwareCodecDeflateQpl"))
    , sw_codec(sw_codec_)
{
}

HardwareCodecDeflateQpl::~HardwareCodecDeflateQpl()
{
#ifndef NDEBUG
    assert(decomp_async_job_map.empty());
#else
    if (!decomp_async_job_map.empty())
    {
        LOG_WARNING(log, "Find un-released job when HardwareCodecDeflateQpl destroy");
        for (auto it : decomp_async_job_map)
        {
            DeflateQplJobHWPool::instance().releaseJob(it.first);
        }
        decomp_async_job_map.clear();
    }
#endif
}

Int32 HardwareCodecDeflateQpl::doCompressData(const char * source, UInt32 source_size, char * dest, UInt32 dest_size) const
{
    UInt32 job_id = 0;
    qpl_job * job_ptr = nullptr;
    UInt32 compressed_size = 0;
    if (!(job_ptr = DeflateQplJobHWPool::instance().acquireJob(job_id)))
    {
        LOG_INFO(log, "DeflateQpl HW codec failed, falling back to SW codec. (Details: doCompressData->acquireJob fail, probably job pool exhausted)");
        return RET_ERROR;
    }

    job_ptr->op = qpl_op_compress;
    job_ptr->next_in_ptr = reinterpret_cast<uint8_t *>(const_cast<char *>(source));
    job_ptr->next_out_ptr = reinterpret_cast<uint8_t *>(dest);
    job_ptr->available_in = source_size;
    job_ptr->level = qpl_default_level;
    job_ptr->available_out = dest_size;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_LAST | QPL_FLAG_OMIT_VERIFY;

    auto status = qpl_execute_job(job_ptr);
    if (status == QPL_STS_OK)
    {
        compressed_size = job_ptr->total_out;
        DeflateQplJobHWPool::instance().releaseJob(job_id);
        return compressed_size;
    }

    LOG_WARNING(
        log,
        "DeflateQpl HW codec failed, falling back to SW codec. (Details: doCompressData->qpl_execute_job with error code: {} - please "
        "refer to qpl_status in ./contrib/qpl/include/qpl/c_api/status.h)",
        static_cast<UInt32>(status));
    DeflateQplJobHWPool::instance().releaseJob(job_id);
    return RET_ERROR;
}

Int32 HardwareCodecDeflateQpl::doDecompressDataSynchronous(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    UInt32 job_id = 0;
    qpl_job * job_ptr = nullptr;
    UInt32 decompressed_size = 0;
    if (!(job_ptr = DeflateQplJobHWPool::instance().acquireJob(job_id)))
    {
        LOG_INFO(log, "DeflateQpl HW codec failed, falling back to SW codec. (Details: doDecompressDataSynchronous->acquireJob fail, probably job pool exhausted)");
        return RET_ERROR;
    }

    // Performing a decompression operation
    job_ptr->op = qpl_op_decompress;
    job_ptr->next_in_ptr = reinterpret_cast<uint8_t *>(const_cast<char *>(source));
    job_ptr->next_out_ptr = reinterpret_cast<uint8_t *>(dest);
    job_ptr->available_in = source_size;
    job_ptr->available_out = uncompressed_size;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST;

    auto status = qpl_submit_job(job_ptr);
    if (status != QPL_STS_OK)
    {
        DeflateQplJobHWPool::instance().releaseJob(job_id);
        LOG_WARNING(log, "DeflateQpl HW codec failed, falling back to SW codec. (Details: doDecompressDataSynchronous->qpl_submit_job with error code: {} - please refer to qpl_status in ./contrib/qpl/include/qpl/c_api/status.h)", static_cast<UInt32>(status));
        return RET_ERROR;
    }
    /// Busy waiting till job complete.
    do
    {
        _tpause(1, __rdtsc() + 1000);
        status = qpl_check_job(job_ptr);
    } while (status == QPL_STS_BEING_PROCESSED);

    if (status != QPL_STS_OK)
    {
        DeflateQplJobHWPool::instance().releaseJob(job_id);
        LOG_WARNING(log, "DeflateQpl HW codec failed, falling back to SW codec. (Details: doDecompressDataSynchronous->qpl_submit_job with error code: {} - please refer to qpl_status in ./contrib/qpl/include/qpl/c_api/status.h)", static_cast<UInt32>(status));
        return RET_ERROR;
    }

    decompressed_size = job_ptr->total_out;
    DeflateQplJobHWPool::instance().releaseJob(job_id);
    return decompressed_size;
}

Int32 HardwareCodecDeflateQpl::doDecompressDataAsynchronous(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    UInt32 job_id = 0;
    qpl_job * job_ptr = nullptr;
    if (!(job_ptr = DeflateQplJobHWPool::instance().acquireJob(job_id)))
    {
        LOG_INFO(log, "DeflateQpl HW codec failed, falling back to SW codec. (Details: doDecompressDataAsynchronous->acquireJob fail, probably job pool exhausted)");
        return RET_ERROR;
    }

    // Performing a decompression operation
    job_ptr->op = qpl_op_decompress;
    job_ptr->next_in_ptr = reinterpret_cast<uint8_t *>(const_cast<char *>(source));
    job_ptr->next_out_ptr = reinterpret_cast<uint8_t *>(dest);
    job_ptr->available_in = source_size;
    job_ptr->available_out = uncompressed_size;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST;

    auto status = qpl_submit_job(job_ptr);
    if (status == QPL_STS_OK)
    {
        decomp_async_job_map.insert({job_id, job_ptr});
        return job_id;
    }

    DeflateQplJobHWPool::instance().releaseJob(job_id);
    LOG_WARNING(
        log,
        "DeflateQpl HW codec failed, falling back to SW codec. (Details: doDecompressDataAsynchronous->qpl_submit_job with error code: {} "
        "- please refer to qpl_status in ./contrib/qpl/include/qpl/c_api/status.h)",
        static_cast<UInt32>(status));
    return RET_ERROR;
}

void HardwareCodecDeflateQpl::flushAsynchronousDecompressRequests()
{
    auto n_jobs_processing = decomp_async_job_map.size();
    std::map<UInt32, qpl_job *>::iterator it = decomp_async_job_map.begin();

    while (n_jobs_processing)
    {
        UInt32 job_id = 0;
        qpl_job * job_ptr = nullptr;
        job_id = it->first;
        job_ptr = it->second;

        auto status = qpl_check_job(job_ptr);
        if (status == QPL_STS_BEING_PROCESSED)
        {
            it++;
        }
        else
        {
            if (status != QPL_STS_OK)
            {
                sw_codec.doDecompressData(
                    reinterpret_cast<const char * >(job_ptr->next_in_ptr),
                    job_ptr->available_in,
                    reinterpret_cast<char *>(job_ptr->next_out_ptr),
                    job_ptr->available_out);
                LOG_WARNING(log, "DeflateQpl HW codec failed, falling back to SW codec. (Details: flushAsynchronousDecompressRequests with error code: {} - please refer to qpl_status in ./contrib/qpl/include/qpl/c_api/status.h)", static_cast<UInt32>(status));
            }
            it = decomp_async_job_map.erase(it);
            DeflateQplJobHWPool::instance().releaseJob(job_id);
            n_jobs_processing--;
            if (n_jobs_processing <= 0)
                break;
        }

        if (it == decomp_async_job_map.end())
        {
            it = decomp_async_job_map.begin();
            _tpause(1, __rdtsc() + 1000);
        }
    }
}

SoftwareCodecDeflateQpl::~SoftwareCodecDeflateQpl()
{
    if (!sw_job)
        qpl_fini_job(sw_job);
}

qpl_job * SoftwareCodecDeflateQpl::getJobCodecPtr()
{
    if (!sw_job)
    {
        UInt32 size = 0;
        qpl_get_job_size(qpl_path_software, &size);

        sw_buffer = std::make_unique<uint8_t[]>(size);
        sw_job = reinterpret_cast<qpl_job *>(sw_buffer.get());

        // Job initialization
        if (auto status = qpl_init_job(qpl_path_software, sw_job); status != QPL_STS_OK)
            throw Exception(ErrorCodes::CANNOT_COMPRESS,
                            "Initialization of DeflateQpl software fallback codec failed. "
                            "(Details: qpl_init_job with error code: "
                            "{} - please refer to qpl_status in ./contrib/qpl/include/qpl/c_api/status.h)",
                            static_cast<UInt32>(status));
    }
    return sw_job;
}

UInt32 SoftwareCodecDeflateQpl::doCompressData(const char * source, UInt32 source_size, char * dest, UInt32 dest_size)
{
    qpl_job * job_ptr = getJobCodecPtr();
    // Performing a compression operation
    job_ptr->op = qpl_op_compress;
    job_ptr->next_in_ptr = reinterpret_cast<uint8_t *>(const_cast<char *>(source));
    job_ptr->next_out_ptr = reinterpret_cast<uint8_t *>(dest);
    job_ptr->available_in = source_size;
    job_ptr->available_out = dest_size;
    job_ptr->level = qpl_default_level;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_LAST | QPL_FLAG_OMIT_VERIFY;

    if (auto status = qpl_execute_job(job_ptr); status != QPL_STS_OK)
        throw Exception(ErrorCodes::CANNOT_COMPRESS,
                        "Execution of DeflateQpl software fallback codec failed. "
                        "(Details: qpl_execute_job with error code: "
                        "{} - please refer to qpl_status in ./contrib/qpl/include/qpl/c_api/status.h)",
                        static_cast<UInt32>(status));

    return job_ptr->total_out;
}

void SoftwareCodecDeflateQpl::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size)
{
    qpl_job * job_ptr = getJobCodecPtr();

    // Performing a decompression operation
    job_ptr->op = qpl_op_decompress;
    job_ptr->next_in_ptr = reinterpret_cast<uint8_t *>(const_cast<char *>(source));
    job_ptr->next_out_ptr = reinterpret_cast<uint8_t *>(dest);
    job_ptr->available_in = source_size;
    job_ptr->available_out = uncompressed_size;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST;

    if (auto status = qpl_execute_job(job_ptr); status != QPL_STS_OK)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS,
                        "Execution of DeflateQpl software fallback codec failed. "
                        "(Details: qpl_execute_job with error code: "
                        "{} - please refer to qpl_status in ./contrib/qpl/include/qpl/c_api/status.h)",
                        static_cast<UInt32>(status));
}

CompressionCodecDeflateQpl::CompressionCodecDeflateQpl()
    : sw_codec(std::make_unique<SoftwareCodecDeflateQpl>())
    , hw_codec(std::make_unique<HardwareCodecDeflateQpl>(*sw_codec))
{
    setCodecDescription("DEFLATE_QPL");
}

uint8_t CompressionCodecDeflateQpl::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::DeflateQpl);
}

void CompressionCodecDeflateQpl::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
}

UInt32 CompressionCodecDeflateQpl::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    /// Aligned with ZLIB
    return ((uncompressed_size) + ((uncompressed_size) >> 12) + ((uncompressed_size) >> 14) + ((uncompressed_size) >> 25) + 13);
}

UInt32 CompressionCodecDeflateQpl::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
/// QPL library is using AVX-512 with some shuffle operations.
/// Memory sanitizer don't understand if there was uninitialized memory in SIMD register but it was not used in the result of shuffle.
    __msan_unpoison(dest, getMaxCompressedDataSize(source_size));
    Int32 res = HardwareCodecDeflateQpl::RET_ERROR;
    if (DeflateQplJobHWPool::instance().isJobPoolReady())
        res = hw_codec->doCompressData(source, source_size, dest, getMaxCompressedDataSize(source_size));
    if (res == HardwareCodecDeflateQpl::RET_ERROR)
        res = sw_codec->doCompressData(source, source_size, dest, getMaxCompressedDataSize(source_size));
    return res;
}

inline void touchBufferWithZeroFilling(char * buffer, UInt32 buffer_size)
{
    for (char * p = buffer; p < buffer + buffer_size; p += ::getPageSize()/(sizeof(*p)))
    {
        *p = 0;
    }
}

void CompressionCodecDeflateQpl::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
/// QPL library is using AVX-512 with some shuffle operations.
/// Memory sanitizer don't understand if there was uninitialized memory in SIMD register but it was not used in the result of shuffle.
    __msan_unpoison(dest, uncompressed_size);
/// Device IOTLB miss has big perf. impact for IAA accelerators.
/// To avoid page fault, we need touch buffers related to accelerator in advance.
    touchBufferWithZeroFilling(dest, uncompressed_size);

    switch (getDecompressMode())
    {
        case CodecMode::Synchronous:
        {
            Int32 res = HardwareCodecDeflateQpl::RET_ERROR;
            if (DeflateQplJobHWPool::instance().isJobPoolReady())
            {
                res = hw_codec->doDecompressDataSynchronous(source, source_size, dest, uncompressed_size);
                if (res == HardwareCodecDeflateQpl::RET_ERROR)
                    sw_codec->doDecompressData(source, source_size, dest, uncompressed_size);
            }
            else
                sw_codec->doDecompressData(source, source_size, dest, uncompressed_size);
            return;
        }
        case CodecMode::Asynchronous:
        {
            Int32 res = HardwareCodecDeflateQpl::RET_ERROR;
            if (DeflateQplJobHWPool::instance().isJobPoolReady())
                res = hw_codec->doDecompressDataAsynchronous(source, source_size, dest, uncompressed_size);
            if (res == HardwareCodecDeflateQpl::RET_ERROR)
                sw_codec->doDecompressData(source, source_size, dest, uncompressed_size);
            return;
        }
        case CodecMode::SoftwareFallback:
            sw_codec->doDecompressData(source, source_size, dest, uncompressed_size);
            return;
    }
}

void CompressionCodecDeflateQpl::flushAsynchronousDecompressRequests()
{
    if (DeflateQplJobHWPool::instance().isJobPoolReady())
        hw_codec->flushAsynchronousDecompressRequests();
    /// After flush previous all async requests, we must restore mode to be synchronous by default.
    setDecompressMode(CodecMode::Synchronous);
}
void registerCodecDeflateQpl(CompressionCodecFactory & factory)
{
    factory.registerSimpleCompressionCodec(
        "DEFLATE_QPL", static_cast<char>(CompressionMethodByte::DeflateQpl), [&]() { return std::make_shared<CompressionCodecDeflateQpl>(); });
}
}
#endif
