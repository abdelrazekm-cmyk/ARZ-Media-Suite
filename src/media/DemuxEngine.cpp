#include "media/DemuxEngine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

namespace aer {

namespace {

int mapToPremiereSafeChannels(int srcCh) {
    if (srcCh <= 1) return 1;
    if (srcCh == 2) return 2;
    if (srcCh >= 6) return 6; // 5.1 & 7.1 route cleanly into Premiere's native 5.1 bus via SwrContext
    return 2;                 // 3/4/5ch downmix cleanly to Stereo
}

bool streamHasContainerAlphaTag(AVStream* vs) {
    if (!vs || !vs->metadata) return false;
    const char* keys[] = {"alpha_mode", "ALPHA_MODE", "alphamode"};
    for (const char* k : keys) {
        if (const AVDictionaryEntry* e = av_dict_get(vs->metadata, k, nullptr, 0)) {
            if (e->value && std::strcmp(e->value, "1") == 0) {
                return true;
            }
        }
    }
    return false;
}

// Recovers exact stream durations from the final cluster/packets of unfinalized MKV/WebM/TS recordings
// (e.g., crashed OBS recordings or Chrome WebM streams missing header duration metadata).
void recoverMissingDurationFromTail(AVFormatContext* ctx, int vIdx, int aIdx,
                                    int64_t& ioMaxVideoEndPts, int64_t& ioMaxAudioEndPts) {
    if (!ctx || !ctx->pb || (ctx->pb->seekable & AVIO_SEEKABLE_NORMAL) == 0) {
        return;
    }

    const int64_t fileSize = avio_size(ctx->pb);
    if (fileSize <= 0) {
        return;
    }

    const int64_t tailByteStart = std::max<int64_t>(0, fileSize - 4LL * 1024LL * 1024LL);
    bool seeked = false;
    if (tailByteStart > 0 &&
        avformat_seek_file(ctx, -1, 0, tailByteStart, fileSize, AVSEEK_FLAG_BYTE) >= 0) {
        seeked = true;
    } else if (vIdx >= 0 &&
               av_seek_frame(ctx, vIdx, INT64_MAX / 4, AVSEEK_FLAG_BACKWARD) >= 0) {
        seeked = true;
    }
    if (!seeked && tailByteStart > 0) {
        if (fileSize > 64LL * 1024LL * 1024LL) {
            return;
        }
        avformat_seek_file(ctx, -1, INT64_MIN, 0, INT64_MAX, AVSEEK_FLAG_BACKWARD);
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    int packetsScanned = 0;
    while (packetsScanned < 25000 && av_read_frame(ctx, pkt) >= 0) {
        ++packetsScanned;
        const int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
        if (ts != AV_NOPTS_VALUE) {
            const int64_t endTs = ts + std::max<int64_t>(0, pkt->duration);
            if (pkt->stream_index == vIdx) {
                ioMaxVideoEndPts = std::max(ioMaxVideoEndPts, endTs);
            } else if (pkt->stream_index == aIdx) {
                ioMaxAudioEndPts = std::max(ioMaxAudioEndPts, endTs);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    if (vIdx >= 0) {
        const int64_t startPts = (ctx->streams[vIdx]->start_time != AV_NOPTS_VALUE)
                                     ? ctx->streams[vIdx]->start_time
                                     : 0;
        avformat_seek_file(ctx, vIdx, INT64_MIN, startPts, startPts, AVSEEK_FLAG_BACKWARD);
    } else {
        avformat_seek_file(ctx, -1, INT64_MIN, 0, 0, AVSEEK_FLAG_BACKWARD);
    }
}

} // namespace

std::string DemuxEngine::wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
#if defined(_WIN32)
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), out.data(), needed, nullptr, nullptr);
    return out;
#else
    return std::string(wstr.begin(), wstr.end());
#endif
}

std::string DemuxEngine::extractExtensionLower(const std::wstring& filePath) {
    const auto dotPos = filePath.find_last_of(L'.');
    if (dotPos == std::wstring::npos || dotPos + 1 >= filePath.size()) {
        return {};
    }
    std::string ext = wideToUtf8(filePath.substr(dotPos + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

AVFormatContext* DemuxEngine::openFormatContextUtf8(const std::string& utf8Path) {
    if (utf8Path.empty()) return nullptr;
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, utf8Path.c_str(), nullptr, nullptr) < 0 || !ctx) {
        return nullptr;
    }
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        return nullptr;
    }
    return ctx;
}

bool DemuxEngine::probeFileMetadata(const std::wstring& filePath,
                                    MediaMetadata& outMeta,
                                    AVFormatContext** outKeepVideoCtx) {
    outMeta = MediaMetadata{};
    outMeta.fileExtensionLower = extractExtensionLower(filePath);
    const std::string utf8Path = wideToUtf8(filePath);
    AVFormatContext* ctx = openFormatContextUtf8(utf8Path);
    if (!ctx) {
        return false;
    }

    if (ctx->iformat && ctx->iformat->name) {
        outMeta.containerName = ctx->iformat->name;
    }

    if (const AVDictionaryEntry* tc = av_dict_get(ctx->metadata, "timecode", nullptr, 0)) {
        outMeta.timecodeString = tc->value;
    }

    int vIdx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int aIdx = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // Ignore embedded album-art pictures on pure audio files (MP3/FLAC/M4A/WAV/AIFF)
    if (vIdx >= 0 && aIdx >= 0 &&
        (ctx->streams[vIdx]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
        vIdx = -1;
    }

    bool durationMissing = (ctx->duration <= 0 || ctx->duration == AV_NOPTS_VALUE);
    if (vIdx >= 0) {
        AVStream* vs = ctx->streams[vIdx];
        if (vs->nb_frames <= 0 && (vs->duration <= 0 || vs->duration == AV_NOPTS_VALUE) && durationMissing) {
            durationMissing = true;
        } else {
            durationMissing = false;
        }
    }

    int64_t recoveredVidEndPts = -1;
    int64_t recoveredAudEndPts = -1;
    if (durationMissing) {
        recoverMissingDurationFromTail(ctx, vIdx, aIdx, recoveredVidEndPts, recoveredAudEndPts);
    }

    if (vIdx >= 0) {
        AVStream* vs = ctx->streams[vIdx];
        const AVCodecParameters* par = vs->codecpar;
        if (par->width > 0 && par->height > 0) {
            outMeta.hasVideo = true;
            outMeta.videoStreamIndex = vIdx;
            outMeta.width = par->width;
            outMeta.height = par->height;
            outMeta.videoCodecName = avcodec_get_name(par->codec_id);
            if (const char* prof = avcodec_profile_name(par->codec_id, par->profile)) {
                outMeta.videoProfileName = prof;
            }
            if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
                outMeta.parNum = static_cast<uint32_t>(par->sample_aspect_ratio.num);
                outMeta.parDen = static_cast<uint32_t>(par->sample_aspect_ratio.den);
            } else if (vs->sample_aspect_ratio.num > 0 && vs->sample_aspect_ratio.den > 0) {
                outMeta.parNum = static_cast<uint32_t>(vs->sample_aspect_ratio.num);
                outMeta.parDen = static_cast<uint32_t>(vs->sample_aspect_ratio.den);
            }

            if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format))) {
                if (desc->nb_components > 0 && desc->comp[0].depth > 0) {
                    outMeta.bitDepth = desc->comp[0].depth;
                }
                outMeta.hasAlpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
            }
            if (streamHasContainerAlphaTag(vs) ||
                par->codec_id == AV_CODEC_ID_GIF ||
                par->codec_id == AV_CODEC_ID_APNG ||
                par->codec_tag == MKTAG('a', 'p', '4', 'h') || // ProRes 4444
                par->codec_tag == MKTAG('a', 'p', '4', 'x')) { // ProRes 4444 XQ
                outMeta.hasAlpha = true;
            }

