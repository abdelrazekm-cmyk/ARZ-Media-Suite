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

    if (!meta_.hasVideo && meta_.hasAudio) {
        if (!ensureAudioDecoder()) {
            meta_.hasAudio = false;
        }
    }

    if (!meta_.hasVideo && !meta_.hasAudio) {
        close();
        return false;
    }

    if (meta_.hasVideo && meta_.totalFrames > 2 && AERConfig::instance().enableFrameCache) {
        startLookaheadWorker();
    }
    return true;
}

void AERSession::startLookaheadWorker() {
    std::lock_guard<std::mutex> lock(lookaheadMutex_);
    if (lookaheadRunning_) return;
    lookaheadStop_ = false;
    lookaheadTarget_.active = false;
    lookaheadRunning_ = true;
    lookaheadThread_ = std::thread(&AERSession::lookaheadLoop, this);
}

void AERSession::stopLookaheadWorker() {
    {
        std::lock_guard<std::mutex> lock(lookaheadMutex_);
        if (!lookaheadRunning_) return;
        lookaheadStop_ = true;
        lookaheadTarget_.active = false;
    }
    lookaheadCv_.notify_all();
    if (lookaheadThread_.joinable()) {
        lookaheadThread_.join();
    }
    lookaheadRunning_ = false;
}

void AERSession::triggerLookahead(int64_t anchorFrame, int width, int height, PixelBufferFormat fmt, uint64_t generation) {
    if (!lookaheadRunning_ || !AERConfig::instance().enableFrameCache) return;
    {
        std::lock_guard<std::mutex> lock(lookaheadMutex_);
        lookaheadTarget_.anchorFrame = anchorFrame;
        lookaheadTarget_.width = width;
        lookaheadTarget_.height = height;
        lookaheadTarget_.format = fmt;
        lookaheadTarget_.generation = generation;
        lookaheadTarget_.active = true;
    }
    lookaheadCv_.notify_one();
}

void AERSession::lookaheadLoop() {
    while (true) {
        LookaheadTarget target{};
        {
            std::unique_lock<std::mutex> lock(lookaheadMutex_);
            lookaheadCv_.wait(lock, [this]() { return lookaheadStop_ || lookaheadTarget_.active; });
            if (lookaheadStop_) break;
            target = lookaheadTarget_;
            lookaheadTarget_.active = false;
        }

        if (!videoDecoder_) continue;

        const VramStatus vst = GpuDeviceManager::instance().queryVramStatus(LRUFrameCache::instance().totalVramBytes());
        const AdaptiveHardwareBudgets budgets = AERConfig::instance()
                                                    .computeAdaptiveBudgets(vst.budgetBytes, vst.currentUsageBytes, LRUFrameCache::instance().totalVramBytes());
        const size_t singleFrameBytes = std::max<size_t>(1, estimateTightFrameBytes(target.width, target.height, target.format));
        const int maxSafeByCacheCapacity = std::max<int>(2, static_cast<int>(budgets.l1VramCacheBytes / singleFrameBytes) - 3);
        const int dynamicWindow = std::min(budgets.lookaheadFrames, maxSafeByCacheCapacity);

        for (int i = 0; i < dynamicWindow; ++i) {
            {
                std::lock_guard<std::mutex> lock(lookaheadMutex_);
                if (lookaheadStop_ || lookaheadTarget_.active) {
                    break;
                }
            }
            if (target.generation != 0 && target.generation != videoDecoder_->currentScrubGeneration()) {
                break;
            }
            int64_t decodedNum = -1;
            if (!videoDecoder_->decodeNextLookaheadFrame(
                    target.width, target.height, target.format, target.generation, decodedNum)) {
                break;
            }
            if (decodedNum < 0 || decodedNum >= target.anchorFrame + dynamicWindow) {
                break;
            }
        }
    }
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
    bumpScrubGeneration();
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
    stopLookaheadWorker();
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

void AERSession::clearAllCaches() {
    LRUFrameCache::instance().clear();
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
    if (outWidth <= 640 && outHeight <= 640 && (meta_.width > 640 || meta_.height > 640)) {
        if (VideoDecoder* td = ensureThumbnailDecoder()) {
            return td->decodeFrame(frameNumber, outWidth, outHeight, outRowBytes,
                                   outFormat, outPixels, flipVertical, false, 0);
        }
    }
    const uint64_t genSnapshot = (expectedGeneration != 0) ? expectedGeneration : videoDecoder_->currentScrubGeneration();
    const bool ok = videoDecoder_->decodeFrame(frameNumber, outWidth, outHeight, outRowBytes,
                                               outFormat, outPixels, flipVertical, fastKeyframeScrub, expectedGeneration);
    if (ok && outPixels != nullptr && !fastKeyframeScrub && outWidth >= (meta_.width / 2)) {
        triggerLookahead(frameNumber, outWidth, outHeight, outFormat, genSnapshot);
    }
    return ok;
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
