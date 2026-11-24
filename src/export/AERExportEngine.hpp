#pragma once

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
  #if defined(AER_CORE_EXPORTS)
    #define AER_API __declspec(dllexport)
  #else
    #define AER_API __declspec(dllimport)
  #endif
#else
  #define AER_API
#endif

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwrContext;
struct SwsContext;

namespace aer {

enum class ExportVideoCodec : int32_t {
    AV1_NVENC = 0,
    AV1_SVT = 1,
    HEVC_NVENC = 2,
    H264_NVENC = 3,
    H264_SW = 4,
    VP9 = 5,
    PRORES_422_HQ = 6,
    PRORES_4444_ALPHA = 7,
    DNXHR_HQ = 8,
    CINEFORM = 9,
    FFV1_LOSSLESS = 10,
    MJPEG = 11,
    GIF = 12,
    WEBP_ANIM = 13,
    APNG = 14
};

enum class ExportAudioCodec : int32_t {
    OPUS = 0,
    FLAC = 1,
    AAC = 2,
    VORBIS = 3,
    MP3 = 4,
    ALAC = 5,
    AC3 = 6,
    EAC3 = 7,
    PCM_S24 = 8,
    PCM_F32 = 9
};

enum class ExportChromaBitDepth : int32_t {
    Bit8_420 = 0,
    Bit10_420_HDR = 1,
    Bit10_422 = 2,
    Alpha_4444 = 3
};

enum class ExportColorSpace : int32_t {
    Rec709_SDR = 0,
    Rec2020_PQ_HDR10 = 1,
    Rec2020_HLG = 2
};

struct ExportJobConfig {
    std::wstring outputPath;
    std::string containerExtension = "mp4"; // mp4, mkv, webm, mov, avi, gif, webp, apng, opus, flac, wav, mp3, aac, aiff

    bool enableVideo = true;
    ExportVideoCodec videoCodec = ExportVideoCodec::AV1_NVENC;
    int width = 1920;
    int height = 1080;
    int64_t fpsNum = 24000;
    int64_t fpsDen = 1001;
    int parNum = 1;
    int parDen = 1;
    bool useCrfMode = true;
    int crfQuality = 23;          // 0..51
    double targetBitrateMbps = 18.0;
    int speedPreset = 4;          // 1 (fastest) .. 7 (best quality)
    ExportChromaBitDepth bitDepthMode = ExportChromaBitDepth::Bit8_420;
    ExportColorSpace colorSpace = ExportColorSpace::Rec709_SDR;

    bool enableAudio = true;
    ExportAudioCodec audioCodec = ExportAudioCodec::OPUS;
    int audioSampleRate = 48000;
    int audioChannels = 2;        // 1, 2, or 6 (5.1)
    int audioBitrateKbps = 192;
};

class AER_API AERExportEngine {
public:
    AERExportEngine();
    ~AERExportEngine();

    bool open(const ExportJobConfig& cfg);

    // Accepts BGRA_8u pixels (either top-down when bottomUpInput=false,
    // or Premiere PPix bottom-up DIB when bottomUpInput=true with ZERO extra copy!)
    bool writeVideoFrameBgra8(const uint8_t* bgraPixels,
                              int rowBytes,
                              bool bottomUpInput);

    // Accepts non-interleaved 32-bit float audio channels from Premiere SequenceAudioSuite
    bool writeAudioSamplesFloatPlanar(const float* const* channelBuffers,
                                      uint32_t sampleFrames);

    bool finalize();
    void abortAndClose();

    const std::string& lastError() const noexcept { return lastError_; }
    const std::string& activeVideoEncoderName() const noexcept { return activeVidEncName_; }
    const std::string& activeAudioEncoderName() const noexcept { return activeAudEncName_; }

private:
    bool initVideoStream();
    bool initAudioStream();
    bool drainPackets(AVCodecContext* encCtx, AVStream* stream);
    bool encodeNextAudioFrameFromFifo(bool flushRemainder);

    ExportJobConfig cfg_{};
    bool opened_ = false;
    bool headerWritten_ = false;
    std::string lastError_;
    std::string activeVidEncName_ = "none";
    std::string activeAudEncName_ = "none";

    AVFormatContext* fmtCtx_ = nullptr;
    AVStream* vidStream_ = nullptr;
    AVCodecContext* vidEncCtx_ = nullptr;
    AVFrame* vidFrame_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    int64_t nextVideoPts_ = 0;

    AVStream* audStream_ = nullptr;
    AVCodecContext* audEncCtx_ = nullptr;
    AVFrame* audFrame_ = nullptr;
    SwrContext* swrCtx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int64_t nextAudioPts_ = 0;
    int effectiveAudRate_ = 48000;
    std::vector<std::vector<float>> audioFifo_;
    size_t audioFifoSamples_ = 0;
};

} // namespace aer

extern "C" AER_API int32_t xAERExportEntry(
    int32_t selector,
    void* stdParms,
    void* param1,
    void* param2);