            AVRational fps = av_guess_frame_rate(ctx, vs, nullptr);
            if (fps.num <= 0 || fps.den <= 0) {
                fps = vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0
                          ? vs->avg_frame_rate
                          : AVRational{30, 1};
            }
            outMeta.fpsNum = fps.num;
            outMeta.fpsDen = fps.den;
            outMeta.vidTimebaseNum = vs->time_base.num;
            outMeta.vidTimebaseDen = vs->time_base.den;
            outMeta.videoStartPts = (vs->start_time != AV_NOPTS_VALUE) ? vs->start_time : 0;

            const AVRational invFps{fps.den, fps.num};
            outMeta.videoFrameStepPts = std::max<int64_t>(
                1, av_rescale_q(1, invFps, vs->time_base));

            if (vs->nb_frames > 0) {
                outMeta.totalFrames = vs->nb_frames;
            } else if (vs->duration > 0 && vs->duration != AV_NOPTS_VALUE) {
                outMeta.totalFrames = std::max<int64_t>(
                    1, av_rescale_q(vs->duration, vs->time_base, invFps));
            } else if (ctx->duration > 0 && ctx->duration != AV_NOPTS_VALUE) {
                outMeta.totalFrames = std::max<int64_t>(
                    1, av_rescale_q(ctx->duration, AVRational{1, AV_TIME_BASE}, invFps));
            } else if (recoveredVidEndPts > outMeta.videoStartPts) {
                const int64_t spanPts = recoveredVidEndPts - outMeta.videoStartPts + outMeta.videoFrameStepPts;
                outMeta.totalFrames = std::max<int64_t>(
                    1, av_rescale_q(spanPts, vs->time_base, invFps));
            } else {
                outMeta.totalFrames = 1;
            }

