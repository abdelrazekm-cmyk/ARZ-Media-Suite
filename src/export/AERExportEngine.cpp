#include "export/AERExportEngine.hpp"
#include "media/DemuxEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/cpu.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace aer {

namespace {

const char* mapExtToFormatName(const std::string& ext) {
    if (ext == "mkv" || ext == "mk3d" || ext == "mka") return "matroska";
    if (ext == "webm" || ext == "weba") return "webm";
    if (ext == "mp4" || ext == "m4v" || ext == "m4a") return "mp4";
    if (ext == "mov" || ext == "qt") return "mov";
    if (ext == "avi") return "avi";
    if (ext == "gif") return "gif";
    if (ext == "webp") return "webp";
    if (ext == "apng") return "apng";
    if (ext == "opus" || ext == "ogg") return "ogg";
    if (ext == "flac") return "flac";
    if (ext == "wav") return "wav";
    if (ext == "mp3") return "mp3";
    if (ext == "aac") return "adts";
    if (ext == "aiff" || ext == "aif") return "aiff";
    if (ext == "ac3") return "ac3";
    if (ext == "eac3") return "eac3";
    return nullptr;
}

std::vector<const char*> videoEncoderCandidates(ExportVideoCodec c) {
    switch (c) {
        case ExportVideoCodec::AV1_NVENC:        return {"av1_nvenc", "libsvtav1", "libaom-av1"};
        case ExportVideoCodec::AV1_SVT:          return {"libsvtav1", "libaom-av1", "av1_nvenc"};
        case ExportVideoCodec::HEVC_NVENC:       return {"hevc_nvenc", "hevc_mf", "libkvazaar"};
        case ExportVideoCodec::H264_NVENC:       return {"h264_nvenc", "libopenh264", "h264_mf"};
        case ExportVideoCodec::H264_SW:          return {"libopenh264", "h264_nvenc"};
        case ExportVideoCodec::VP9:              return {"libvpx-vp9"};
        case ExportVideoCodec::PRORES_422_HQ:    return {"prores_ks", "prores_aw", "prores"};
        case ExportVideoCodec::PRORES_4444_ALPHA:return {"prores_ks", "prores"};
        case ExportVideoCodec::DNXHR_HQ:         return {"dnxhd"};
        case ExportVideoCodec::CINEFORM:         return {"cfhd"};
        case ExportVideoCodec::FFV1_LOSSLESS:    return {"ffv1"};
        case ExportVideoCodec::MJPEG:            return {"mjpeg"};
        case ExportVideoCodec::GIF:              return {"gif"};
        case ExportVideoCodec::WEBP_ANIM:        return {"libwebp_anim", "libwebp"};
        case ExportVideoCodec::APNG:             return {"apng"};
    }
    return {"libsvtav1"};
}

std::vector<const char*> audioEncoderCandidates(ExportAudioCodec c, const std::string& ext) {
    const bool bigEndianAiff = (ext == "aiff" || ext == "aif");
    switch (c) {
        case ExportAudioCodec::OPUS:    return {"libopus", "opus"};
        case ExportAudioCodec::FLAC:    return {"flac"};
        case ExportAudioCodec::AAC:     return {"aac", "aac_mf"};
        case ExportAudioCodec::VORBIS:  return {"libvorbis", "vorbis"};
        case ExportAudioCodec::MP3:     return {"libmp3lame", "mp3_mf"};
        case ExportAudioCodec::ALAC:    return {"alac"};
        case ExportAudioCodec::AC3:     return {"ac3", "ac3_mf"};
        case ExportAudioCodec::EAC3:    return {"eac3"};
        case ExportAudioCodec::PCM_S24: return bigEndianAiff ? std::vector<const char*>{"pcm_s24be"}
                                                             : std::vector<const char*>{"pcm_s24le"};
        case ExportAudioCodec::PCM_F32: return bigEndianAiff ? std::vector<const char*>{"pcm_f32be"}
                                                             : std::vector<const char*>{"pcm_f32le"};
    }
    return {"aac"};
}

AVPixelFormat selectSupportedPixFmt(const AVCodec* codec,
                                    ExportVideoCodec requestedCodec,
                                    ExportChromaBitDepth depthMode) {
    std::vector<AVPixelFormat> preferred;
    if (requestedCodec == ExportVideoCodec::PRORES_4444_ALPHA) {
        preferred = {AV_PIX_FMT_YUVA444P10LE, AV_PIX_FMT_YUV444P10LE};
    } else if (requestedCodec == ExportVideoCodec::PRORES_422_HQ ||
               requestedCodec == ExportVideoCodec::CINEFORM) {
        preferred = {AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV422P};
    } else if (requestedCodec == ExportVideoCodec::DNXHR_HQ) {
        if (depthMode == ExportChromaBitDepth::Bit10_422 || depthMode == ExportChromaBitDepth::Bit10_420_HDR) {
            preferred = {AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV422P};
        } else {
            preferred = {AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV422P10LE};
        }
    } else if (requestedCodec == ExportVideoCodec::GIF) {
        preferred = {AV_PIX_FMT_RGB8, AV_PIX_FMT_BGR8, AV_PIX_FMT_PAL8};
    } else if (requestedCodec == ExportVideoCodec::APNG) {
        preferred = {AV_PIX_FMT_RGBA, AV_PIX_FMT_RGB24};
    } else if (requestedCodec == ExportVideoCodec::WEBP_ANIM) {
        preferred = (depthMode == ExportChromaBitDepth::Alpha_4444)
                        ? std::vector<AVPixelFormat>{AV_PIX_FMT_BGRA, AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV420P}
                        : std::vector<AVPixelFormat>{AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGRA};
    } else if (requestedCodec == ExportVideoCodec::MJPEG) {
        preferred = {AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P};
    } else if (depthMode == ExportChromaBitDepth::Alpha_4444) {
        preferred = {AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVA444P10LE, AV_PIX_FMT_BGRA, AV_PIX_FMT_RGBA, AV_PIX_FMT_YUV420P};
    } else if (depthMode == ExportChromaBitDepth::Bit10_420_HDR) {
        preferred = {AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P};
    } else if (depthMode == ExportChromaBitDepth::Bit10_422) {
        preferred = {AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV420P};
    } else {
        preferred = {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P};
    }

    const AVPixelFormat* supported = nullptr;
    int numSupported = 0;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     reinterpret_cast<const void**>(&supported), &numSupported) >= 0 &&
        supported && numSupported > 0) {
        for (AVPixelFormat want : preferred) {
            for (int i = 0; i < numSupported; ++i) {
                if (supported[i] == want) return want;
            }
        }
        return supported[0];
    }
