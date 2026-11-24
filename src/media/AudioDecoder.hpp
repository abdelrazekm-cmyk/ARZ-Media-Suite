#pragma once

#include "media/DemuxEngine.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace aer {

class AudioDecoder {
public:
    explicit AudioDecoder(std::wstring filePath);
    ~AudioDecoder();

    bool open(const MediaMetadata& meta);
    void close();

    bool readPlanarFloatSamples(int64_t startSampleFrame,
                                uint32_t sampleFrameCount,
                                float* const* outChannelBuffers);

    bool computePeakAudio(int64_t startSampleFrame,
                          float peakSampleRate,
                          int32_t numPeakFrames,
                          float** outMinima,
                          float** outMaxima);

private:
    bool decodeNextChunkLocked();
    void discardLeadingFifoSamplesLocked(size_t count);

    std::wstring filePath_;
    MediaMetadata meta_{};

    mutable std::mutex mutex_;
    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwrContext* swrCtx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;

    bool eofReached_ = false;
    int64_t fifoStartSample_ = 0;
    size_t fifoSampleCount_ = 0;
    std::vector<std::vector<float>> channelFifo_;
};

} // namespace aer
