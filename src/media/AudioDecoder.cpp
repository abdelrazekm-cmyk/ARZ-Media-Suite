#include "media/AudioDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace aer {

AudioDecoder::AudioDecoder(std::wstring filePath)
    : filePath_(std::move(filePath)) {}

AudioDecoder::~AudioDecoder() {
    close();
}

void AudioDecoder::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame_) av_frame_free(&frame_);
    if (pkt_) av_packet_free(&pkt_);
    if (swrCtx_) swr_free(&swrCtx_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
    if (fmtCtx_) avformat_close_input(&fmtCtx_);
    channelFifo_.clear();
    fifoSampleCount_ = 0;
    fifoStartSample_ = 0;
    eofReached_ = false;
}

bool AudioDecoder::open(const MediaMetadata& meta) {
    std::lock_guard<std::mutex> lock(mutex_);
    meta_ = meta;
    if (!meta_.hasAudio || meta_.audioStreamIndex < 0) {
        return false;
    }

    fmtCtx_ = DemuxEngine::openFormatContextUtf8(DemuxEngine::wideToUtf8(filePath_));
    if (!fmtCtx_ || meta_.audioStreamIndex >= static_cast<int>(fmtCtx_->nb_streams)) {
        return false;
    }

    AVStream* st = fmtCtx_->streams[meta_.audioStreamIndex];
    const AVCodec* decoder = nullptr;
    if (st->codecpar->codec_id == AV_CODEC_ID_OPUS) {
        decoder = avcodec_find_decoder_by_name("libopus");
    } else if (st->codecpar->codec_id == AV_CODEC_ID_VORBIS) {
        decoder = avcodec_find_decoder_by_name("libvorbis");
    }
    if (!decoder) {
        decoder = avcodec_find_decoder(st->codecpar->codec_id);
    }
    if (!decoder) return false;

    codecCtx_ = avcodec_alloc_context3(decoder);
    if (!codecCtx_) return false;
    if (avcodec_parameters_to_context(codecCtx_, st->codecpar) < 0) {
        return false;
    }
    codecCtx_->pkt_timebase = st->time_base;
    if (avcodec_open2(codecCtx_, decoder, nullptr) < 0) {
        return false;
    }

    AVChannelLayout outLayout{};
    av_channel_layout_default(&outLayout, meta_.audioChannels);
    if (swr_alloc_set_opts2(&swrCtx_,
                            &outLayout,
                            AV_SAMPLE_FMT_FLTP,
                            meta_.audioSampleRate,
                            &codecCtx_->ch_layout,
                            codecCtx_->sample_fmt,
                            codecCtx_->sample_rate,
                            0,
                            nullptr) < 0 ||
        !swrCtx_ || swr_init(swrCtx_) < 0) {
        av_channel_layout_uninit(&outLayout);
        return false;
    }
    av_channel_layout_uninit(&outLayout);

    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    channelFifo_.assign(static_cast<size_t>(meta_.audioChannels), {});
    fifoStartSample_ = 0;
    fifoSampleCount_ = 0;
    eofReached_ = false;
    return pkt_ != nullptr && frame_ != nullptr;
}

void AudioDecoder::discardLeadingFifoSamplesLocked(size_t count) {
    const size_t n = std::min(count, fifoSampleCount_);
    if (n == 0) return;
    for (auto& ch : channelFifo_) {
        ch.erase(ch.begin(), ch.begin() + static_cast<std::ptrdiff_t>(n));
    }
    fifoStartSample_ += static_cast<int64_t>(n);
    fifoSampleCount_ -= n;
}