#endif
    return preferred.front();
}

AVSampleFormat selectSupportedSampleFmt(const AVCodec* codec) {
    const std::vector<AVSampleFormat> preferred = {
        AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32,
        AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16
    };
    const AVSampleFormat* supported = nullptr;
    int numSupported = 0;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     reinterpret_cast<const void**>(&supported), &numSupported) >= 0 &&
        supported && numSupported > 0) {
        for (AVSampleFormat want : preferred) {
            for (int i = 0; i < numSupported; ++i) {
                if (supported[i] == want) return want;
            }
        }
        return supported[0];
    }
#endif
    return AV_SAMPLE_FMT_FLTP;
}

int selectSupportedSampleRate(const AVCodec* codec, int desiredRate) {
    const int* supported = nullptr;
    int numSupported = 0;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0,
                                     reinterpret_cast<const void**>(&supported), &numSupported) >= 0 &&
        supported && numSupported > 0) {
        int best = supported[0];
        int bestDiff = std::abs(best - desiredRate);
        for (int i = 0; i < numSupported; ++i) {
            if (supported[i] <= 0) continue;
            if (supported[i] == desiredRate) return desiredRate;
            const int d = std::abs(supported[i] - desiredRate);
            if (d < bestDiff) {
                best = supported[i];
                bestDiff = d;
            }
        }
        return best;
    }
#endif
    return desiredRate;
}

} // namespace

AERExportEngine::AERExportEngine() = default;

AERExportEngine::~AERExportEngine() {
    abortAndClose();
}

void AERExportEngine::abortAndClose() {
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (swrCtx_) { swr_free(&swrCtx_); swrCtx_ = nullptr; }
    if (vidFrame_) av_frame_free(&vidFrame_);
    if (audFrame_) av_frame_free(&audFrame_);
    if (pkt_) av_packet_free(&pkt_);
    if (vidEncCtx_) avcodec_free_context(&vidEncCtx_);
    if (audEncCtx_) avcodec_free_context(&audEncCtx_);
    if (fmtCtx_) {
        if (fmtCtx_->pb && !(fmtCtx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmtCtx_->pb);
        }
        avformat_free_context(fmtCtx_);
        fmtCtx_ = nullptr;
    }
    vidStream_ = nullptr;
    audStream_ = nullptr;
    audioFifo_.clear();
    audioFifoSamples_ = 0;
    opened_ = false;
    headerWritten_ = false;
}

