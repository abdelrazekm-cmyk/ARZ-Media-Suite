#include "media/VideoDecoder.hpp"
#include "cache/ThumbnailCache.hpp"
#include "core/AERConfig.hpp"
#include "core/AERTelemetry.hpp"
#include "media/GpuDeviceManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace aer {

std::atomic<uint32_t> VideoDecoder::s_activeGpuStreams_{0};

namespace {

enum AVPixelFormat selectHwFormatCallback(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts) {
    const int wanted = *reinterpret_cast<const int*>(ctx->opaque);
    for (const enum AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (static_cast<int>(*p) == wanted) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

void copyRows(uint8_t* dst,
              int dstRowBytes,
              const uint8_t* src,
              int srcRowBytes,
              int rowCopyBytes,
              int height,
              bool flipVertical) {
    const ptrdiff_t signedDstStride = static_cast<ptrdiff_t>(dstRowBytes);
    if (!flipVertical && signedDstStride == srcRowBytes && srcRowBytes == rowCopyBytes) {
        std::memcpy(dst, src, static_cast<size_t>(rowCopyBytes) * static_cast<size_t>(height));
        return;
    }
    for (int y = 0; y < height; ++y) {
        const int dstY = flipVertical ? (height - 1 - y) : y;
        std::memcpy(dst + static_cast<ptrdiff_t>(dstY) * signedDstStride,
                    src + static_cast<size_t>(y) * static_cast<size_t>(srcRowBytes),
                    static_cast<size_t>(rowCopyBytes));
    }
}

std::vector<AVHWDeviceType> buildPreferredHwBackendChain() {
    const std::string& pref = AERConfig::instance().preferredGpuBackend;
    if (pref == "d3d11va") return {AV_HWDEVICE_TYPE_D3D11VA};
    if (pref == "cuda" || pref == "nvdec") return {AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_D3D11VA};
    if (pref == "vulkan") return {AV_HWDEVICE_TYPE_VULKAN, AV_HWDEVICE_TYPE_D3D11VA};
    if (pref == "d3d12va") return {AV_HWDEVICE_TYPE_D3D12VA, AV_HWDEVICE_TYPE_D3D11VA};
    return {
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_VULKAN,
        AV_HWDEVICE_TYPE_D3D12VA
    };
}

} // namespace

VideoDecoder::VideoDecoder(uint64_t sessionId, std::wstring filePath)
    : sessionId_(sessionId), filePath_(std::move(filePath)) {}

VideoDecoder::~VideoDecoder() {
    close();
}

uint32_t VideoDecoder::activeHardwareStreamCount() noexcept {
    return s_activeGpuStreams_.load(std::memory_order_relaxed);
}

void VideoDecoder::trackGpuActiveLocked(bool nowActive) {
    if (nowActive && !countedInGpuActive_) {
        s_activeGpuStreams_.fetch_add(1, std::memory_order_acq_rel);
        countedInGpuActive_ = true;
    } else if (!nowActive && countedInGpuActive_) {
        s_activeGpuStreams_.fetch_sub(1, std::memory_order_acq_rel);
        countedInGpuActive_ = false;
    }
}

bool VideoDecoder::hasDynamicVramHeadroomLocked() const {
    auto& gpuMgr = GpuDeviceManager::instance();
    if (!gpuMgr.ensureInitialized()) {
        return true;
    }
    constexpr uint64_t kDecoderPoolEstimate = 128ULL * 1024ULL * 1024ULL;
    const VramStatus vst = gpuMgr.queryVramStatus(LRUFrameCache::instance().totalVramBytes());
    if (!vst.valid) {
        return true;
    }
    return vst.hasHeadroomFor(kDecoderPoolEstimate, AERConfig::instance().maxPercentVramBudget);
}

void VideoDecoder::publishTelemetryLocked(const std::string& stateOverride) {
    StreamGpuTelemetry t;
    t.sessionId = sessionId_;
    t.filePathUtf8 = DemuxEngine::wideToUtf8(filePath_);
    t.videoCodec = meta_.videoCodecName;
    t.width = meta_.width;
    t.height = meta_.height;
    t.bitDepth = meta_.bitDepth;
    t.usingHardware = usingHardware_;
    t.selectedBackend = backendName_;
    t.state = !stateOverride.empty()
                  ? stateOverride
                  : (usingHardware_ ? "gpu_active" : "cpu_active");
    t.reasonDetail = fallbackReason_;
    AERTelemetry::instance().updateSession(t, activeHardwareStreamCount());
}

void VideoDecoder::resetScalerLocked() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    swsSrcW_ = swsSrcH_ = swsDstW_ = swsDstH_ = 0;
    swsSrcFmt_ = swsDstFmt_ = -1;
}

void VideoDecoder::quietHardwareSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!usingHardware_ || !fmtCtx_) {
        return;
    }
    const VramStatus vst = GpuDeviceManager::instance().queryVramStatus(
        LRUFrameCache::instance().totalVramBytes());
    if (vst.valid && vst.currentUsageBytes <= vst.effectiveCeilingBytes(AERConfig::instance().maxPercentVramBudget)) {
        return;
    }
    if (fallbackToSoftwareMidStreamLocked("vram_pressure_parked_idle_stream")) {
        quietedFromHardware_ = true;
        publishTelemetryLocked("quieted_vram_pressure");
    }
}