bool AudioDecoder::decodeNextChunkLocked() {
    while (true) {
        const int ret = avcodec_receive_frame(codecCtx_, frame_);
        if (ret == 0) {
            const int maxOut = swr_get_out_samples(swrCtx_, frame_->nb_samples);
            if (maxOut > 0) {
                std::vector<std::vector<float>> temp(
                    static_cast<size_t>(meta_.audioChannels),
                    std::vector<float>(static_cast<size_t>(maxOut)));
                std::vector<uint8_t*> outPtrs(static_cast<size_t>(meta_.audioChannels));
                for (int c = 0; c < meta_.audioChannels; ++c) {
                    outPtrs[static_cast<size_t>(c)] =
                        reinterpret_cast<uint8_t*>(temp[static_cast<size_t>(c)].data());
                }
                const int converted = swr_convert(
                    swrCtx_,
                    outPtrs.data(),
                    maxOut,
                    const_cast<const uint8_t**>(frame_->extended_data),
                    frame_->nb_samples);
                av_frame_unref(frame_);
                if (converted > 0) {
                    for (int c = 0; c < meta_.audioChannels; ++c) {
                        auto& dst = channelFifo_[static_cast<size_t>(c)];
                        dst.insert(dst.end(),
                                   temp[static_cast<size_t>(c)].begin(),
                                   temp[static_cast<size_t>(c)].begin() + converted);
                    }
                    fifoSampleCount_ += static_cast<size_t>(converted);
                    return true;
                }
            } else {
                av_frame_unref(frame_);
            }
            continue;
        }

        if (ret == AVERROR_EOF) {
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            continue;
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
        if (pkt_->stream_index == meta_.audioStreamIndex) {
            avcodec_send_packet(codecCtx_, pkt_);
        }
        av_packet_unref(pkt_);
    }
}

bool AudioDecoder::readPlanarFloatSamples(int64_t startSampleFrame,
                                          uint32_t sampleFrameCount,
                                          float* const* outChannelBuffers) {
    if (!outChannelBuffers || sampleFrameCount == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!fmtCtx_ || !codecCtx_ || !swrCtx_) {
        return false;
    }

    for (int c = 0; c < meta_.audioChannels; ++c) {
        if (outChannelBuffers[c]) {
            std::memset(outChannelBuffers[c], 0, static_cast<size_t>(sampleFrameCount) * sizeof(float));
        }
    }

    const int64_t targetStart = (startSampleFrame < 0)
                                    ? fifoStartSample_
                                    : startSampleFrame;

    const int64_t fifoEnd = fifoStartSample_ + static_cast<int64_t>(fifoSampleCount_);
    const bool needSeek =
        (targetStart < fifoStartSample_) ||
        (targetStart > fifoEnd + static_cast<int64_t>(meta_.audioSampleRate * 2));

    if (needSeek) {
        AVStream* st = fmtCtx_->streams[meta_.audioStreamIndex];
        const int64_t streamStart = (st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;
        const int64_t seekPts = streamStart + av_rescale_q(
            targetStart, AVRational{1, meta_.audioSampleRate}, st->time_base);
        avformat_seek_file(fmtCtx_, meta_.audioStreamIndex, INT64_MIN, seekPts, seekPts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx_);
        swr_init(swrCtx_);
        for (auto& ch : channelFifo_) ch.clear();
        fifoSampleCount_ = 0;
        fifoStartSample_ = targetStart;
        eofReached_ = false;

        if (av_read_frame(fmtCtx_, pkt_) >= 0) {
            if (pkt_->stream_index == meta_.audioStreamIndex) {
                if (pkt_->pts != AV_NOPTS_VALUE) {
                    const int64_t actualSample = av_rescale_q(
                        std::max<int64_t>(0, pkt_->pts - streamStart),
                        st->time_base,
                        AVRational{1, meta_.audioSampleRate});
                    if (actualSample <= targetStart) {
                        fifoStartSample_ = actualSample;
                    }
                }
                avcodec_send_packet(codecCtx_, pkt_);
            }
            av_packet_unref(pkt_);
        }
    }

    while (fifoStartSample_ + static_cast<int64_t>(fifoSampleCount_) <= targetStart) {
        discardLeadingFifoSamplesLocked(fifoSampleCount_);
        if (!decodeNextChunkLocked()) {
            return true;
        }
    }
    if (targetStart > fifoStartSample_) {
        discardLeadingFifoSamplesLocked(static_cast<size_t>(targetStart - fifoStartSample_));
    }

    while (fifoSampleCount_ < sampleFrameCount) {
        if (!decodeNextChunkLocked()) {
            break;
        }
    }

    const size_t available = std::min<size_t>(fifoSampleCount_, sampleFrameCount);
    for (int c = 0; c < meta_.audioChannels; ++c) {
        if (outChannelBuffers[c] && available > 0) {
            std::memcpy(outChannelBuffers[c],
                        channelFifo_[static_cast<size_t>(c)].data(),
                        available * sizeof(float));
        }
    }
    discardLeadingFifoSamplesLocked(available);
    return true;
}

bool AudioDecoder::computePeakAudio(int64_t startSampleFrame,
                                    float peakSampleRate,
                                    int32_t numPeakFrames,
                                    float** outMinima,
                                    float** outMaxima) {
    if (!outMinima || !outMaxima || numPeakFrames <= 0 || meta_.audioSampleRate <= 0) {
        return false;
    }
    const double ratio = (peakSampleRate > 0.0f)
                             ? (static_cast<double>(meta_.audioSampleRate) / static_cast<double>(peakSampleRate))
                             : 1.0;
    const uint32_t samplesPerBin = static_cast<uint32_t>(std::clamp<double>(std::round(ratio), 1.0, 4096.0));
    const uint32_t totalToRead = static_cast<uint32_t>(
        std::min<uint64_t>(static_cast<uint64_t>(numPeakFrames) * samplesPerBin, 262144ULL));

    const int chCount = meta_.audioChannels;
    std::vector<std::vector<float>> scratch(static_cast<size_t>(chCount), std::vector<float>(totalToRead, 0.0f));
    std::vector<float*> ptrs(static_cast<size_t>(chCount));
    for (int c = 0; c < chCount; ++c) {
        ptrs[static_cast<size_t>(c)] = scratch[static_cast<size_t>(c)].data();
    }

    const int64_t srcStart = static_cast<int64_t>(std::max<double>(0.0, static_cast<double>(startSampleFrame) * ratio));
    if (!readPlanarFloatSamples(srcStart, totalToRead, ptrs.data())) {
        return false;
    }

    for (int c = 0; c < chCount; ++c) {
        if (!outMinima[c] || !outMaxima[c]) continue;
        for (int32_t b = 0; b < numPeakFrames; ++b) {
            const size_t binStart = std::min<size_t>(static_cast<size_t>(b) * samplesPerBin, totalToRead);
            const size_t binEnd = std::min<size_t>(binStart + samplesPerBin, totalToRead);
            if (binStart >= binEnd) {
                outMinima[c][b] = 0.0f;
                outMaxima[c][b] = 0.0f;
                continue;
            }
            float mn = scratch[static_cast<size_t>(c)][binStart];
            float mx = mn;
            for (size_t i = binStart + 1; i < binEnd; ++i) {
                const float v = scratch[static_cast<size_t>(c)][i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            outMinima[c][b] = mn;
            outMaxima[c][b] = mx;
        }
    }
    return true;
}

} // namespace aer
