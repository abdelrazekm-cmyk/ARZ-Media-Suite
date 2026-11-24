#pragma once

#include "cache/FrameCache.hpp"
#include "cache/GOPSeekIndex.hpp"
#include "media/DemuxEngine.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace aer {

class VideoDecoder {
public:
    VideoDecoder(uint64_t sessionId, std::wstring filePath);
    ~VideoDecoder();

    bool open(AVFormatContext* adoptedVideoCtx, MediaMetadata& ioMeta);
    void quietHardwareSession();
    void close();

    // Immediately signals any in-flight Long-GOP decode loop to abort so the newest scrub position wins!
    uint64_t bumpScrubGeneration() noexcept {
        return scrubGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    uint64_t currentScrubGeneration() const noexcept {
        return scrubGeneration_.load(std::memory_order_acquire);
    }

    bool decodeFrame(int64_t frameNumber,
                     int outWidth,
                     int outHeight,
                     int outRowBytes,
                     PixelBufferFormat outFormat,
                     uint8_t* outBuffer,
                     bool flipVertical = false,
                     bool fastKeyframeScrub = false,
                     uint64_t expectedGeneration = 0);

    int64_t nearestKeyframeFrame(int64_t frameNumber) const;

    void simulateHardwareFailureNextDecode() noexcept { forceSimulateHwFailure_ = true; }

    const std::string& activeBackendName() const noexcept { return backendName_; }
    const std::string& fallbackReason() const noexcept { return fallbackReason_; }
    bool isUsingHardware() const noexcept { return usingHardware_; }
    const GOPSeekIndex& seekIndex() const noexcept { return seekIndex_; }
    static uint32_t activeHardwareStreamCount() noexcept;

private:
    bool hasDynamicVramHeadroomLocked() const;
    void trackGpuActiveLocked(bool nowActive);
    bool tryOpenCodecLocked(bool allowHardware);
    bool fallbackToSoftwareMidStreamLocked(const std::string& reason);
    void publishTelemetryLocked(const std::string& stateOverride = {});

    bool decodeNextRawOrSwFrameLocked(AVFrame* outDecodedFrame, int64_t& outPts, bool& outIsKey);

    bool materializeAndCacheFrameLocked(const AVFrame* decodedFrame,
                                        int64_t frameNumber,
                                        int outWidth,
                                        int outHeight,
                                        int outRowBytes,
                                        PixelBufferFormat outFormat,
                                        uint8_t* optionalCpuDst,
                                        bool flipVertical);

    bool convertSwAVFrameToTargetLocked(const AVFrame* src,
                                        int outWidth,
                                        int outHeight,
                                        int outRowBytes,
                                        PixelBufferFormat outFormat,
                                        uint8_t* dstBuffer);
    void resetScalerLocked();

    static std::atomic<uint32_t> s_activeGpuStreams_;

    uint64_t sessionId_ = 0;
    std::wstring filePath_;
    MediaMetadata meta_{};

    std::atomic<uint64_t> scrubGeneration_{1};
    mutable std::mutex mutex_;
    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVBufferRef* hwDeviceCtx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* rawFrame_ = nullptr;
    AVFrame* swTransferFrame_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    int swsSrcW_ = 0;
    int swsSrcH_ = 0;
    int swsSrcFmt_ = -1;
    int swsDstW_ = 0;
    int swsDstH_ = 0;
    int swsDstFmt_ = -1;
    std::vector<uint16_t> tempRgba64_;

    bool usingHardware_ = false;
    bool usingSharedD3D11_ = false;
    bool countedInGpuActive_ = false;
    bool quietedFromHardware_ = false;
    bool forceSimulateHwFailure_ = false;
    bool eofReached_ = false;
    int hwPixFmt_ = -1;
    int64_t currentPts_ = -1;
    int64_t currentFrameNumber_ = -1;
    int64_t lastRequestedFrame_ = -1;
    std::chrono::steady_clock::time_point lastJumpTime_{};
    int rapidJumpStreak_ = 0;
    std::string backendName_ = "none";
    std::string fallbackReason_ = "ok";
    GOPSeekIndex seekIndex_;
};

} // namespace aer