bool AERExportEngine::initVideoStream() {
    const auto candidates = videoEncoderCandidates(cfg_.videoCodec);
    const AVCodec* chosenCodec = nullptr;

    for (const char* encName : candidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(encName);
        if (!codec) continue;

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;

        // Ensure even dimensions for 4:2:0 / 4:2:2 YUV codecs
        const AVPixelFormat pixFmt = selectSupportedPixFmt(codec, cfg_.videoCodec, cfg_.bitDepthMode);
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt);
        int w = std::max(16, cfg_.width);
        int h = std::max(16, cfg_.height);
        if (desc && desc->log2_chroma_w > 0 && (w & 1)) w &= ~1;
        if (desc && desc->log2_chroma_h > 0 && (h & 1)) h &= ~1;

        ctx->width = w;
        ctx->height = h;
        ctx->pix_fmt = pixFmt;
        ctx->time_base = AVRational{static_cast<int>(cfg_.fpsDen), static_cast<int>(cfg_.fpsNum)};
        ctx->framerate = AVRational{static_cast<int>(cfg_.fpsNum), static_cast<int>(cfg_.fpsDen)};
        ctx->sample_aspect_ratio = AVRational{std::max(1, cfg_.parNum), std::max(1, cfg_.parDen)};
        ctx->gop_size = std::max<int>(12, static_cast<int>((cfg_.fpsNum * 2) / std::max<int64_t>(1, cfg_.fpsDen)));

        if (cfg_.colorSpace == ExportColorSpace::Rec2020_PQ_HDR10) {
            ctx->colorspace = AVCOL_SPC_BT2020_NCL;
            ctx->color_primaries = AVCOL_PRI_BT2020;
            ctx->color_trc = AVCOL_TRC_SMPTE2084;
        } else if (cfg_.colorSpace == ExportColorSpace::Rec2020_HLG) {
            ctx->colorspace = AVCOL_SPC_BT2020_NCL;
            ctx->color_primaries = AVCOL_PRI_BT2020;
            ctx->color_trc = AVCOL_TRC_ARIB_STD_B67;
        } else {
            ctx->colorspace = AVCOL_SPC_BT709;
            ctx->color_primaries = AVCOL_PRI_BT709;
            ctx->color_trc = AVCOL_TRC_BT709;
        }
        ctx->color_range = (pixFmt == AV_PIX_FMT_YUVJ420P || pixFmt == AV_PIX_FMT_YUVJ422P)
                               ? AVCOL_RANGE_JPEG
                               : AVCOL_RANGE_MPEG;

        if (fmtCtx_->oformat->flags & AVFMT_GLOBALHEADER) {
            ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        const int threads = std::clamp(av_cpu_count(), 2, 16);
        ctx->thread_count = threads;

        AVDictionary* opts = nullptr;
        const std::string nameStr(encName);
        const int clampedCrf = std::clamp(cfg_.crfQuality, 0, 51);
        const int64_t targetBitsPerSec = static_cast<int64_t>(std::max(0.25, cfg_.targetBitrateMbps) * 1000000.0);

        if (nameStr.find("nvenc") != std::string::npos) {
            ctx->thread_count = 1;
            const std::string pName = "p" + std::to_string(std::clamp(cfg_.speedPreset, 1, 7));
            av_dict_set(&opts, "preset", pName.c_str(), 0);
            if (cfg_.useCrfMode) {
                av_dict_set(&opts, "rc", "vbr", 0);
                av_dict_set_int(&opts, "cq", std::clamp(clampedCrf, 1, 51), 0);
                ctx->bit_rate = 0;
            } else {
                ctx->bit_rate = targetBitsPerSec;
                ctx->rc_max_rate = (targetBitsPerSec * 3) / 2;
                ctx->rc_buffer_size = static_cast<int>(targetBitsPerSec * 2);
            }
        } else if (nameStr == "libsvtav1") {
            const int svtPreset = std::clamp(12 - cfg_.speedPreset * 2, 2, 10);
            av_dict_set_int(&opts, "preset", svtPreset, 0);
            if (cfg_.useCrfMode) {
                av_dict_set_int(&opts, "crf", clampedCrf, 0);
                ctx->bit_rate = 0;
            } else {
                ctx->bit_rate = targetBitsPerSec;
            }
        } else if (nameStr == "libvpx-vp9") {
            av_dict_set_int(&opts, "row-mt", 1, 0);
            av_dict_set_int(&opts, "cpu-used", std::clamp(8 - cfg_.speedPreset, 1, 5), 0);
            if (cfg_.useCrfMode) {
                av_dict_set_int(&opts, "crf", clampedCrf, 0);
                ctx->bit_rate = 0;
            } else {
                ctx->bit_rate = targetBitsPerSec;
            }
        } else if (nameStr == "prores_ks") {
            if (cfg_.videoCodec == ExportVideoCodec::PRORES_4444_ALPHA) {
                av_dict_set(&opts, "profile", "4444", 0);
            } else {
                av_dict_set(&opts, "profile", "hq", 0);
            }
        } else if (nameStr == "dnxhd") {
            av_dict_set(&opts, "profile",
                        (pixFmt == AV_PIX_FMT_YUV422P10LE) ? "dnxhr_hqx" : "dnxhr_hq", 0);
        } else if (nameStr == "ffv1") {
            ctx->level = 3;
            av_dict_set_int(&opts, "slicecrc", 1, 0);
        } else if (nameStr == "libwebp_anim" || nameStr == "libwebp") {
            av_dict_set_int(&opts, "loop", 0, 0);
            av_dict_set_int(&opts, "quality", std::clamp(100 - clampedCrf * 2, 35, 100), 0);
        } else {
            if (!cfg_.useCrfMode) {
                ctx->bit_rate = targetBitsPerSec;
            } else {
                ctx->bit_rate = targetBitsPerSec;
            }
        }

        const int openRet = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);
        if (openRet == 0) {
            vidEncCtx_ = ctx;
            chosenCodec = codec;
            activeVidEncName_ = encName;
            break;
        }
        avcodec_free_context(&ctx);
    }

    if (!vidEncCtx_ || !chosenCodec) {
        lastError_ = "Failed to open any compatible video encoder for requested format.";
        return false;
    }

    vidStream_ = avformat_new_stream(fmtCtx_, chosenCodec);
    if (!vidStream_) {
        lastError_ = "Failed creating output video stream.";
        return false;
    }
    vidStream_->time_base = vidEncCtx_->time_base;
    vidStream_->avg_frame_rate = vidEncCtx_->framerate;
    if (avcodec_parameters_from_context(vidStream_->codecpar, vidEncCtx_) < 0) {
        lastError_ = "Failed copying video codec parameters.";
        return false;
    }
    if (cfg_.videoCodec == ExportVideoCodec::VP9 && vidEncCtx_->pix_fmt == AV_PIX_FMT_YUVA420P) {
        av_dict_set(&vidStream_->metadata, "alpha_mode", "1", 0);
    }

    vidFrame_ = av_frame_alloc();
    vidFrame_->format = vidEncCtx_->pix_fmt;
    vidFrame_->width = vidEncCtx_->width;
    vidFrame_->height = vidEncCtx_->height;
    if (av_frame_get_buffer(vidFrame_, 32) < 0) {
        lastError_ = "Failed allocating encoder video frame.";
        return false;
    }

    swsCtx_ = sws_getContext(
        cfg_.width, cfg_.height, AV_PIX_FMT_BGRA,
        vidEncCtx_->width, vidEncCtx_->height, vidEncCtx_->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) {
        lastError_ = "Failed creating BGRA -> encoder pixel format scaler.";
        return false;
    }

    const int csCoeff = (cfg_.colorSpace == ExportColorSpace::Rec709_SDR) ? SWS_CS_ITU709 : SWS_CS_BT2020;
    const int* coeffs = sws_getCoefficients(csCoeff);
    const int dstRange = (vidEncCtx_->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    sws_setColorspaceDetails(swsCtx_, coeffs, 1, coeffs, dstRange, 0, 1 << 16, 1 << 16);
    return true;
}