void VideoDecoder::close() {
    bumpScrubGeneration();
    std::lock_guard<std::mutex> lock(mutex_);
    resetScalerLocked();
    if (rawFrame_) av_frame_free(&rawFrame_);
    if (swTransferFrame_) av_frame_free(&swTransferFrame_);
    if (pkt_) av_packet_free(&pkt_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
    if (hwDeviceCtx_) av_buffer_unref(&hwDeviceCtx_);
    if (fmtCtx_) avformat_close_input(&fmtCtx_);
    trackGpuActiveLocked(false);
    usingHardware_ = false;
    usingSharedD3D11_ = false;
    quietedFromHardware_ = false;
    eofReached_ = false;
    currentPts_ = AV_NOPTS_VALUE;
    currentFrameNumber_ = -1;
    AERTelemetry::instance().removeSession(sessionId_, activeHardwareStreamCount());
}

int64_t VideoDecoder::nearestKeyframeFrame(int64_t frameNumber) const {
    const int64_t clamped = std::clamp<int64_t>(
        frameNumber, 0, std::max<int64_t>(0, meta_.totalFrames - 1));
    const int64_t targetPts = seekIndex_.frameToTargetPts(clamped);
    if (auto anchorPts = seekIndex_.nearestKeyframePtsBefore(targetPts, /*maxLookbackFrames=*/600)) {
        return std::clamp<int64_t>(
            seekIndex_.ptsToApproxFrame(*anchorPts),
            0,
            std::max<int64_t>(0, meta_.totalFrames - 1));
    }
    return clamped;
}

bool VideoDecoder::tryOpenCodecLocked(bool allowHardware) {
    if (codecCtx_) avcodec_free_context(&codecCtx_);
    if (hwDeviceCtx_) av_buffer_unref(&hwDeviceCtx_);
    trackGpuActiveLocked(false);
    usingHardware_ = false;
    usingSharedD3D11_ = false;
    hwPixFmt_ = AV_PIX_FMT_NONE;

    AVStream* st = fmtCtx_->streams[meta_.videoStreamIndex];
    const AVCodecParameters* par = st->codecpar;
    const AVCodec* decoder = nullptr;
    std::string chosenHwTypeName;

    const bool isStillPicture = (meta_.totalFrames <= 1);
    const bool preserveAlphaInSw = meta_.hasAlpha;
    const bool hwEligibleCodec =
        !preserveAlphaInSw &&
        (par->codec_id == AV_CODEC_ID_AV1 ||
         par->codec_id == AV_CODEC_ID_HEVC ||
         par->codec_id == AV_CODEC_ID_H264 ||
         par->codec_id == AV_CODEC_ID_VP9 ||
         par->codec_id == AV_CODEC_ID_VP8 ||
         par->codec_id == AV_CODEC_ID_VC1 ||
         par->codec_id == AV_CODEC_ID_WMV3 ||
         par->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
         par->codec_id == AV_CODEC_ID_MJPEG);

    if (allowHardware && AERConfig::instance().enableHardwareDecode && hwEligibleCodec && !isStillPicture) {
        if (!hasDynamicVramHeadroomLocked()) {
            fallbackReason_ = "vram_full_spilled_to_ram_cpu";
        } else {
            const AVCodec* hwDecCandidate = nullptr;
            if (par->codec_id == AV_CODEC_ID_AV1) {
                hwDecCandidate = avcodec_find_decoder_by_name("av1");
            } else if (par->codec_id == AV_CODEC_ID_VP9) {
                hwDecCandidate = avcodec_find_decoder_by_name("vp9");
            } else if (par->codec_id == AV_CODEC_ID_VP8) {
                hwDecCandidate = avcodec_find_decoder_by_name("vp8");
            }
            if (!hwDecCandidate) {
                hwDecCandidate = avcodec_find_decoder(par->codec_id);
            }

            if (hwDecCandidate) {
                for (AVHWDeviceType devType : buildPreferredHwBackendChain()) {
                    int candidatePixFmt = AV_PIX_FMT_NONE;
                    for (int i = 0;; ++i) {
                        const AVCodecHWConfig* cfg = avcodec_get_hw_config(hwDecCandidate, i);
                        if (!cfg) break;
                        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
                            cfg->device_type == devType) {
                            candidatePixFmt = static_cast<int>(cfg->pix_fmt);
                            break;
                        }
                    }
                    if (candidatePixFmt == AV_PIX_FMT_NONE) {
                        continue;
                    }

                    if (devType == AV_HWDEVICE_TYPE_D3D11VA) {
                        hwDeviceCtx_ = GpuDeviceManager::instance().acquireSharedD3D11HwDeviceRef();
                        if (hwDeviceCtx_) {
                            usingSharedD3D11_ = true;
                        }
                    }
                    if (!hwDeviceCtx_) {
                        av_hwdevice_ctx_create(&hwDeviceCtx_, devType, nullptr, nullptr, 0);
                    }

                    if (hwDeviceCtx_) {
                        decoder = hwDecCandidate;
                        hwPixFmt_ = candidatePixFmt;
                        const char* tn = av_hwdevice_get_type_name(devType);
                        chosenHwTypeName = tn ? tn : "gpu";
                        break;
                    }
                }
            }

            if (!hwDeviceCtx_) {
                fallbackReason_ = "no_compatible_gpu_backend";
            }
        }
    } else if (preserveAlphaInSw) {
        fallbackReason_ = "alpha_channel_preserved_multithread";
    } else if (!AERConfig::instance().enableHardwareDecode) {
        fallbackReason_ = "gpu_disabled_by_config";
    } else if (isStillPicture) {
        fallbackReason_ = "still_image_cpu_fast_path";
    }

    if (!hwDeviceCtx_) {
        if (par->codec_id == AV_CODEC_ID_AV1) {
            decoder = avcodec_find_decoder_by_name("libdav1d");
        } else if (par->codec_id == AV_CODEC_ID_VP9) {
            decoder = avcodec_find_decoder_by_name("libvpx-vp9");
        } else if (par->codec_id == AV_CODEC_ID_VP8) {
            decoder = avcodec_find_decoder_by_name("libvpx");
        }
        if (!decoder) {
            decoder = avcodec_find_decoder(par->codec_id);
        }
    }

    if (!decoder) {
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(decoder);
    if (!codecCtx_) {
        return false;
    }

    if (avcodec_parameters_to_context(codecCtx_, par) < 0) {
        avcodec_free_context(&codecCtx_);
        return false;
    }
    codecCtx_->pkt_timebase = st->time_base;

    AVDictionary* opts = nullptr;
    if (hwDeviceCtx_ && hwPixFmt_ != AV_PIX_FMT_NONE) {
        codecCtx_->opaque = &hwPixFmt_;
        codecCtx_->get_format = selectHwFormatCallback;
        codecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        usingHardware_ = true;
        fallbackReason_ = "ok";
    } else if (AERConfig::instance().enableMultithreading) {
        const int hwThreads = std::clamp(av_cpu_count(), 2, 16);
        codecCtx_->thread_count = hwThreads;
        codecCtx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (std::string(decoder->name) == "libdav1d") {
            av_dict_set_int(&opts, "threads", hwThreads, 0);
            av_dict_set_int(&opts, "tilethreads", std::clamp(hwThreads / 2, 1, 4), 0);
            av_dict_set_int(&opts, "framethreads", std::clamp(hwThreads / 2, 1, 4), 0);
        }
    } else {
        codecCtx_->thread_count = 1;
    }

    const int openRes = avcodec_open2(codecCtx_, decoder, &opts);
    av_dict_free(&opts);
    if (openRes < 0) {
        avcodec_free_context(&codecCtx_);
        if (hwDeviceCtx_) av_buffer_unref(&hwDeviceCtx_);
        usingHardware_ = false;
        usingSharedD3D11_ = false;
        return false;
    }

    trackGpuActiveLocked(usingHardware_);
    if (usingHardware_) {
        const std::string& gpuName = GpuDeviceManager::instance().adapterName();
        backendName_ = std::string(decoder->name) + " [" + chosenHwTypeName + " @ " + gpuName + "]";
    } else {
        backendName_ = std::string(decoder->name);
    }
    return true;
}

bool VideoDecoder::fallbackToSoftwareMidStreamLocked(const std::string& reason) {
    resetScalerLocked();
    if (!tryOpenCodecLocked(/*allowHardware=*/false)) {
        return false;
    }
    fallbackReason_ = reason;
    eofReached_ = false;
    currentPts_ = AV_NOPTS_VALUE;
    currentFrameNumber_ = -1;
    publishTelemetryLocked("fell_back_mid_stream");
    return true;
}

bool VideoDecoder::open(AVFormatContext* adoptedVideoCtx, MediaMetadata& ioMeta) {
    std::lock_guard<std::mutex> lock(mutex_);
    fmtCtx_ = adoptedVideoCtx;
    meta_ = ioMeta;
    if (!fmtCtx_ || !meta_.hasVideo || meta_.videoStreamIndex < 0) {
        return false;
    }

    pkt_ = av_packet_alloc();
    rawFrame_ = av_frame_alloc();
    swTransferFrame_ = av_frame_alloc();
    if (!pkt_ || !rawFrame_ || !swTransferFrame_) {
        return false;
    }

    AVStream* st = fmtCtx_->streams[meta_.videoStreamIndex];
    if (AERConfig::instance().enableSeekingTable) {
        seekIndex_.initializeFromStream(
            st,
            meta_.videoStartPts,
            meta_.videoFrameStepPts,
            meta_.fpsNum,
            meta_.fpsDen,
            meta_.vidTimebaseNum,
            meta_.vidTimebaseDen);
    }

    if (!tryOpenCodecLocked(/*allowHardware=*/true)) {
        if (!tryOpenCodecLocked(/*allowHardware=*/false)) {
            return false;
        }
    }

    int64_t firstPts = AV_NOPTS_VALUE;
    bool firstKey = true;
    AVFrame* probed = av_frame_alloc();
    bool probeOk = decodeNextRawOrSwFrameLocked(probed, firstPts, firstKey);
    if (!probeOk && usingHardware_) {
        avformat_seek_file(fmtCtx_, meta_.videoStreamIndex, INT64_MIN, meta_.videoStartPts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
        eofReached_ = false;
        if (tryOpenCodecLocked(/*allowHardware=*/false)) {
            fallbackReason_ = "hw_profile_or_chroma_unsupported";
            probeOk = decodeNextRawOrSwFrameLocked(probed, firstPts, firstKey);
        }
    }

    if (!probeOk) {
        av_frame_free(&probed);
        return false;
    }

    if (probed->format != hwPixFmt_) {
        if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(probed->format))) {
            if (desc->nb_components > 0 && desc->comp[0].depth > 0) {
                ioMeta.bitDepth = desc->comp[0].depth;
                meta_.bitDepth = desc->comp[0].depth;
            }
            if ((desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0) {
                ioMeta.hasAlpha = true;
                meta_.hasAlpha = true;
            }
        }
    }
    ioMeta.isInterlaced = (probed->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    ioMeta.topFieldFirst = (probed->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
    if (firstPts != AV_NOPTS_VALUE && (st->start_time == AV_NOPTS_VALUE || firstPts < meta_.videoStartPts)) {
        ioMeta.videoStartPts = firstPts;
        meta_.videoStartPts = firstPts;
        if (AERConfig::instance().enableSeekingTable) {
            seekIndex_.initializeFromStream(
                st,
                meta_.videoStartPts,
                meta_.videoFrameStepPts,
                meta_.fpsNum,
                meta_.fpsDen,
                meta_.vidTimebaseNum,
                meta_.vidTimebaseDen);
        }
    }
    meta_ = ioMeta;

    currentPts_ = firstPts;
    currentFrameNumber_ = 0;
    seekIndex_.recordObservedFrame(0, firstPts, true);
    publishTelemetryLocked();

    materializeAndCacheFrameLocked(probed, 0, meta_.width, meta_.height,
                                   meta_.width * 4, PixelBufferFormat::BGRA_8u, nullptr, false);

    av_frame_free(&probed);
    return true;
}

bool VideoDecoder::decodeNextRawOrSwFrameLocked(AVFrame* outDecodedFrame, int64_t& outPts, bool& outIsKey) {
    av_frame_unref(outDecodedFrame);
    if (usingHardware_ && forceSimulateHwFailure_) {
        forceSimulateHwFailure_ = false;
        return false;
    }

    while (true) {
        int ret = avcodec_receive_frame(codecCtx_, rawFrame_);
        if (ret == 0) {
            const bool isKey = (rawFrame_->flags & AV_FRAME_FLAG_KEY) != 0;
            int64_t pts = rawFrame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = rawFrame_->pts;

            if (av_frame_ref(outDecodedFrame, rawFrame_) < 0) {
                av_frame_unref(rawFrame_);
                return false;
            }
            av_frame_unref(rawFrame_);
            outPts = pts;
            outIsKey = isKey;
            return true;
        }

        if (ret == AVERROR_EOF) {
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            return false;
        }

        if (eofReached_) {
            avcodec_send_packet(codecCtx_, nullptr);
            continue;
        }

        const int readRet = av_read_frame(fmtCtx_, pkt_);
        if (readRet < 0) {
            eofReached_ = true;
            avcodec_send_packet(codecCtx_, nullptr);
            continue;
        }

        if (pkt_->stream_index == meta_.videoStreamIndex) {
            const int sendRet = avcodec_send_packet(codecCtx_, pkt_);
            av_packet_unref(pkt_);
            if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
                return false;
            }
        } else {
            av_packet_unref(pkt_);
        }
    }
}

bool VideoDecoder::materializeAndCacheFrameLocked(const AVFrame* decodedFrame,
                                                  int64_t frameNumber,
                                                  int outWidth,
                                                  int outHeight,
                                                  int outRowBytes,
                                                  PixelBufferFormat outFormat,
                                                  uint8_t* optionalCpuDst,
                                                  bool flipVertical) {
    if (!decodedFrame) return false;
    const FrameCacheKey cacheKey{sessionId_, frameNumber, outWidth, outHeight, outFormat};

    // FAST PATH 1: 100% GPU Hardware Color Conversion (NV12 / 10-bit P010 -> VRAM BGRA8 Texture)
    if (!meta_.hasAlpha && usingHardware_ && usingSharedD3D11_ && decodedFrame->format == AV_PIX_FMT_D3D11 &&
        outFormat == PixelBufferFormat::BGRA_8u && AERConfig::instance().enableGpuColorConvert) {
        auto vramTex = GpuDeviceManager::instance().convertHwFrameToVramBgra8(
            decodedFrame, outWidth, outHeight);
        if (vramTex) {
            bool ok = true;
            if (optionalCpuDst) {
                ok = GpuDeviceManager::instance().readbackVramTextureToCpu(
                    vramTex.Get(), outWidth, outHeight, optionalCpuDst, outRowBytes, flipVertical);
                if (ok && !flipVertical && outWidth <= 640 && outHeight <= 640 && outRowBytes == outWidth * 4) {
                    ThumbnailCache::storeBGRA8(
                        filePath_, outWidth, outHeight, frameNumber,
                        optionalCpuDst, static_cast<size_t>(outRowBytes) * static_cast<size_t>(outHeight));
                }
            }
            LRUFrameCache::instance().putVramTexture(cacheKey, std::move(vramTex));
            return ok;
        }
    }

    // PATH 2: Software / Alpha / 32-bit float conversion -> RAM cache (always stored top-down in cache)
    const AVFrame* swSrc = decodedFrame;
    if (usingHardware_ && decodedFrame->format == hwPixFmt_) {
        av_frame_unref(swTransferFrame_);
        if (av_hwframe_transfer_data(swTransferFrame_, decodedFrame, 0) < 0) {
            return false;
        }
        swTransferFrame_->colorspace = decodedFrame->colorspace;
        swTransferFrame_->color_range = decodedFrame->color_range;
        swSrc = swTransferFrame_;
    }

    const int tightRowBytes = outWidth * (outFormat == PixelBufferFormat::BGRA_32f ? 16 : 4);
    auto ramCached = std::make_shared<CachedVideoFrame>();
    ramCached->frameNumber = frameNumber;
    ramCached->width = outWidth;
    ramCached->height = outHeight;
    ramCached->rowBytes = tightRowBytes;
    ramCached->format = outFormat;
    ramCached->pixels.resize(static_cast<size_t>(tightRowBytes) * static_cast<size_t>(outHeight));

    if (!convertSwAVFrameToTargetLocked(swSrc, outWidth, outHeight, tightRowBytes,
                                        outFormat, ramCached->pixels.data())) {
        return false;
    }

    if (optionalCpuDst) {
        copyRows(optionalCpuDst, outRowBytes, ramCached->pixels.data(),
                 tightRowBytes, tightRowBytes, outHeight, flipVertical);
    }
    if (outFormat == PixelBufferFormat::BGRA_8u && outWidth <= 640 && outHeight <= 640) {
        ThumbnailCache::storeBGRA8(
            filePath_, outWidth, outHeight, frameNumber,
            ramCached->pixels.data(), ramCached->pixels.size());
    }
    LRUFrameCache::instance().putRamFrame(cacheKey, std::move(ramCached));
    return true;
}

bool VideoDecoder::convertSwAVFrameToTargetLocked(const AVFrame* src,
                                                  int outWidth,
                                                  int outHeight,
                                                  int outRowBytes,
                                                  PixelBufferFormat outFormat,
                                                  uint8_t* dstBuffer) {
    if (!src || !dstBuffer || outWidth <= 0 || outHeight <= 0) {
        return false;
    }

    const AVPixelFormat targetAvFmt =
        (outFormat == PixelBufferFormat::BGRA_32f) ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_BGRA;

    if (!swsCtx_ || swsSrcW_ != src->width || swsSrcH_ != src->height || swsSrcFmt_ != src->format ||
        swsDstW_ != outWidth || swsDstH_ != outHeight || swsDstFmt_ != static_cast<int>(targetAvFmt)) {
        resetScalerLocked();
        swsCtx_ = sws_alloc_context();
        if (!swsCtx_) return false;

        av_opt_set_int(swsCtx_, "srcw", src->width, 0);
        av_opt_set_int(swsCtx_, "srch", src->height, 0);
        av_opt_set_int(swsCtx_, "src_format", src->format, 0);
        av_opt_set_int(swsCtx_, "dstw", outWidth, 0);
        av_opt_set_int(swsCtx_, "dsth", outHeight, 0);
        av_opt_set_int(swsCtx_, "dst_format", targetAvFmt, 0);
        av_opt_set_int(swsCtx_, "sws_flags", SWS_BILINEAR, 0);
        if (AERConfig::instance().enableMultithreading) {
            av_opt_set_int(swsCtx_, "threads", std::clamp(av_cpu_count() / 2, 1, 8), 0);
        }
        if (sws_init_context(swsCtx_, nullptr, nullptr) < 0) {
            resetScalerLocked();
            return false;
        }

        int csCoeff = SWS_CS_ITU709;
        if (src->colorspace == AVCOL_SPC_BT2020_NCL || src->colorspace == AVCOL_SPC_BT2020_CL || meta_.isBt2020) {
            csCoeff = SWS_CS_BT2020;
        } else if (src->colorspace == AVCOL_SPC_SMPTE170M || src->colorspace == AVCOL_SPC_BT470BG || meta_.isBt601) {
            csCoeff = SWS_CS_ITU601;
        }
        const int srcRange = (src->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
        const int* coeffs = sws_getCoefficients(csCoeff);
        sws_setColorspaceDetails(swsCtx_, coeffs, srcRange, coeffs, 1, 0, 1 << 16, 1 << 16);

        swsSrcW_ = src->width;
        swsSrcH_ = src->height;
        swsSrcFmt_ = src->format;
        swsDstW_ = outWidth;
        swsDstH_ = outHeight;
        swsDstFmt_ = static_cast<int>(targetAvFmt);
    }

    if (outFormat == PixelBufferFormat::BGRA_8u) {
        uint8_t* dstPlanes[4] = {dstBuffer, nullptr, nullptr, nullptr};
        int dstStrides[4] = {outRowBytes, 0, 0, 0};
        return sws_scale(swsCtx_, src->data, src->linesize, 0, src->height, dstPlanes, dstStrides) > 0;
    }

    const size_t pixelCount = static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight);
    tempRgba64_.resize(pixelCount * 4ULL);
    uint8_t* dstPlanes[4] = {reinterpret_cast<uint8_t*>(tempRgba64_.data()), nullptr, nullptr, nullptr};
    int dstStrides[4] = {outWidth * 8, 0, 0, 0};
    if (sws_scale(swsCtx_, src->data, src->linesize, 0, src->height, dstPlanes, dstStrides) <= 0) {
        return false;
    }

    constexpr float kInv65535 = 1.0f / 65535.0f;
    for (int y = 0; y < outHeight; ++y) {
        const uint16_t* srcRow = tempRgba64_.data() + static_cast<size_t>(y) * static_cast<size_t>(outWidth) * 4ULL;
        float* dstRow = reinterpret_cast<float*>(dstBuffer + static_cast<size_t>(y) * static_cast<size_t>(outRowBytes));
        for (int x = 0; x < outWidth; ++x) {
            const uint16_t r = srcRow[x * 4 + 0];
            const uint16_t g = srcRow[x * 4 + 1];
            const uint16_t b = srcRow[x * 4 + 2];
            const uint16_t a = srcRow[x * 4 + 3];
            dstRow[x * 4 + 0] = static_cast<float>(b) * kInv65535;
            dstRow[x * 4 + 1] = static_cast<float>(g) * kInv65535;
            dstRow[x * 4 + 2] = static_cast<float>(r) * kInv65535;
            dstRow[x * 4 + 3] = static_cast<float>(a) * kInv65535;
        }
    }
    return true;
}

bool VideoDecoder::decodeFrame(int64_t frameNumber,
                               int outWidth,
                               int outHeight,
                               int outRowBytes,
                               PixelBufferFormat outFormat,
                               uint8_t* outBuffer,
                               bool flipVertical,
                               bool fastKeyframeScrub,
                               uint64_t expectedGeneration) {
    if (!outBuffer || outWidth <= 0 || outHeight <= 0 || outRowBytes == 0) {
        return false;
    }

    const int64_t clampedFrame = std::clamp<int64_t>(
        frameNumber, 0, std::max<int64_t>(0, meta_.totalFrames - 1));
    const int tightRowBytes = outWidth * (outFormat == PixelBufferFormat::BGRA_32f ? 16 : 4);

    const FrameCacheKey cacheKey{sessionId_, clampedFrame, outWidth, outHeight, outFormat};
    if (LRUFrameCache::instance().tryReadCachedFrame(cacheKey, outBuffer, outRowBytes, flipVertical)) {
        return true;
    }

    if (outFormat == PixelBufferFormat::BGRA_8u && outWidth <= 640 && outHeight <= 640) {
        std::vector<uint8_t> thumbPixels;
        if (ThumbnailCache::tryLoadBGRA8(filePath_, outWidth, outHeight, clampedFrame, thumbPixels)) {
            copyRows(outBuffer, outRowBytes, thumbPixels.data(), tightRowBytes, tightRowBytes, outHeight, flipVertical);
            return true;
        }
    }

    if (expectedGeneration != 0 && expectedGeneration != currentScrubGeneration()) {
        return false; // Already superseded by a newer mouse scrub frame!
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!fmtCtx_ || !codecCtx_) {
        return false;
    }

    if (LRUFrameCache::instance().tryReadCachedFrame(cacheKey, outBuffer, outRowBytes, flipVertical)) {
        return true;
    }

    if (expectedGeneration != 0 && expectedGeneration != currentScrubGeneration()) {
        return false;
    }

    if (!usingHardware_ && quietedFromHardware_ && hasDynamicVramHeadroomLocked()) {
        if (tryOpenCodecLocked(/*allowHardware=*/true)) {
            quietedFromHardware_ = false;
            currentPts_ = AV_NOPTS_VALUE;
            currentFrameNumber_ = -1;
            publishTelemetryLocked("gpu_active");
        }
    }

    // Detect rapid mouse-drag timeline scrubbing (> 3 non-sequential jumps within 180 ms)
    const auto now = std::chrono::steady_clock::now();
    const bool isSequentialOrReverseStep =
        (lastRequestedFrame_ >= 0 && std::abs(clampedFrame - lastRequestedFrame_) <= 8);
    if (!isSequentialOrReverseStep) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastJumpTime_).count();
        if (elapsedMs >= 0 && elapsedMs < 140) {
            ++rapidJumpStreak_;
        } else {
            rapidJumpStreak_ = 0;
        }
        lastJumpTime_ = now;
    } else {
        rapidJumpStreak_ = 0;
    }
    const bool isReverseStep = (lastRequestedFrame_ >= 0 && clampedFrame < lastRequestedFrame_ &&
                                (lastRequestedFrame_ - clampedFrame) <= 24);
    lastRequestedFrame_ = clampedFrame;

    // DaVinci Resolve-style Fast Scrub: when dragging rapidly across Long-GOP clips,
    // snap intermediate drag previews to the nearest keyframe (~4 ms!) so the viewer never lags!
    const bool useFastKeyPreview = fastKeyframeScrub || (rapidJumpStreak_ >= 3);

    AVStream* st = fmtCtx_->streams[meta_.videoStreamIndex];
    const int64_t targetPts = seekIndex_.frameToTargetPts(clampedFrame);
    const int64_t halfStep = std::max<int64_t>(1, meta_.videoFrameStepPts / 2);

    AVFrame* decoded = av_frame_alloc();
    AVFrame* lastValid = av_frame_alloc();
    int64_t deliveredFrameNum = clampedFrame;

    bool haveLastValid = false;
    for (int attempt = 0; attempt < 2 && !haveLastValid; ++attempt) {
        const bool canStepForward =
            (attempt == 0) && !useFastKeyPreview &&
            ((currentFrameNumber_ >= 0 &&
              clampedFrame > currentFrameNumber_ &&
              (clampedFrame - currentFrameNumber_) <= 24) ||
             seekIndex_.shouldDecodeForward(currentPts_, targetPts, 24));

        if (!canStepForward) {
            int64_t seekPts = targetPts;
            if (auto anchor = seekIndex_.nearestKeyframePtsBefore(targetPts, /*maxLookbackFrames=*/300)) {
                seekPts = *anchor;
            }
            if (avformat_seek_file(fmtCtx_, meta_.videoStreamIndex, INT64_MIN, seekPts, seekPts, AVSEEK_FLAG_BACKWARD) < 0) {
                av_seek_frame(fmtCtx_, meta_.videoStreamIndex, seekPts, AVSEEK_FLAG_BACKWARD);
            }
            if (AERConfig::instance().enableSeekingTable) {
                seekIndex_.refreshFromStream(st);
            }
            avcodec_flush_buffers(codecCtx_);
            eofReached_ = false;
            currentPts_ = AV_NOPTS_VALUE;
            currentFrameNumber_ = -1;
        }

        int decodedInBurst = 0;
        constexpr int kMaxGOPDecodeFrames = 450;
        while (decodedInBurst < kMaxGOPDecodeFrames) {
            if (expectedGeneration != 0 && expectedGeneration != currentScrubGeneration()) {
                break; // Immediately abort stale Long-GOP decode when playhead moved!
            }

            int64_t pts = AV_NOPTS_VALUE;
            bool isKey = false;
            if (!decodeNextRawOrSwFrameLocked(decoded, pts, isKey)) {
                break;
            }
            ++decodedInBurst;

            int64_t estFrame = clampedFrame;
            if (pts != AV_NOPTS_VALUE) {
                estFrame = seekIndex_.ptsToApproxFrame(pts);
            } else if (currentFrameNumber_ >= 0) {
                estFrame = currentFrameNumber_ + 1;
            }

            if (decodedInBurst == 1 && !useFastKeyPreview && (clampedFrame - estFrame) > 360) {
                if (av_seek_frame(fmtCtx_, meta_.videoStreamIndex, targetPts, AVSEEK_FLAG_BACKWARD) >= 0) {
                    if (AERConfig::instance().enableSeekingTable) {
                        seekIndex_.refreshFromStream(st);
                    }
                    avcodec_flush_buffers(codecCtx_);
                    eofReached_ = false;
                    continue;
                }
            }

            currentPts_ = pts;
            currentFrameNumber_ = estFrame;
            seekIndex_.recordObservedFrame(estFrame, pts, isKey);

            av_frame_unref(lastValid);
            av_frame_ref(lastValid, decoded);
            haveLastValid = true;

            if (useFastKeyPreview) {
                // Deliver this keyframe immediately (~4 ms) during high-speed mouse drag!
                deliveredFrameNum = estFrame;
                break;
            }

            // Pre-cache trailing frames ONLY during active reverse-scrubbing (never stall forward jumps!)
            if (isReverseStep && (clampedFrame - estFrame) > 0 && (clampedFrame - estFrame) <= 8 &&
                AERConfig::instance().enableFrameCache && outWidth >= (meta_.width / 2)) {
                materializeAndCacheFrameLocked(
                    decoded, estFrame, outWidth, outHeight, tightRowBytes, outFormat, nullptr, false);
            }

            if ((pts != AV_NOPTS_VALUE && pts + halfStep >= targetPts) || estFrame >= clampedFrame) {
                deliveredFrameNum = clampedFrame;
                break;
            }
        }

        if (!haveLastValid && usingHardware_ && attempt == 0) {
            if (!fallbackToSoftwareMidStreamLocked("mid_playback_gpu_decode_failure")) {
                break;
            }
        } else {
            break;
        }
    }

    bool ok = false;
    if (haveLastValid) {
        ok = materializeAndCacheFrameLocked(
            lastValid, deliveredFrameNum, outWidth, outHeight, outRowBytes, outFormat, outBuffer, flipVertical);
        publishTelemetryLocked();
    }

    av_frame_free(&decoded);
    av_frame_free(&lastValid);
    return ok;
}

} // namespace aer
