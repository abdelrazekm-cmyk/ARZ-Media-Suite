#pragma once

#include <cstdint>
#include <string>

struct AVFormatContext;
struct AVStream;

namespace aer {

struct MediaMetadata {
    bool valid = false;
    bool hasVideo = false;
    bool hasAudio = false;
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;

    int width = 0;
    int height = 0;
    uint32_t parNum = 1;
    uint32_t parDen = 1;
    int64_t fpsNum = 30000;
    int64_t fpsDen = 1001;
    int64_t totalFrames = 0;
    int bitDepth = 8;
    bool hasAlpha = false;
    bool isInterlaced = false;
    bool topFieldFirst = true;
    bool isBt2020 = false;
    bool isBt601 = false;
    bool isHdr = false;
    int rotationDegrees = 0;

    int64_t videoStartPts = 0;
    int64_t videoFrameStepPts = 1;
    int32_t vidTimebaseNum = 1;
    int32_t vidTimebaseDen = 30000;

    int audioSampleRate = 48000;
    int sourceAudioChannels = 2;
    int audioChannels = 2; // Host-safe Premiere channel count (1, 2, or 6 for 5.1/7.1)
    int64_t totalAudioSampleFrames = 0;

    std::string containerName;
    std::string fileExtensionLower;
    std::string videoCodecName;
    std::string videoProfileName;
    std::string audioCodecName;
    std::string colorSpaceSummary;
    std::string timecodeString;
};

class DemuxEngine {
public:
    static std::string wideToUtf8(const std::wstring& wstr);
    static std::string extractExtensionLower(const std::wstring& filePath);
    static AVFormatContext* openFormatContextUtf8(const std::string& utf8Path);

    static bool probeFileMetadata(const std::wstring& filePath,
                                  MediaMetadata& outMeta,
                                  AVFormatContext** outKeepVideoCtx = nullptr);
};

} // namespace aer