bool AERExportEngine::initAudioStream() {
    const auto candidates = audioEncoderCandidates(cfg_.audioCodec, cfg_.containerExtension);
    const AVCodec* chosenCodec = nullptr;

    for (const char* encName : candidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(encName);
        if (!codec) continue;

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;

        const int outChannels = (cfg_.audioCodec == ExportAudioCodec::MP3 && cfg_.audioChannels > 2)
                                    ? 2
                                    : std::clamp(cfg_.audioChannels, 1, 6);
        effectiveAudRate_ = selectSupportedSampleRate(codec, std::max(8000, cfg_.audioSampleRate));
        ctx->sample_rate = effectiveAudRate_;
        ctx->sample_fmt = selectSupportedSampleFmt(codec);
        av_channel_layout_default(&ctx->ch_layout, outChannels);
        ctx->time_base = AVRational{1, effectiveAudRate_};
        ctx->bit_rate = static_cast<int64_t>(std::clamp(cfg_.audioBitrateKbps, 64, 640)) * 1000LL;

        if (fmtCtx_->oformat->flags & AVFMT_GLOBALHEADER) {
            ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        AVDictionary* opts = nullptr;
        if (std::string(encName) == "opus" || std::string(encName) == "vorbis") {
            ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        }
        const int openRet = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);
        if (openRet == 0) {
            audEncCtx_ = ctx;
            chosenCodec = codec;
            activeAudEncName_ = encName;
            break;
        }
        avcodec_free_context(&ctx);
    }

    if (!audEncCtx_ || !chosenCodec) {
        lastError_ = "Failed to open audio encoder.";
        return false;
    }

    audStream_ = avformat_new_stream(fmtCtx_, chosenCodec);
    if (!audStream_) {
        lastError_ = "Failed creating output audio stream.";
        return false;
    }
    audStream_->time_base = audEncCtx_->time_base;
    if (avcodec_parameters_from_context(audStream_->codecpar, audEncCtx_) < 0) {
        lastError_ = "Failed copying audio codec parameters.";
        return false;
    }

    AVChannelLayout inLayout{};
    av_channel_layout_default(&inLayout, std::clamp(cfg_.audioChannels, 1, 6));
    if (swr_alloc_set_opts2(&swrCtx_,
                            &audEncCtx_->ch_layout,
                            audEncCtx_->sample_fmt,
                            audEncCtx_->sample_rate,
                            &inLayout,
                            AV_SAMPLE_FMT_FLTP,
                            std::max(8000, cfg_.audioSampleRate),
                            0,
                            nullptr) < 0 ||
        !swrCtx_ || swr_init(swrCtx_) < 0) {
        av_channel_layout_uninit(&inLayout);
        lastError_ = "Failed initializing audio resampler.";
        return false;
    }
    av_channel_layout_uninit(&inLayout);

    audFrame_ = av_frame_alloc();
    audioFifo_.assign(static_cast<size_t>(std::clamp(cfg_.audioChannels, 1, 6)), {});
    audioFifoSamples_ = 0;
    return true;
}

