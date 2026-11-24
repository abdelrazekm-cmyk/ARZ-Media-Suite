#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

struct AVStream;

namespace aer {

struct KeyframeAnchor {
    int64_t pts = 0;
    int64_t approxFrame = 0;
};

class GOPSeekIndex {
public:
    GOPSeekIndex() = default;

    void initializeFromStream(AVStream* stream,
                              int64_t startPts,
                              int64_t frameStepPts,
                              int64_t fpsNum,
                              int64_t fpsDen,
                              int32_t tbNum,
                              int32_t tbDen);

    // Imports any newly loaded container index entries (e.g. lazy Matroska Cues loaded on first seek)
    void refreshFromStream(AVStream* stream);

    void recordObservedFrame(int64_t frameNumber, int64_t pts, bool isKeyframe);

    int64_t frameToTargetPts(int64_t frameNumber) const;
    int64_t ptsToApproxFrame(int64_t pts) const;

    // Returns a known keyframe anchor ONLY if it lies within maxLookbackFrames of targetPts;
    // otherwise returns nullopt so avformat_seek_file uses container Cues directly at targetPts!
    std::optional<int64_t> nearestKeyframePtsBefore(int64_t targetPts,
                                                    int64_t maxLookbackFrames = 300) const;

    bool shouldDecodeForward(int64_t currentDecoderPts,
                             int64_t targetPts,
                             int64_t maxForwardFrames = 24) const;

    size_t keyframeCount() const;

private:
    void importStreamIndexEntriesLocked(AVStream* stream);
    int64_t frameToPtsLocked(int64_t frameNumber) const;
    int64_t ptsToFrameLocked(int64_t pts) const;

    mutable std::mutex mutex_;
    int64_t startPts_ = 0;
    int64_t frameStepPts_ = 1;
    int64_t fpsNum_ = 30000;
    int64_t fpsDen_ = 1001;
    int32_t tbNum_ = 1;
    int32_t tbDen_ = 1000;
    int lastImportedIndexCount_ = 0;
    std::atomic<bool> cuesHarvested_{false};
    std::vector<KeyframeAnchor> keyframes_;
    std::map<int64_t, int64_t> observedFramePts_;
};

} // namespace aer