            if (outMeta.timecodeString.empty()) {
                if (const AVDictionaryEntry* stc = av_dict_get(vs->metadata, "timecode", nullptr, 0)) {
                    outMeta.timecodeString = stc->value;
                }
            }

            outMeta.isBt2020 = (par->color_space == AVCOL_SPC_BT2020_NCL ||
                                par->color_space == AVCOL_SPC_BT2020_CL ||
                                par->color_primaries == AVCOL_PRI_BT2020);
            outMeta.isBt601 = (par->color_space == AVCOL_SPC_SMPTE170M ||
                               par->color_space == AVCOL_SPC_BT470BG);
            outMeta.isHdr = (par->color_trc == AVCOL_TRC_SMPTE2084 ||
                             par->color_trc == AVCOL_TRC_ARIB_STD_B67);

            if (const AVPacketSideData* sd = av_packet_side_data_get(
                    par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
                if (sd->size >= 36) {
                    const double rot = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
                    if (!std::isnan(rot)) {
                        int deg = static_cast<int>(std::lround(-rot)) % 360;
                        if (deg < 0) deg += 360;
                        outMeta.rotationDegrees = deg;
                    }
                }
            }

            const char* cs = av_color_space_name(par->color_space);
            const char* trc = av_color_transfer_name(par->color_trc);
            const char* rng = av_color_range_name(par->color_range);
            outMeta.colorSpaceSummary = std::string(cs ? cs : "unknown") + "/" +
                                        std::string(trc ? trc : "unknown") + "/" +
                                        std::string(rng ? rng : "unknown");
        }
    }

    if (aIdx >= 0) {
        AVStream* as = ctx->streams[aIdx];
        const AVCodecParameters* apar = as->codecpar;
        if (apar->sample_rate > 0 && apar->ch_layout.nb_channels > 0) {
            outMeta.hasAudio = true;
            outMeta.audioStreamIndex = aIdx;
            outMeta.audioSampleRate = apar->sample_rate;
            outMeta.sourceAudioChannels = std::clamp(apar->ch_layout.nb_channels, 1, 32);
            outMeta.audioChannels = mapToPremiereSafeChannels(outMeta.sourceAudioChannels);
            outMeta.audioCodecName = avcodec_get_name(apar->codec_id);

            const int64_t audStart = (as->start_time != AV_NOPTS_VALUE) ? as->start_time : 0;
            if (as->duration > 0 && as->duration != AV_NOPTS_VALUE) {
                outMeta.totalAudioSampleFrames = std::max<int64_t>(
                    1, av_rescale_q(as->duration, as->time_base, AVRational{1, apar->sample_rate}));
            } else if (ctx->duration > 0 && ctx->duration != AV_NOPTS_VALUE) {
                outMeta.totalAudioSampleFrames = std::max<int64_t>(
                    1, av_rescale_q(ctx->duration, AVRational{1, AV_TIME_BASE}, AVRational{1, apar->sample_rate}));
            } else if (recoveredAudEndPts > audStart) {
                outMeta.totalAudioSampleFrames = std::max<int64_t>(
                    1, av_rescale_q(recoveredAudEndPts - audStart, as->time_base, AVRational{1, apar->sample_rate}));
            } else if (outMeta.hasVideo && outMeta.fpsNum > 0) {
                outMeta.totalAudioSampleFrames =
                    (outMeta.totalFrames * outMeta.fpsDen * outMeta.audioSampleRate) / outMeta.fpsNum;
            }
        }
    }

    outMeta.valid = (outMeta.hasVideo || outMeta.hasAudio);
    if (!outMeta.valid) {
        avformat_close_input(&ctx);
        return false;
    }

    if (outKeepVideoCtx && outMeta.hasVideo) {
        *outKeepVideoCtx = ctx;
    } else {
        avformat_close_input(&ctx);
    }
    return true;
}

} // namespace aer