bool AERExportEngine::open(const ExportJobConfig& cfg) {
    abortAndClose();
    cfg_ = cfg;
    lastError_.clear();

    if (cfg_.outputPath.empty()) {
        lastError_ = "Empty output file path.";
        return false;
    }

    // Image-animation formats do not carry audio streams
    if (cfg_.containerExtension == "gif" || cfg_.containerExtension == "webp" || cfg_.containerExtension == "apng") {
        cfg_.enableAudio = false;
    }
    // Pure audio containers do not carry video streams
    if (cfg_.containerExtension == "opus" || cfg_.containerExtension == "flac" ||
        cfg_.containerExtension == "wav" || cfg_.containerExtension == "mp3" ||
        cfg_.containerExtension == "aac" || cfg_.containerExtension == "aiff") {
        cfg_.enableVideo = false;
    }

    if (!cfg_.enableVideo && !cfg_.enableAudio) {
        lastError_ = "Neither video nor audio enabled for export.";
        return false;
    }

    const std::string utf8Path = DemuxEngine::wideToUtf8(cfg_.outputPath);
    const char* fmtName = mapExtToFormatName(cfg_.containerExtension);
    if (avformat_alloc_output_context2(&fmtCtx_, nullptr, fmtName, utf8Path.c_str()) < 0 || !fmtCtx_) {
        lastError_ = "Failed allocating output format context.";
        return false;
    }

    pkt_ = av_packet_alloc();
    if (!pkt_) return false;

    if (cfg_.enableVideo && !initVideoStream()) {
        abortAndClose();
        return false;
    }
    if (cfg_.enableAudio && !initAudioStream()) {
        abortAndClose();
        return false;
    }

    av_dict_set(&fmtCtx_->metadata, "encoder", "ARZ Exporter 1.0.0 (FFmpeg 9.1+)", 0);

    if (!(fmtCtx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmtCtx_->pb, utf8Path.c_str(), AVIO_FLAG_WRITE) < 0) {
            lastError_ = "Failed opening output file for writing: " + utf8Path;
            abortAndClose();
            return false;
        }
    }

    AVDictionary* muxOpts = nullptr;
    if (cfg_.containerExtension == "mp4" || cfg_.containerExtension == "mov") {
        av_dict_set(&muxOpts, "movflags", "+faststart", 0);
    }
    const int hdrRet = avformat_write_header(fmtCtx_, &muxOpts);
    av_dict_free(&muxOpts);
    if (hdrRet < 0) {
        lastError_ = "Failed writing container header.";
        abortAndClose();
        return false;
    }

    nextVideoPts_ = 0;
    nextAudioPts_ = 0;
    opened_ = true;
    headerWritten_ = true;
    return true;
}

