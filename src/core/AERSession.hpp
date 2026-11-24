#pragma once

#include "cache/FrameCache.hpp"
#include "media/AudioDecoder.hpp"
#include "media/DemuxEngine.hpp"
#include "media/VideoDecoder.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#if defined(_WIN32)
  #if defined(AER_CORE_EXPORTS)
    #define AER_API __declspec(dllexport)
  #else
    #define AER_API __declspec(dllimport)
  #endif
#else
  #define AER_API
#endif

namespace aer {

class AER_API AERSession {
public:
    AERSession();
    ~AERSession();

    bool open(const std::wstring& filePath, bool highPriorityMode);
    void quiet();
    void close();

    uint64_t sessionId() const noexcept { return sessionId_; }
    const std::wstring& filePath() const noexcept { return filePath_; }
    const MediaMetadata& metadata() const noexcept { return meta_; }
    std::string activeVideoBackend() const;
    std::string videoFallbackReason() const;
    bool isUsingHardwareDecode() const;
    size_t keyframeCount() const;

    uint64_t bumpScrubGeneration() noexcept;
    uint64_t currentScrubGeneration() const noexcept;
    int64_t nearestKeyframeFrame(int64_t frameNumber) const;

    void simulateHardwareFailureNextDecode();

    static uint32_t activeGpuStreams();
    static std::string telemetryStatusJsonPath();
    static size_t totalCachedVramBytes();
    static size_t totalCachedVramFrames();
    static size_t totalCachedBytes();
    static size_t totalCachedFrames();

    bool renderVideoFrame(int64_t frameNumber,
                          int outWidth,
                          int outHeight,
                          int outRowBytes,
                          PixelBufferFormat outFormat,
                          uint8_t* outPixels,
                          bool flipVertical = false,
                          bool fastKeyframeScrub = false,
                          uint64_t expectedGeneration = 0);

    bool renderAudioSamples(int64_t startSampleFrame,
                            uint32_t sampleFrameCount,
                            float* const* outChannelBuffers);

    bool renderAudioPeaks(int64_t startSampleFrame,
                          float peakSampleRate,
                          int32_t numPeakFrames,
                          float** outMinima,
                          float** outMaxima);

    static bool shouldAcceptFile(const MediaMetadata& meta, bool highPriorityMode);

private:
    VideoDecoder* ensureThumbnailDecoder();

    static std::atomic<uint64_t> s_nextId_;
    uint64_t sessionId_ = 0;
    std::wstring filePath_;
    MediaMetadata meta_{};
    std::unique_ptr<VideoDecoder> videoDecoder_;
    std::mutex thumbInitMutex_;
    std::unique_ptr<VideoDecoder> thumbDecoder_;
    std::unique_ptr<AudioDecoder> audioDecoder_;
};

} // namespace aer

extern "C" AER_API int32_t xAEREntry(
    int32_t selector,
    void* stdParms,
    void* param1,
    void* param2,
    int32_t isHighPriority);
