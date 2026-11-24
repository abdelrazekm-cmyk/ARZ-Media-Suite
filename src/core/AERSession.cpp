#include "core/AERSession.hpp"
#include "core/AERConfig.hpp"
#include "core/AERTelemetry.hpp"
#include "media/GpuDeviceManager.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

namespace aer {

std::atomic<uint64_t> AERSession::s_nextId_{1};

AERSession::AERSession()
    : sessionId_(s_nextId_.fetch_add(1, std::memory_order_relaxed)) {}

AERSession::~AERSession() {
    close();
}

bool AERSession::shouldAcceptFile(const MediaMetadata& meta, bool highPriorityMode) {
    if (!meta.valid) {
        return false;
    }
    if (!highPriorityMode) {
        return true;
    }
    const auto& cfg = AERConfig::instance();
    if (cfg.forceOverrideAll) {
        return true;
    }
    if (cfg.isExtensionForced(meta.fileExtensionLower)) {
        return true;
    }
    if (!cfg.selectiveOverride) {
        return false;
    }
    if (meta.hasVideo && cfg.isCodecAutoOverride(meta.videoCodecName)) {
        return true;
    }
    if (meta.hasAudio && cfg.isCodecAutoOverride(meta.audioCodecName)) {
        return true;
    }
    return false;
}

bool AERSession::open(const std::wstring& filePath, bool highPriorityMode) {
    close();
    filePath_ = filePath;

    AVFormatContext* keptVideoCtx = nullptr;
    if (!DemuxEngine::probeFileMetadata(filePath_, meta_, &keptVideoCtx)) {
        if (keptVideoCtx) avformat_close_input(&keptVideoCtx);
        return false;
    }

    if (!shouldAcceptFile(meta_, highPriorityMode)) {
        if (keptVideoCtx) avformat_close_input(&keptVideoCtx);
        meta_ = MediaMetadata{};
        return false;
    }

    if (meta_.hasVideo) {
        videoDecoder_ = std::make_unique<VideoDecoder>(sessionId_, filePath_);
        if (!videoDecoder_->open(keptVideoCtx, meta_)) {
            videoDecoder_.reset();
            meta_.hasVideo = false;
        }
    } else if (keptVideoCtx) {
        avformat_close_input(&keptVideoCtx);
    }

    // Lazy AudioDecoder initialization: for video+audio containers, defer opening a second
    // AVFormatContext/codec until Premiere actually requests audio samples or waveform peaks!
    if (!meta_.hasVideo && meta_.hasAudio) {
        if (!ensureAudioDecoder()) {
            meta_.hasAudio = false;
        }
    }

    if (!meta_.hasVideo && !meta_.hasAudio) {
        close();
        return false;
    }
    return true;
}

AudioDecoder* AERSession::ensureAudioDecoder() {
    if (!meta_.hasAudio) return nullptr;
    std::lock_guard<std::mutex> lock(audioInitMutex_);
    if (audioDecoder_) {
        return audioDecoder_.get();
    }
    auto ad = std::make_unique<AudioDecoder>(filePath_);
    if (!ad->open(meta_)) {
        return nullptr;
    }
    audioDecoder_ = std::move(ad);
    return audioDecoder_.get();
}

VideoDecoder* AERSession::ensureThumbnailDecoder() {
    std::lock_guard<std::mutex> lock(thumbInitMutex_);
    if (thumbDecoder_) {
        return thumbDecoder_.get();
    }
    AVFormatContext* thumbCtx = nullptr;
    MediaMetadata thumbMeta{};
    if (!DemuxEngine::probeFileMetadata(filePath_, thumbMeta, &thumbCtx)) {
        if (thumbCtx) avformat_close_input(&thumbCtx);
        return videoDecoder_.get();
    }
    auto td = std::make_unique<VideoDecoder>(sessionId_ + 1000000ULL, filePath_);
    if (!td->open(thumbCtx, thumbMeta, /*lowResourceThumbnailMode=*/true)) {
        return videoDecoder_.get();
    }
    thumbDecoder_ = std::move(td);
    return thumbDecoder_.get();
}

void AERSession::quiet() {
    if (videoDecoder_) {
        videoDecoder_->quietHardwareSession();
    }
    {
        std::lock_guard<std::mutex> lock(thumbInitMutex_);
        if (thumbDecoder_) {
            thumbDecoder_->close();
            thumbDecoder_.reset();
        }
    }
    {
        std::lock_guard<std::mutex> lock(audioInitMutex_);
        if (audioDecoder_ && meta_.hasVideo) {
            audioDecoder_->close();
            audioDecoder_.reset();
        }
    }
    LRUFrameCache::instance().evictSession(sessionId_);
    LRUFrameCache::instance().evictSession(sessionId_ + 1000000ULL);
    if (VideoDecoder::activeHardwareStreamCount() == 0) {
        GpuDeviceManager::instance().trimIdleResources();
    }
}

void AERSession::close() {
    {
        std::lock_guard<std::mutex> lock(thumbInitMutex_);
        if (thumbDecoder_) {
            thumbDecoder_->close();
            thumbDecoder_.reset();
        }
    }
    if (videoDecoder_) {
        videoDecoder_->close();
        videoDecoder_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(audioInitMutex_);
        if (audioDecoder_) {
            audioDecoder_->close();
            audioDecoder_.reset();
        }
    }
    LRUFrameCache::instance().evictSession(sessionId_);
    LRUFrameCache::instance().evictSession(sessionId_ + 1000000ULL);
    if (VideoDecoder::activeHardwareStreamCount() == 0) {
        GpuDeviceManager::instance().trimIdleResources();
    }
    meta_ = MediaMetadata{};
}

std::string AERSession::activeVideoBackend() const {
    return videoDecoder_ ? videoDecoder_->activeBackendName() : "none";
}

std::string AERSession::videoFallbackReason() const {
    return videoDecoder_ ? videoDecoder_->fallbackReason() : "none";
}

bool AERSession::isUsingHardwareDecode() const {
    return videoDecoder_ && videoDecoder_->isUsingHardware();
}

size_t AERSession::keyframeCount() const {
    return videoDecoder_ ? videoDecoder_->seekIndex().keyframeCount() : 0;
}

uint64_t AERSession::bumpScrubGeneration() noexcept {
    return videoDecoder_ ? videoDecoder_->bumpScrubGeneration() : 0;
}

uint64_t AERSession::currentScrubGeneration() const noexcept {
    return videoDecoder_ ? videoDecoder_->currentScrubGeneration() : 0;
}

int64_t AERSession::nearestKeyframeFrame(int64_t frameNumber) const {
    return videoDecoder_ ? videoDecoder_->nearestKeyframeFrame(frameNumber) : frameNumber;
}

void AERSession::simulateHardwareFailureNextDecode() {
    if (videoDecoder_) {
        videoDecoder_->simulateHardwareFailureNextDecode();
    }
}

uint32_t AERSession::activeGpuStreams() {
    return VideoDecoder::activeHardwareStreamCount();
}

std::string AERSession::telemetryStatusJsonPath() {
    return AERTelemetry::instance().statusFilePath();
}

size_t AERSession::totalCachedVramBytes() {
    return LRUFrameCache::instance().totalVramBytes();
}

size_t AERSession::totalCachedVramFrames() {
    return LRUFrameCache::instance().totalVramEntries();
}

size_t AERSession::totalCachedBytes() {
    return LRUFrameCache::instance().totalRamBytes();
}

size_t AERSession::totalCachedFrames() {
    return LRUFrameCache::instance().totalRamEntries();
}

bool AERSession::renderVideoFrame(int64_t frameNumber,
                                  int outWidth,
                                  int outHeight,
                                  int outRowBytes,
                                  PixelBufferFormat outFormat,
                                  uint8_t* outPixels,
                                  bool flipVertical,
                                  bool fastKeyframeScrub,
                                  uint64_t expectedGeneration) {
    if (!videoDecoder_ || !meta_.hasVideo) {
        return false;
    }
    // Isolate small thumbnail requests (<= 640px when source is larger) onto a separate decoder
    // so Premiere's Timeline clip thumbnails NEVER lock or flush the active Timeline playhead decoder!
    if (outWidth <= 640 && outHeight <= 640 && (meta_.width > 640 || meta_.height > 640)) {
        if (VideoDecoder* td = ensureThumbnailDecoder()) {
            return td->decodeFrame(frameNumber, outWidth, outHeight, outRowBytes,
                                   outFormat, outPixels, flipVertical, false, 0);
        }
    }
    return videoDecoder_->decodeFrame(frameNumber, outWidth, outHeight, outRowBytes,
                                      outFormat, outPixels, flipVertical, fastKeyframeScrub, expectedGeneration);
}

bool AERSession::renderAudioSamples(int64_t startSampleFrame,
                                    uint32_t sampleFrameCount,
                                    float* const* outChannelBuffers) {
    AudioDecoder* ad = ensureAudioDecoder();
    if (!ad) {
        return false;
    }
    return ad->readPlanarFloatSamples(startSampleFrame, sampleFrameCount, outChannelBuffers);
}

bool AERSession::renderAudioPeaks(int64_t startSampleFrame,
                                  float peakSampleRate,
                                  int32_t numPeakFrames,
                                  float** outMinima,
                                  float** outMaxima) {
    AudioDecoder* ad = ensureAudioDecoder();
    if (!ad) {
        return false;
    }
    return ad->computePeakAudio(startSampleFrame, peakSampleRate, numPeakFrames, outMinima, outMaxima);
}

} // namespace aer