bool AERExportEngine::drainPackets(AVCodecContext* encCtx, AVStream* stream) {
    while (true) {
        const int ret = avcodec_receive_packet(encCtx, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            lastError_ = "Encoder packet receive failed.";
            return false;
        }
        av_packet_rescale_ts(pkt_, encCtx->time_base, stream->time_base);
        pkt_->stream_index = stream->index;
        const int wret = av_interleaved_write_frame(fmtCtx_, pkt_);
        av_packet_unref(pkt_);
        if (wret < 0) {
            lastError_ = "Container packet write failed.";
            return false;
        }
    }
}

bool AERExportEngine::writeVideoFrameBgra8(const uint8_t* bgraPixels,
                                           int rowBytes,
                                           bool bottomUpInput) {
    if (!opened_ || !vidEncCtx_ || !vidFrame_ || !swsCtx_ || !bgraPixels || rowBytes == 0) {
        return false;
    }
    if (av_frame_make_writable(vidFrame_) < 0) {
        return false;
    }

    // Zero-copy vertical flip when Premiere hands us bottom-up DIB BGRA_4444_8u!
    const uint8_t* srcSlice[4] = {nullptr, nullptr, nullptr, nullptr};
    int srcStride[4] = {0, 0, 0, 0};

    if (bottomUpInput && rowBytes > 0) {
        srcSlice[0] = bgraPixels + static_cast<size_t>(cfg_.height - 1) * static_cast<size_t>(rowBytes);
        srcStride[0] = -rowBytes;
    } else {
        srcSlice[0] = bgraPixels;
        srcStride[0] = rowBytes;
    }

    if (sws_scale(swsCtx_, srcSlice, srcStride, 0, cfg_.height, vidFrame_->data, vidFrame_->linesize) <= 0) {
        lastError_ = "sws_scale failed on export frame.";
        return false;
    }

    vidFrame_->pts = nextVideoPts_++;
    if (avcodec_send_frame(vidEncCtx_, vidFrame_) < 0) {
        lastError_ = "avcodec_send_frame (video) failed.";
        return false;
    }
    return drainPackets(vidEncCtx_, vidStream_);
}

