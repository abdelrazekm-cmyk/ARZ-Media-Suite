#include "cache/GOPSeekIndex.hpp"

#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace aer {

int64_t GOPSeekIndex::frameToPtsLocked(int64_t frameNumber) const {
    if (frameNumber <= 0) {
        return startPts_;
    }
    if (fpsNum_ > 0 && fpsDen_ > 0 && tbNum_ > 0 && tbDen_ > 0) {
        return startPts_ + av_rescale_q(
            frameNumber,
            AVRational{static_cast<int>(fpsDen_), static_cast<int>(fpsNum_)},
            AVRational{tbNum_, tbDen_});
    }
    return startPts_ + frameNumber * frameStepPts_;
}

int64_t GOPSeekIndex::ptsToFrameLocked(int64_t pts) const {
    const int64_t relPts = std::max<int64_t>(0, pts - startPts_);
    if (fpsNum_ > 0 && fpsDen_ > 0 && tbNum_ > 0 && tbDen_ > 0) {
        return std::max<int64_t>(0, av_rescale_q_rnd(
            relPts,
            AVRational{tbNum_, tbDen_},
            AVRational{static_cast<int>(fpsDen_), static_cast<int>(fpsNum_)},
            AV_ROUND_NEAR_INF));
    }
    return (relPts + (frameStepPts_ / 2)) / frameStepPts_;
}

void GOPSeekIndex::importStreamIndexEntriesLocked(AVStream* stream) {
    if (!stream) {
        return;
    }
    const int count = avformat_index_get_entries_count(stream);
    if (count <= 0 || count == lastImportedIndexCount_) {
        return;
    }
    lastImportedIndexCount_ = count;

    keyframes_.reserve(static_cast<size_t>(std::max(count, 32)));
    for (int i = 0; i < count; ++i) {
        const AVIndexEntry* entry = avformat_index_get_entry(stream, i);
        if (!entry) continue;
        if ((entry->flags & AVINDEX_KEYFRAME) != 0 && entry->timestamp != AV_NOPTS_VALUE) {
            const int64_t ts = entry->timestamp;
            const int64_t approxFrame = ptsToFrameLocked(ts);
            if (keyframes_.empty() || keyframes_.back().pts < ts) {
                keyframes_.push_back(KeyframeAnchor{ts, approxFrame});
            } else {
                auto it = std::lower_bound(
                    keyframes_.begin(), keyframes_.end(), ts,
                    [](const KeyframeAnchor& a, int64_t val) { return a.pts < val; });
                if (it == keyframes_.end() || it->pts != ts) {
                    keyframes_.insert(it, KeyframeAnchor{ts, approxFrame});
                }
            }
        }
    }
}

void GOPSeekIndex::initializeFromStream(AVStream* stream,
                                        int64_t startPts,
                                        int64_t frameStepPts,
                                        int64_t fpsNum,
                                        int64_t fpsDen,
                                        int32_t tbNum,
                                        int32_t tbDen) {
    std::lock_guard<std::mutex> lock(mutex_);
    startPts_ = startPts;
    frameStepPts_ = frameStepPts > 0 ? frameStepPts : 1;
    fpsNum_ = fpsNum > 0 ? fpsNum : 30000;
    fpsDen_ = fpsDen > 0 ? fpsDen : 1001;
    tbNum_ = tbNum > 0 ? tbNum : 1;
    tbDen_ = tbDen > 0 ? tbDen : 1000;
    lastImportedIndexCount_ = 0;
    keyframes_.clear();
    observedFramePts_.clear();

    importStreamIndexEntriesLocked(stream);
}

void GOPSeekIndex::refreshFromStream(AVStream* stream) {
    if (!stream) return;
    std::lock_guard<std::mutex> lock(mutex_);
    importStreamIndexEntriesLocked(stream);
}

void GOPSeekIndex::recordObservedFrame(int64_t frameNumber, int64_t pts, bool isKeyframe) {
    if (pts == AV_NOPTS_VALUE || frameNumber < 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (observedFramePts_.size() < 250000) {
        observedFramePts_[frameNumber] = pts;
    }
    if (isKeyframe) {
        auto it = std::lower_bound(
            keyframes_.begin(), keyframes_.end(), pts,
            [](const KeyframeAnchor& a, int64_t val) { return a.pts < val; });
        if (it == keyframes_.end() || it->pts != pts) {
            keyframes_.insert(it, KeyframeAnchor{pts, frameNumber});
        }
    }
}

int64_t GOPSeekIndex::frameToTargetPts(int64_t frameNumber) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto exact = observedFramePts_.find(frameNumber);
    if (exact != observedFramePts_.end()) {
        return exact->second;
    }
    // Interpolate from nearby observed VFR frame if within 8 frames; otherwise exact rational rescale
    if (!observedFramePts_.empty()) {
        auto it = observedFramePts_.lower_bound(frameNumber);
        if (it != observedFramePts_.begin()) {
            auto prev = std::prev(it);
            const int64_t deltaFrames = frameNumber - prev->first;
            if (deltaFrames >= 0 && deltaFrames <= 8) {
                const int64_t deltaPts = frameToPtsLocked(frameNumber) - frameToPtsLocked(prev->first);
                return prev->second + deltaPts;
            }
        }
    }
    return frameToPtsLocked(frameNumber);
}

int64_t GOPSeekIndex::ptsToApproxFrame(int64_t pts) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ptsToFrameLocked(pts);
}

std::optional<int64_t> GOPSeekIndex::nearestKeyframePtsBefore(int64_t targetPts,
                                                              int64_t maxLookbackFrames) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (keyframes_.empty()) {
        return std::nullopt;
    }
    auto it = std::upper_bound(
        keyframes_.begin(), keyframes_.end(), targetPts,
        [](int64_t val, const KeyframeAnchor& a) { return val < a.pts; });
    if (it == keyframes_.begin()) {
        return std::nullopt;
    }
    --it;
    const int64_t maxLookbackPts =
        frameToPtsLocked(std::max<int64_t>(1, maxLookbackFrames)) - startPts_;
    if (targetPts - it->pts > maxLookbackPts) {
        // Known keyframe is too far behind (e.g. Matroska Cues not loaded yet);
        // let avformat_seek_file jump directly to targetPts via container Cues!
        return std::nullopt;
    }
    return it->pts;
}

bool GOPSeekIndex::shouldDecodeForward(int64_t currentDecoderPts,
                                       int64_t targetPts,
                                       int64_t maxForwardFrames) const {
    if (currentDecoderPts == AV_NOPTS_VALUE || targetPts < currentDecoderPts) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t ptsGap = targetPts - currentDecoderPts;
    const int64_t maxGap = frameToPtsLocked(std::max<int64_t>(1, maxForwardFrames)) - startPts_;
    if (ptsGap <= maxGap) {
        return true;
    }
    // Only step forward further if keyframes ARE populated around this region and no keyframe lies ahead
    auto it = std::upper_bound(
        keyframes_.begin(), keyframes_.end(), currentDecoderPts,
        [](int64_t val, const KeyframeAnchor& a) { return val < a.pts; });
    if (it == keyframes_.end()) {
        return false; // Keyframes ahead are unknown (unloaded index); always container-seek!
    }
    if (it->pts <= targetPts) {
        return false; // A closer keyframe exists ahead; seek to it instead!
    }
    return ptsGap <= maxGap * 3;
}

size_t GOPSeekIndex::keyframeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keyframes_.size();
}

} // namespace aer