bool AERExportEngine::encodeNextAudioFrameFromFifo(bool flushRemainder) {
    if (!audEncCtx_ || !audStream_ || !swrCtx_) return true;

    const bool varFrameSize = (audEncCtx_->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0;
    const int encFrameSize = (audEncCtx_->frame_size > 0 && !varFrameSize)
                                 ? audEncCtx_->frame_size
                                 : 2048;

    // Derive required input samples corresponding to encFrameSize output samples
    const int inRate = std::max(8000, cfg_.audioSampleRate);
    const size_t minInChunk = static_cast<size_t>(
        std::max<int64_t>(1, av_rescale_rnd(encFrameSize, inRate, audEncCtx_->sample_rate, AV_ROUND_UP)));

    while (audioFifoSamples_ >= minInChunk || (flushRemainder && audioFifoSamples_ > 0)) {
        const size_t takeIn = flushRemainder
                                  ? std::min(audioFifoSamples_, minInChunk)
                                  : minInChunk;
        const int outSamples = flushRemainder && (audioFifoSamples_ < minInChunk) && !varFrameSize
                                   ? encFrameSize
                                   : static_cast<int>(av_rescale_rnd(
                                         swr_get_delay(swrCtx_, inRate) + static_cast<int64_t>(takeIn),
                                         audEncCtx_->sample_rate, inRate, AV_ROUND_UP));
        const int allocSamples = (!varFrameSize && audEncCtx_->frame_size > 0)
                                     ? audEncCtx_->frame_size
                                     : std::max(1, outSamples);

        av_frame_unref(audFrame_);
        audFrame_->nb_samples = allocSamples;
        audFrame_->format = audEncCtx_->sample_fmt;
        av_channel_layout_copy(&audFrame_->ch_layout, &audEncCtx_->ch_layout);
        audFrame_->sample_rate = audEncCtx_->sample_rate;
        if (av_frame_get_buffer(audFrame_, 0) < 0) {
            return false;
        }
        if (av_frame_make_writable(audFrame_) < 0) {
            return false;
        }
        // Pad frame with silence in case flushRemainder has fewer samples than fixed frame_size
        av_samples_set_silence(audFrame_->extended_data, 0, allocSamples,
                               audFrame_->ch_layout.nb_channels,
                               static_cast<AVSampleFormat>(audFrame_->format));

        std::vector<const uint8_t*> inPtrs(audioFifo_.size());
        for (size_t c = 0; c < audioFifo_.size(); ++c) {
            inPtrs[c] = reinterpret_cast<const uint8_t*>(audioFifo_[c].data());
        }

        const int converted = swr_convert(
            swrCtx_,
            audFrame_->extended_data,
            allocSamples,
            inPtrs.data(),
            static_cast<int>(takeIn));

        for (auto& ch : audioFifo_) {
            ch.erase(ch.begin(), ch.begin() + static_cast<std::ptrdiff_t>(takeIn));
        }
        audioFifoSamples_ -= takeIn;

        if (converted > 0) {
            if (varFrameSize || audEncCtx_->frame_size == 0) {
                audFrame_->nb_samples = converted;
            }
            audFrame_->pts = nextAudioPts_;
            nextAudioPts_ += audFrame_->nb_samples;

            if (avcodec_send_frame(audEncCtx_, audFrame_) < 0) {
                lastError_ = "avcodec_send_frame (audio) failed.";
                return false;
            }
            if (!drainPackets(audEncCtx_, audStream_)) {
                return false;
            }
        }
    }
    return true;
}

bool AERExportEngine::writeAudioSamplesFloatPlanar(const float* const* channelBuffers,
                                                   uint32_t sampleFrames) {
    if (!opened_ || !audEncCtx_ || !channelBuffers || sampleFrames == 0) {
        return true;
    }
    const size_t chCount = audioFifo_.size();
    for (size_t c = 0; c < chCount; ++c) {
        if (channelBuffers[c]) {
            audioFifo_[c].insert(audioFifo_[c].end(),
                                 channelBuffers[c],
                                 channelBuffers[c] + sampleFrames);
        } else {
            audioFifo_[c].insert(audioFifo_[c].end(), sampleFrames, 0.0f);
        }
    }
    audioFifoSamples_ += sampleFrames;
    return encodeNextAudioFrameFromFifo(/*flushRemainder=*/false);
}

bool AERExportEngine::finalize() {
    if (!opened_ || !fmtCtx_) return false;
    bool ok = true;

    if (audEncCtx_ && audStream_) {
        ok = encodeNextAudioFrameFromFifo(/*flushRemainder=*/true) && ok;
        avcodec_send_frame(audEncCtx_, nullptr);
        ok = drainPackets(audEncCtx_, audStream_) && ok;
    }
    if (vidEncCtx_ && vidStream_) {
        avcodec_send_frame(vidEncCtx_, nullptr);
        ok = drainPackets(vidEncCtx_, vidStream_) && ok;
    }
    if (headerWritten_) {
        if (av_write_trailer(fmtCtx_) < 0) {
            lastError_ = "av_write_trailer failed.";
            ok = false;
        }
        headerWritten_ = false;
    }

    abortAndClose();
    return ok;
}

} // namespace aer
