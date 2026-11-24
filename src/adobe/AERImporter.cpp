#include "core/AERSession.hpp"

#ifndef PRWIN_ENV
#define PRWIN_ENV 1
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <PrSDKAsyncImporter.h>
#include <PrSDKAudioSuite.h>
#include <PrSDKImport.h>
#include <PrSDKMALErrors.h>
#include <PrSDKPPix2Suite.h>
#include <PrSDKPPixCacheSuite.h>
#include <PrSDKPPixCreatorSuite.h>
#include <PrSDKPPixSuite.h>
#include <PrSDKPixelFormat.h>
#include <PrSDKStringSuite.h>
#include <PrSDKTimeSuite.h>
#include <SPBasic.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct FormatDescriptor {
    csSDK_int32 fourcc;
    const char* longName;
    const char* shortName;
    const char* extensions; // double-null terminated
    size_t extBytes;
};

constexpr csSDK_int32 makeFourCC(char a, char b, char c, char d) {
    return (static_cast<csSDK_int32>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<csSDK_int32>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<csSDK_int32>(static_cast<uint8_t>(c)) << 8) |
           (static_cast<csSDK_int32>(static_cast<uint8_t>(d)));
}

// 27 Format Groups covering 59 individual media extensions (ARZ Identity)
static const FormatDescriptor kSupportedFormats[] = {
    {makeFourCC('A', 'R', 'Z', ' '), "ARZ Universal Media", "ARZ Media", "arz\0aer\0\0", 9},
    {makeFourCC('m', 'k', 'v', ' '), "Matroska Video / Audio (ARZ)", "MKV", "mkv\0mk3d\0mka\0mks\0\0", 18},
    {makeFourCC('w', 'e', 'b', 'm'), "WebM / WebP Media (ARZ)", "WebM", "webm\0weba\0webp\0\0", 16},
    {makeFourCC('m', 'p', 'g', '4'), "MPEG-4 Container (ARZ)", "MP4", "mp4\0m4v\0m4a\03gp\0\0", 17},
    {makeFourCC('M', 'o', 'o', 'V'), "QuickTime / ProRes / CineForm / DNxHR / DXV (ARZ)", "MOV", "mov\0qt\0\0", 8},
    {makeFourCC('V', 'f', 'w', ' '), "Audio Video Interleave (ARZ)", "AVI", "avi\0\0", 5},
    {makeFourCC('w', 'm', 'v', '3'), "Windows Media Video / ASF (ARZ)", "WMV", "wmv\0asf\0wma\0\0", 13},
    {makeFourCC('o', 'g', 'g', ' '), "Ogg / Opus / Vorbis / Speex (ARZ)", "OGG", "ogg\0ogv\0opus\0spx\0\0", 18},
    {makeFourCC('F', 'L', 'A', 'C'), "Free Lossless Audio (ARZ)", "FLAC", "flac\0\0", 6},
    {makeFourCC('D', 'T', 'S', ' '), "DTS / Dolby AC-3 Audio (ARZ)", "DTS/AC3", "dts\0dtshd\0ac3\0eac3\0\0", 20},
    {makeFourCC('f', 'l', 'v', ' '), "Flash Video (ARZ)", "FLV", "flv\0f4v\0\0", 9},
    {makeFourCC('m', 'x', 'f', ' '), "Material Exchange / DNxHR / ProRes (ARZ)", "MXF", "mxf\0\0", 5},
    {makeFourCC('t', 's', ' ', ' '), "MPEG Transport / Blu-ray Stream (ARZ)", "MPEG-TS", "ts\0m2ts\0mts\0vob\0mpg\0mpeg\0\0", 26},
    {makeFourCC('I', 'V', 'F', ' '), "Raw AV1 / HEVC / H.264 / IVF (ARZ)", "IVF/RAW", "ivf\0av1\0hevc\0h265\0h264\0265\0264\0\0", 32},
    {makeFourCC('R', 'M', 'V', 'B'), "RealMedia Variable Bitrate (ARZ)", "RMVB", "rm\0rmvb\0\0", 9},
    {makeFourCC('Y', 'V', '1', '2'), "YUV4MPEG2 Raw Stream (ARZ)", "Y4M", "y4m\0\0", 5},
    {makeFourCC('G', 'I', 'F', ' '), "GIF Animation (ARZ)", "GIF", "gif\0\0", 5},
    {makeFourCC('M', 'J', 'P', 'G'), "Motion JPEG Raw Video (ARZ)", "MJPEG", "mjpeg\0mjpg\0\0", 12},
    {makeFourCC('J', 'X', 'L', ' '), "JPEG XL Image (ARZ)", "JXL", "jxl\0\0", 5},
    {makeFourCC('Q', 'O', 'I', ' '), "Quite OK Image (ARZ)", "QOI", "qoi\0\0", 5},
    {makeFourCC('A', 'P', 'N', 'G'), "Animated PNG (ARZ)", "APNG", "apng\0\0", 6},
    {makeFourCC('B', 'I', 'N', 'K'), "Bink Video (ARZ)", "BINK", "bik\0bk2\0\0", 9},
    {makeFourCC('3', '6', '0', ' '), "GoPro 360 Spherical Video (ARZ)", "360", "360\0\0", 5},
    {makeFourCC('W', 'A', 'V', 'E'), "Waveform Audio (ARZ)", "WAV", "wav\0\0", 5},
    {makeFourCC('M', 'P', '3', ' '), "MPEG Layer III Audio (ARZ)", "MP3", "mp3\0\0", 5},
    {makeFourCC('A', 'A', 'C', ' '), "Advanced Audio Coding (ARZ)", "AAC", "aac\0\0", 5},
    {makeFourCC('A', 'I', 'F', 'F'), "Audio Interchange File (ARZ)", "AIFF", "aiff\0aif\0\0", 10},
};

struct AERInstanceRec {
    uint32_t magic = 0x41455231; // 'AER1'
    csSDK_int32 importerID = 0;
    bool highPriority = false;
    SPBasicSuite* basicSuite = nullptr;
    PrSDKPPixCreatorSuite* ppixCreatorSuite = nullptr;
    PrSDKPPixCacheSuite* ppixCacheSuite = nullptr;
    PrSDKPPixSuite* ppixSuite = nullptr;
    PrSDKTimeSuite* timeSuite = nullptr;
    PrSDKStringSuite* stringSuite = nullptr;
    aer::AERSession* session = nullptr;

    void acquireSuites(imStdParms* stdParms) {
        if (!stdParms || !stdParms->piSuites || !stdParms->piSuites->utilFuncs) return;
        if (!basicSuite) {
            basicSuite = stdParms->piSuites->utilFuncs->getSPBasicSuite();
        }
        if (!basicSuite) return;
        if (!ppixCreatorSuite) {
            basicSuite->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion,
                                     const_cast<const void**>(reinterpret_cast<void**>(&ppixCreatorSuite)));
        }
        if (!ppixCacheSuite) {
            basicSuite->AcquireSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion,
                                     const_cast<const void**>(reinterpret_cast<void**>(&ppixCacheSuite)));
        }
        if (!ppixSuite) {
            basicSuite->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion,
                                     const_cast<const void**>(reinterpret_cast<void**>(&ppixSuite)));
        }
        if (!timeSuite) {
            basicSuite->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion,
                                     const_cast<const void**>(reinterpret_cast<void**>(&timeSuite)));
        }
        if (!stringSuite) {
            basicSuite->AcquireSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion,
                                     const_cast<const void**>(reinterpret_cast<void**>(&stringSuite)));
        }
    }

    void releaseSuites() {
        if (!basicSuite) return;
        if (ppixCreatorSuite) {
            basicSuite->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
            ppixCreatorSuite = nullptr;
        }
        if (ppixCacheSuite) {
            basicSuite->ReleaseSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion);
            ppixCacheSuite = nullptr;
        }
        if (ppixSuite) {
            basicSuite->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
            ppixSuite = nullptr;
        }
        if (timeSuite) {
            basicSuite->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
            timeSuite = nullptr;
        }
        if (stringSuite) {
            basicSuite->ReleaseSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion);
            stringSuite = nullptr;
        }
    }
};

using AERInstanceHandle = AERInstanceRec**;

std::wstring prUtf16ToWide(const prUTF16Char* p) {
    if (!p) return {};
    return std::wstring(reinterpret_cast<const wchar_t*>(p));
}

void negotiateFrameFormat(const imSourceVideoRec* videoRec,
                          const aer::MediaMetadata& meta,
                          PrPixelFormat& outChosenFormat,
                          int& outDstW,
                          int& outDstH) {
    outChosenFormat = PrPixelFormat_BGRA_4444_8u;
    outDstW = meta.width;
    outDstH = meta.height;

    if (videoRec && videoRec->inNumFrameFormats > 0 && videoRec->inFrameFormats) {
        bool matchedExplicit = false;
        for (csSDK_int32 i = 0; i < videoRec->inNumFrameFormats; ++i) {
            const auto& cand = videoRec->inFrameFormats[i];
            if (cand.inPixelFormat == PrPixelFormat_BGRA_4444_8u ||
                cand.inPixelFormat == PrPixelFormat_BGRA_4444_32f) {
                outChosenFormat = cand.inPixelFormat;
                outDstW = cand.inFrameWidth > 0 ? cand.inFrameWidth : meta.width;
                outDstH = cand.inFrameHeight > 0 ? cand.inFrameHeight : meta.height;
                matchedExplicit = true;
                break;
            }
        }
        if (!matchedExplicit) {
            const auto& first = videoRec->inFrameFormats[0];
            if (first.inFrameWidth > 0 && first.inFrameHeight > 0) {
                outDstW = first.inFrameWidth;
                outDstH = first.inFrameHeight;
            }
        }
    }
}

prMALError populatePPixFromSession(aer::AERSession* session,
                                   PrSDKPPixCreatorSuite* creatorSuite,
                                   PrSDKPPixSuite* ppixSuite,
                                   PrSDKPPixCacheSuite* cacheSuite,
                                   csSDK_int32 importerID,
                                   int64_t frameNumber,
                                   imSourceVideoRec* videoRec,
                                   bool fastKeyframeScrub,
                                   uint64_t expectedGeneration) {
    if (!session || !creatorSuite || !ppixSuite || !videoRec || !videoRec->outFrame) {
        return imOtherErr;
    }

    const auto& meta = session->metadata();
    if (!meta.hasVideo) return imOtherErr;

    if (cacheSuite && videoRec->inNumFrameFormats > 0) {
        if (cacheSuite->GetFrameFromCache(
                importerID,
                0,
                static_cast<csSDK_int32>(frameNumber),
                videoRec->inNumFrameFormats,
                videoRec->inFrameFormats,
                videoRec->outFrame,
                nullptr,
                0) == suiteError_NoError) {
            return malNoError;
        }
    }

    PrPixelFormat chosenFormat = PrPixelFormat_BGRA_4444_8u;
    int dstW = meta.width;
    int dstH = meta.height;
    negotiateFrameFormat(videoRec, meta, chosenFormat, dstW, dstH);

    prRect bounds{};
    prSetRect(&bounds, 0, 0, dstW, dstH);
    if (creatorSuite->CreatePPix(
            videoRec->outFrame, PrPPixBufferAccess_ReadWrite, chosenFormat, &bounds) != suiteError_NoError ||
        !*videoRec->outFrame) {
        return imMemErr;
    }

    char* dstPixels = nullptr;
    csSDK_int32 rowBytes = 0;
    ppixSuite->GetPixels(*videoRec->outFrame, PrPPixBufferAccess_ReadWrite, &dstPixels);
    ppixSuite->GetRowBytes(*videoRec->outFrame, &rowBytes);

    const aer::PixelBufferFormat aerFmt =
        (chosenFormat == PrPixelFormat_BGRA_4444_32f)
            ? aer::PixelBufferFormat::BGRA_32f
            : aer::PixelBufferFormat::BGRA_8u;

    // Premiere PPix BGRA_4444_8u / 32f uses standard Windows bottom-up DIB scanline ordering
    // (row 0 = bottom line of frame when rowBytes > 0; top line when rowBytes < 0).
    // Combined with container rotationDegrees == 180 if present:
    const bool ppixIsBottomUp = (rowBytes > 0);
    const bool mediaRotated180 = (meta.rotationDegrees == 180);
    const bool flipVertical = (ppixIsBottomUp != mediaRotated180);

    if (!dstPixels || rowBytes == 0 ||
        !session->renderVideoFrame(
            frameNumber, dstW, dstH, rowBytes, aerFmt,
            reinterpret_cast<uint8_t*>(dstPixels), flipVertical, fastKeyframeScrub, expectedGeneration)) {
        ppixSuite->Dispose(*videoRec->outFrame);
        *videoRec->outFrame = nullptr;
        return malUnknownError;
    }

    if (cacheSuite) {
        cacheSuite->AddFrameToCache(
            importerID,
            0,
            *videoRec->outFrame,
            static_cast<csSDK_int32>(frameNumber),
            nullptr,
            0);
    }
    return malNoError;
}

// =========================================================================================
// DaVinci Resolve-Class Asynchronous Importer (Latest-Wins Background Prefetch + Cancel)
// =========================================================================================
class AERAsyncImporter {
public:
    AERAsyncImporter(SPBasicSuite* basic,
                     csSDK_int32 importerID,
                     const std::wstring& filePath,
                     bool highPriority)
        : basicSuite_(basic), importerID_(importerID) {
        if (basicSuite_) {
            basicSuite_->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion,
                                      const_cast<const void**>(reinterpret_cast<void**>(&ppixCreatorSuite_)));
            basicSuite_->AcquireSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion,
                                      const_cast<const void**>(reinterpret_cast<void**>(&ppixCacheSuite_)));
            basicSuite_->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion,
                                      const_cast<const void**>(reinterpret_cast<void**>(&ppixSuite_)));
            basicSuite_->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion,
                                      const_cast<const void**>(reinterpret_cast<void**>(&timeSuite_)));
        }
        session_ = std::make_unique<aer::AERSession>();
        valid_ = session_->open(filePath, highPriority);
        if (valid_) {
            worker_ = std::thread(&AERAsyncImporter::workerLoop, this);
        }
    }

    ~AERAsyncImporter() {
        shutdownWorker();
        if (session_) {
            session_->close();
            session_.reset();
        }
        if (basicSuite_) {
            if (ppixCreatorSuite_) basicSuite_->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
            if (ppixCacheSuite_) basicSuite_->ReleaseSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion);
            if (ppixSuite_) basicSuite_->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
            if (timeSuite_) basicSuite_->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
        }
    }

    bool isValid() const noexcept { return valid_; }

    int64_t timeToFrame(PrTime frameTime) const {
        if (!timeSuite_ || !session_) return 0;
        const auto& meta = session_->metadata();
        PrTime ticksPerSec = 0;
        timeSuite_->GetTicksPerSecond(&ticksPerSec);
        if (ticksPerSec > 0 && meta.fpsDen > 0 && meta.fpsNum > 0) {
            const PrTime ticksPerFrame = (ticksPerSec * meta.fpsDen) / meta.fpsNum;
            if (ticksPerFrame > 0) {
                return (frameTime + (ticksPerFrame / 2)) / ticksPerFrame;
            }
        }
        return 0;
    }

    PrTime frameToTime(int64_t frameNum) const {
        if (!timeSuite_ || !session_) return 0;
        const auto& meta = session_->metadata();
        PrTime ticksPerSec = 0;
        timeSuite_->GetTicksPerSecond(&ticksPerSec);
        if (ticksPerSec > 0 && meta.fpsNum > 0) {
            const PrTime ticksPerFrame = (ticksPerSec * meta.fpsDen) / meta.fpsNum;
            return frameNum * ticksPerFrame;
        }
        return 0;
    }

    int onInitiateAsyncRead(const imSourceVideoRec& srcRec) {
        if (!valid_ || !session_) return aiUnknownError;
        const int64_t frameNum = timeToFrame(srcRec.inFrameTime);

        if (ppixCacheSuite_ && srcRec.inNumFrameFormats > 0 && srcRec.inFrameFormats) {
            PPixHand tempPPix = nullptr;
            if (ppixCacheSuite_->GetFrameFromCache(
                    importerID_, 0, static_cast<csSDK_int32>(frameNum),
                    srcRec.inNumFrameFormats, srcRec.inFrameFormats,
                    &tempPPix, nullptr, 0) == suiteError_NoError && tempPPix) {
                if (ppixSuite_) ppixSuite_->Dispose(tempPPix);
                return aiNoError;
            }
        }

        PrPixelFormat chosenFmt = PrPixelFormat_BGRA_4444_8u;
        int dstW = session_->metadata().width;
        int dstH = session_->metadata().height;
        negotiateFrameFormat(&srcRec, session_->metadata(), chosenFmt, dstW, dstH);

        const aer::PixelBufferFormat aerFmt =
            (chosenFmt == PrPixelFormat_BGRA_4444_32f)
                ? aer::PixelBufferFormat::BGRA_32f
                : aer::PixelBufferFormat::BGRA_8u;

        const aer::FrameCacheKey ckey{session_->sessionId(), frameNum, dstW, dstH, aerFmt};
        if (aer::LRUFrameCache::instance().hasCachedFrame(ckey)) {
            return aiNoError;
        }

        // Latest-Wins Queue: bump scrub generation so any obsolete Long-GOP decode aborts immediately!
        const uint64_t gen = session_->bumpScrubGeneration();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            pendingJob_ = AsyncJob{frameNum, dstW, dstH, aerFmt, gen};
        }
        queueCv_.notify_one();
        return aiNoError;
    }

    int onCancelAsyncRead(const imSourceVideoRec& srcRec) {
        if (!valid_ || !session_) return aiNoError;
        const int64_t cancelFrame = timeToFrame(srcRec.inFrameTime);
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (pendingJob_ && pendingJob_->frameNumber == cancelFrame) {
            pendingJob_.reset();
            session_->bumpScrubGeneration();
        }
        return aiNoError;
    }

    int onFlush() {
        if (!valid_ || !session_) return aiNoError;
        session_->bumpScrubGeneration();
        std::unique_lock<std::mutex> lock(queueMutex_);
        pendingJob_.reset();
        doneCv_.wait(lock, [this]() { return !workerBusy_; });
        return aiNoError;
    }

    int onGetFrame(imSourceVideoRec* getRec) {
        if (!valid_ || !session_ || !getRec || !getRec->outFrame) return aiUnknownError;
        const int64_t frameNum = timeToFrame(getRec->inFrameTime);

        const prMALError err = populatePPixFromSession(
            session_.get(), ppixCreatorSuite_, ppixSuite_, ppixCacheSuite_,
            importerID_, frameNum, getRec, /*fastKeyframeScrub=*/false, /*expectedGeneration=*/0);
        return (err == malNoError) ? aiNoError : aiFrameNotFound;
    }

    int onSelectEfficientRenderTime(aiSelectEfficientRenderTimeRec* effRec) {
        if (!valid_ || !session_ || !effRec) return aiUnsupported;
        const int64_t reqFrame = timeToFrame(effRec->ioTime);
        const int64_t keyFrame = session_->nearestKeyframeFrame(reqFrame);
        effRec->ioTime = frameToTime(keyFrame);
        return aiNoError;
    }

private:
    struct AsyncJob {
        int64_t frameNumber = 0;
        int width = 0;
        int height = 0;
        aer::PixelBufferFormat format = aer::PixelBufferFormat::BGRA_8u;
        uint64_t generation = 0;
    };

    void shutdownWorker() {
        if (session_) session_->bumpScrubGeneration();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopWorker_ = true;
            pendingJob_.reset();
        }
        queueCv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void workerLoop() {
        std::vector<uint8_t> scratch;
        while (true) {
            AsyncJob job{};
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCv_.wait(lock, [this]() { return stopWorker_ || pendingJob_.has_value(); });
                if (stopWorker_) break;
                job = *pendingJob_;
                pendingJob_.reset();
                workerBusy_ = true;
            }

            const size_t pixelBytes = static_cast<size_t>(job.width) * static_cast<size_t>(job.height) *
                                      (job.format == aer::PixelBufferFormat::BGRA_32f ? 16ULL : 4ULL);
            if (scratch.size() < pixelBytes) {
                scratch.resize(pixelBytes);
            }

            // Decode directly into AER's VRAM/RAM cache (aborts mid-GOP if superseded by newer generation!)
            session_->renderVideoFrame(
                job.frameNumber,
                job.width,
                job.height,
                job.width * (job.format == aer::PixelBufferFormat::BGRA_32f ? 16 : 4),
                job.format,
                scratch.data(),
                /*flipVertical=*/false,
                /*fastKeyframeScrub=*/false,
                job.generation);

            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                workerBusy_ = false;
            }
            doneCv_.notify_all();
        }
    }

    SPBasicSuite* basicSuite_ = nullptr;
    PrSDKPPixCreatorSuite* ppixCreatorSuite_ = nullptr;
    PrSDKPPixCacheSuite* ppixCacheSuite_ = nullptr;
    PrSDKPPixSuite* ppixSuite_ = nullptr;
    PrSDKTimeSuite* timeSuite_ = nullptr;
    csSDK_int32 importerID_ = 0;
    std::unique_ptr<aer::AERSession> session_;
    bool valid_ = false;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::condition_variable doneCv_;
    std::optional<AsyncJob> pendingJob_;
    bool stopWorker_ = false;
    bool workerBusy_ = false;
    std::thread worker_;
};

PREMPLUGENTRY xAERAsyncImportEntry(int inSelector, void* inParam) {
    if (!inParam) return aiUnknownError;
    switch (inSelector) {
        case aiInitiateAsyncRead: {
            auto* req = reinterpret_cast<aiAsyncRequest*>(inParam);
            auto* imp = reinterpret_cast<AERAsyncImporter*>(req->inPrivateData);
            return imp ? imp->onInitiateAsyncRead(req->inSourceRec) : aiUnknownError;
        }
        case aiCancelAsyncRead: {
            auto* req = reinterpret_cast<aiAsyncRequest*>(inParam);
            auto* imp = reinterpret_cast<AERAsyncImporter*>(req->inPrivateData);
            return imp ? imp->onCancelAsyncRead(req->inSourceRec) : aiNoError;
        }
        case aiFlush: {
            auto* imp = reinterpret_cast<AERAsyncImporter*>(inParam);
            return imp ? imp->onFlush() : aiNoError;
        }
        case aiGetFrame: {
            auto* rec = reinterpret_cast<imSourceVideoRec*>(inParam);
            auto* imp = reinterpret_cast<AERAsyncImporter*>(rec->inPrivateData);
            return imp ? imp->onGetFrame(rec) : aiUnknownError;
        }
        case aiClose: {
            auto* imp = reinterpret_cast<AERAsyncImporter*>(inParam);
            delete imp;
            return aiNoError;
        }
        case aiSelectEfficientRenderTime: {
            auto* eff = reinterpret_cast<aiSelectEfficientRenderTimeRec*>(inParam);
            auto* imp = reinterpret_cast<AERAsyncImporter*>(eff->inPrivateData);
            return imp ? imp->onSelectEfficientRenderTime(eff) : aiUnsupported;
        }
        case aiGetMinPlaybackSpeedForSelectingEfficientTime: {
            auto* spd = reinterpret_cast<aiGetMinPlaybackSpeedForSelectingEfficientTimeRec*>(inParam);
            if (spd) spd->outPlaybackSpeed = 1.5;
            return aiNoError;
        }
        default:
            return aiUnsupported;
    }
}

prMALError handleCreateAsyncImporter(imAsyncImporterCreationRec* asyncRec) {
    if (!asyncRec) return imOtherErr;
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(asyncRec->inPrivateData);
    if (!h || !*h || !(*h)->session || !(*h)->basicSuite) {
        return imOtherErr;
    }

    auto* asyncImp = new AERAsyncImporter(
        (*h)->basicSuite,
        (*h)->importerID,
        (*h)->session->filePath(),
        (*h)->highPriority);
    if (!asyncImp->isValid()) {
        delete asyncImp;
        return imOtherErr;
    }

    asyncRec->outAsyncEntry = xAERAsyncImportEntry;
    asyncRec->outAsyncPrivateData = reinterpret_cast<void*>(asyncImp);
    return malNoError;
}

prMALError handleInit(imStdParms* stdParms, imImportInfoRec* info, bool highPriority) {
    if (!info) return imOtherErr;
    info->importerType = highPriority ? makeFourCC('A', 'R', 'Z', 'H') : makeFourCC('A', 'R', 'Z', ' ');
    info->canSave = kPrFalse;
    info->canDelete = kPrFalse;
    info->canResize = kPrTrue;
    info->canDoSubsize = kPrFalse;
    info->canDoContinuousTime = kPrFalse;
    info->noFile = kPrFalse;
    info->addToMenu = imMenuNone;
    info->hasSetup = kPrTrue;
    info->dontCache = kPrFalse;
    info->setupOnDblClk = kPrFalse;
    info->keepLoaded = kPrFalse;
    info->priority = highPriority ? 10000 : 0;
    info->canTrim = kPrFalse;
    info->canCalcSizes = kPrTrue;
    if (stdParms && stdParms->imInterfaceVer >= IMPORTMOD_VERSION_6) {
        info->avoidAudioConform = kPrFalse;
    }
    return imIsCacheable;
}

void launchARZControlCenter() {
    HMODULE selfMod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&launchARZControlCenter), &selfMod)) {
        return;
    }
    wchar_t modPath[MAX_PATH * 2]{};
    if (!GetModuleFileNameW(selfMod, modPath, MAX_PATH * 2)) return;
    std::wstring exePath(modPath);
    const auto slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exePath.erase(slash + 1);
    exePath += L"ARZDesktop.exe";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exePath + L"\"";
    if (CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

prMALError handleGetPrefs8(imStdParms* /*stdParms*/, imGetPrefsRec* prefsRec) {
    if (!prefsRec) return imOtherErr;
    if (prefsRec->prefs == nullptr || prefsRec->prefsLength == 0) {
        prefsRec->prefsLength = 16;
        return malNoError;
    }
    if (!prefsRec->firstTime) {
        launchARZControlCenter();
    }
    return malNoError;
}

prMALError handleGetIndFormat(csSDK_size_t index, imIndFormatRec* rec) {
    constexpr size_t kCount = sizeof(kSupportedFormats) / sizeof(kSupportedFormats[0]);
    if (!rec || index >= kCount) {
        return imBadFormatIndex;
    }
    const auto& fmt = kSupportedFormats[index];
    rec->filetype = fmt.fourcc;
    rec->canWriteTimecode = kPrFalse;
    rec->canWriteMetaData = kPrFalse;
    rec->flags = xfCanOpen | xfCanImport | xfCanReplace | xfIsMovie | xfUsesFiles;
    strcpy_s(rec->FormatName, sizeof(rec->FormatName), fmt.longName);
    strcpy_s(rec->FormatShortName, sizeof(rec->FormatShortName), fmt.shortName);
    std::memset(rec->PlatformExtension, 0, sizeof(rec->PlatformExtension));
    std::memcpy(rec->PlatformExtension, fmt.extensions,
                std::min(fmt.extBytes, sizeof(rec->PlatformExtension)));
    return malNoError;
}

prMALError handleGetIndPixelFormat(csSDK_size_t index, imIndPixelFormatRec* rec) {
    if (!rec) return imOtherErr;
    if (index == 0) {
        rec->outPixelFormat = PrPixelFormat_BGRA_4444_8u;
        return malNoError;
    }
    if (index == 1) {
        rec->outPixelFormat = PrPixelFormat_BGRA_4444_32f;
        return malNoError;
    }
    return imBadFormatIndex;
}

prMALError ensureInstanceAndSession(imStdParms* stdParms,
                                    const prUTF16Char* filepath,
                                    void*& ioPrivateData,
                                    bool highPriority,
                                    AERInstanceHandle& outHandle) {
    outHandle = nullptr;
    if (!stdParms || !filepath || filepath[0] == 0) {
        return imBadFile;
    }

    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(ioPrivateData);
    bool allocatedNow = false;
    if (!h) {
        h = reinterpret_cast<AERInstanceHandle>(
            stdParms->piSuites->memFuncs->newHandle(sizeof(AERInstanceRec)));
        if (!h || !*h) {
            return imMemErr;
        }
        std::memset(*h, 0, sizeof(AERInstanceRec));
        (*h)->magic = 0x41455231;
        (*h)->highPriority = highPriority;
        allocatedNow = true;
    }

    (*h)->acquireSuites(stdParms);

    const std::wstring wpath = prUtf16ToWide(filepath);
    if (!(*h)->session) {
        auto sess = std::make_unique<aer::AERSession>();
        if (!sess->open(wpath, highPriority)) {
            if (allocatedNow) {
                (*h)->releaseSuites();
                stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<char**>(h));
                ioPrivateData = nullptr;
            }
            return imBadFile;
        }
        (*h)->session = sess.release();
    }

    ioPrivateData = reinterpret_cast<void*>(h);
    outHandle = h;
    return malNoError;
}

prMALError handleOpenFile8(imStdParms* stdParms,
                           imFileRef* outFileRef,
                           imFileOpenRec8* openRec,
                           bool highPriority) {
    if (!openRec || !outFileRef) return imBadFile;

    AERInstanceHandle h = nullptr;
    const prMALError err = ensureInstanceAndSession(
        stdParms, openRec->fileinfo.filepath, openRec->privatedata, highPriority, h);
    if (err != malNoError || !h || !*h) {
        *outFileRef = imInvalidHandleValue;
        return imBadFile;
    }

    (*h)->importerID = openRec->inImporterID;
    openRec->outExtraMemoryUsage = aer::LRUFrameCache::instance().totalRamBytes();

    HANDLE winFile = CreateFileW(
        reinterpret_cast<LPCWSTR>(openRec->fileinfo.filepath),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (winFile == INVALID_HANDLE_VALUE) {
        *outFileRef = imInvalidHandleValue;
        return imBadFile;
    }

    *outFileRef = winFile;
    openRec->fileinfo.fileref = winFile;
    openRec->fileinfo.filetype = makeFourCC('A', 'R', 'Z', ' ');
    return malNoError;
}

prMALError handleQuietFile(imFileRef* fileRef, void* privateData) {
    if (fileRef && *fileRef != imInvalidHandleValue) {
        CloseHandle(*fileRef);
        *fileRef = imInvalidHandleValue;
    }
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(privateData);
    if (h && *h && (*h)->session) {
        (*h)->session->quiet();
    }
    return malNoError;
}

prMALError handleCloseFile(imStdParms* stdParms, imFileRef* fileRef, void* privateData) {
    handleQuietFile(fileRef, privateData);
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(privateData);
    if (h && *h) {
        if ((*h)->session) {
            (*h)->session->close();
            delete (*h)->session;
            (*h)->session = nullptr;
        }
        (*h)->releaseSuites();
        if (stdParms && stdParms->piSuites && stdParms->piSuites->memFuncs) {
            stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<char**>(h));
        }
    }
    return malNoError;
}

prMALError handleGetInfo8(imStdParms* stdParms,
                          imFileAccessRec8* accessRec,
                          imFileInfoRec8* infoRec,
                          bool highPriority) {
    if (!accessRec || !infoRec) return imBadFile;

    AERInstanceHandle h = nullptr;
    const prMALError err = ensureInstanceAndSession(
        stdParms, accessRec->filepath, infoRec->privatedata, highPriority, h);
    if (err != malNoError || !h || !*h || !(*h)->session) {
        return imBadFile;
    }

    stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(h));
    const auto& meta = (*h)->session->metadata();
    if (infoRec->vidInfo.importerID != 0) {
        (*h)->importerID = infoRec->vidInfo.importerID;
    }

    infoRec->accessModes = kRandomAccessImport;
    infoRec->hasDataRate = kPrFalse;
    infoRec->streamsAsComp = kPrFalse;

    if (meta.hasVideo && infoRec->streamIdx == 0) {
        infoRec->hasVideo = kPrTrue;
        infoRec->vidInfo.imageWidth = meta.width;
        infoRec->vidInfo.imageHeight = meta.height;
        infoRec->vidInfo.depth = meta.hasAlpha ? 32 : 24;
        infoRec->vidInfo.bitDepth = meta.bitDepth;
        infoRec->vidInfo.subType = makeFourCC('A', 'V', '0', '1');
        infoRec->vidInfo.fieldType = meta.isInterlaced
                                         ? (meta.topFieldFirst ? prFieldsUpperFirst : prFieldsLowerFirst)
                                         : prFieldsNone;
        infoRec->vidInfo.alphaType = meta.hasAlpha ? alphaStraight : alphaNone;
        infoRec->vidInfo.pixelAspectNum = meta.parNum;
        infoRec->vidInfo.pixelAspectDen = meta.parDen;
        // Enable Premiere Pro Async I/O (`imCreateAsyncImporter`) for Resolve-speed non-blocking scrubbing!
        infoRec->vidInfo.supportsAsyncIO = (meta.totalFrames > 1) ? kPrTrue : kPrFalse;
        infoRec->vidInfo.supportsGetSourceVideo = kPrTrue;
        infoRec->vidInfo.hasPulldown = kPrFalse;
        infoRec->vidInfo.isStill = (meta.totalFrames <= 1) ? kPrTrue : kPrFalse;
        infoRec->vidInfo.noDuration = imNoDurationFalse;

        infoRec->vidScale = static_cast<csSDK_int32>(meta.fpsNum);
        infoRec->vidSampleSize = static_cast<csSDK_int32>(meta.fpsDen);
        infoRec->vidDurationInFrames = meta.totalFrames;
        const int64_t legacyDur = meta.totalFrames * meta.fpsDen;
        infoRec->vidDuration = static_cast<csSDK_int32>(
            std::min<int64_t>(legacyDur, 0x7FFFFFFFLL));

        if ((*h)->timeSuite) {
            PrTime ticksPerSec = 0;
            (*h)->timeSuite->GetTicksPerSecond(&ticksPerSec);
            if (ticksPerSec > 0 && meta.fpsNum > 0) {
                infoRec->vidInfo.frameRate = (ticksPerSec * meta.fpsDen) / meta.fpsNum;
            }
        }

        if ((*h)->stringSuite) {
            const std::string desc = "ARZ: " + meta.videoCodecName + " (" +
                                     (*h)->session->activeVideoBackend() + ")";
            (*h)->stringSuite->AllocateFromUTF8(
                reinterpret_cast<const prUTF8Char*>(desc.c_str()),
                &infoRec->vidInfo.codecDescription);
        }
    } else {
        infoRec->hasVideo = kPrFalse;
    }

    if (meta.hasAudio && infoRec->streamIdx == 0) {
        infoRec->hasAudio = kPrTrue;
        infoRec->audInfo.numChannels = meta.audioChannels;
        infoRec->audInfo.sampleRate = static_cast<float>(meta.audioSampleRate);
        infoRec->audInfo.sampleType = kPrAudioSampleType_32BitFloat;
        infoRec->audDuration = meta.totalAudioSampleFrames;
    } else {
        infoRec->hasAudio = kPrFalse;
    }

    if (accessRec->filepath) {
        wcsncpy_s(reinterpret_cast<wchar_t*>(infoRec->filePath),
                  sizeof(infoRec->filePath) / sizeof(prUTF16Char),
                  reinterpret_cast<const wchar_t*>(accessRec->filepath),
                  _TRUNCATE);
    }

    stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(h));
    return malNoError;
}

prMALError handlePreferredFrameSize(imPreferredFrameSizeRec* rec) {
    if (!rec) return imOtherErr;
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(rec->inPrivateData);
    if (!h || !*h || !(*h)->session || rec->inIndex != 0) {
        return imOtherErr;
    }
    rec->outWidth = (*h)->session->metadata().width;
    rec->outHeight = (*h)->session->metadata().height;
    return malNoError;
}

prMALError handleGetTimeInfo8(imTimeInfoRec8* rec) {
    if (!rec) return malNoError;
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(rec->privatedata);
    if (h && *h && (*h)->session) {
        const auto& tc = (*h)->session->metadata().timecodeString;
        if (!tc.empty()) {
            strcpy_s(rec->orgtime, sizeof(rec->orgtime), tc.c_str());
            rec->dataType = 1;
        }
    }
    return malNoError;
}

prMALError handleAnalysis(imAnalysisRec* rec) {
    if (!rec || !rec->buffer || rec->buffersize <= 0) return malNoError;
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(rec->privatedata);
    if (!h || !*h || !(*h)->session) return malNoError;

    const auto& m = (*h)->session->metadata();
    char summary[512]{};
    std::snprintf(summary, sizeof(summary),
                  "Importer: ARZ 1.0.0\r\nContainer: %s\r\nVideo: %s (%s, %d-bit%s, %s)\r\nDecoder: %s\r\nAudio: %s (%d Hz, %d ch)",
                  m.containerName.c_str(),
                  m.videoCodecName.c_str(),
                  m.videoProfileName.empty() ? "default" : m.videoProfileName.c_str(),
                  m.bitDepth,
                  m.hasAlpha ? " + Alpha" : "",
                  m.colorSpaceSummary.c_str(),
                  (*h)->session->activeVideoBackend().c_str(),
                  m.hasAudio ? m.audioCodecName.c_str() : "none",
                  m.audioSampleRate,
                  m.audioChannels);
    strncpy_s(rec->buffer, static_cast<size_t>(rec->buffersize), summary, _TRUNCATE);
    return malNoError;
}

prMALError handleCalcSize8(imCalcSizeRec* calcRec, imFileAccessRec8* accessRec) {
    if (!calcRec || !accessRec) return imOtherErr;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (accessRec->filepath &&
        GetFileAttributesExW(reinterpret_cast<LPCWSTR>(accessRec->filepath), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER sz{};
        sz.HighPart = fad.nFileSizeHigh;
        sz.LowPart = fad.nFileSizeLow;
        calcRec->sizeInBytes = static_cast<prInt64>(sz.QuadPart);
        return malNoError;
    }
    return imMemErr;
}

prMALError handleGetSourceVideo(imSourceVideoRec* videoRec) {
    if (!videoRec || !videoRec->outFrame) return imOtherErr;
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(videoRec->inPrivateData);
    if (!h || !*h || !(*h)->session || !(*h)->ppixCreatorSuite || !(*h)->ppixSuite || !(*h)->timeSuite) {
        return imOtherErr;
    }

    const auto& meta = (*h)->session->metadata();
    if (!meta.hasVideo) return imOtherErr;

    PrTime ticksPerSec = 0;
    (*h)->timeSuite->GetTicksPerSecond(&ticksPerSec);
    int64_t frameNumber = 0;
    if (ticksPerSec > 0 && meta.fpsDen > 0 && meta.fpsNum > 0) {
        const PrTime ticksPerFrame = (ticksPerSec * meta.fpsDen) / meta.fpsNum;
        if (ticksPerFrame > 0) {
            frameNumber = (videoRec->inFrameTime + (ticksPerFrame / 2)) / ticksPerFrame;
        }
    }

    return populatePPixFromSession(
        (*h)->session,
        (*h)->ppixCreatorSuite,
        (*h)->ppixSuite,
        (*h)->ppixCacheSuite,
        (*h)->importerID,
        frameNumber,
        videoRec,
        /*fastKeyframeScrub=*/false,
        /*expectedGeneration=*/0);
}

prMALError handleImportAudio7(imImportAudioRec7* audioRec) {
    if (!audioRec || !audioRec->buffer) return imOtherErr;
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(audioRec->privateData);
    if (!h || !*h || !(*h)->session) return imOtherErr;

    if (!(*h)->session->renderAudioSamples(
            audioRec->position, static_cast<uint32_t>(audioRec->size), audioRec->buffer)) {
        return malUnknownError;
    }
    return malNoError;
}

prMALError handleGetPeakAudio(imPeakAudioRec* peakRec) {
    if (!peakRec || !peakRec->outMinima || !peakRec->outMaxima || peakRec->inNumSampleFrames <= 0) {
        return imOtherErr;
    }
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(peakRec->inPrivateData);
    if (!h || !*h || !(*h)->session) return imOtherErr;

    if (!(*h)->session->renderAudioPeaks(
            peakRec->inPosition,
            peakRec->inSampleRate,
            peakRec->inNumSampleFrames,
            peakRec->outMinima,
            peakRec->outMaxima)) {
        return malUnknownError;
    }
    return malNoError;
}

prMALError handleGetAudioChannelLayout(imGetAudioChannelLayoutRec* layoutRec) {
    if (!layoutRec) return imOtherErr;
    AERInstanceHandle h = reinterpret_cast<AERInstanceHandle>(layoutRec->inPrivateData);
    if (!h || !*h || !(*h)->session) return imOtherErr;

    const int ch = (*h)->session->metadata().audioChannels;
    for (int i = 0; i < kMaxAudioChannelCount; ++i) {
        layoutRec->outChannelLabels[i] = kPrAudioChannelLabel_Discrete;
    }
    if (ch == 1) {
        layoutRec->outChannelLabels[0] = kPrAudioChannelLabel_FrontCenter;
    } else if (ch == 2) {
        layoutRec->outChannelLabels[0] = kPrAudioChannelLabel_FrontLeft;
        layoutRec->outChannelLabels[1] = kPrAudioChannelLabel_FrontRight;
    } else if (ch == 6) {
        layoutRec->outChannelLabels[0] = kPrAudioChannelLabel_FrontLeft;
        layoutRec->outChannelLabels[1] = kPrAudioChannelLabel_FrontRight;
        layoutRec->outChannelLabels[2] = kPrAudioChannelLabel_FrontCenter;
        layoutRec->outChannelLabels[3] = kPrAudioChannelLabel_LowFrequency;
        layoutRec->outChannelLabels[4] = kPrAudioChannelLabel_BackLeft;
        layoutRec->outChannelLabels[5] = kPrAudioChannelLabel_BackRight;
    }
    return malNoError;
}

} // namespace

extern "C" AER_API int32_t xAEREntry(
    int32_t selector,
    void* stdParmsPtr,
    void* param1,
    void* param2,
    int32_t isHighPriority) {
    auto* stdParms = reinterpret_cast<imStdParms*>(stdParmsPtr);
    const bool highPriority = (isHighPriority != 0);

    switch (selector) {
        case imInit:
            return handleInit(stdParms, reinterpret_cast<imImportInfoRec*>(param1), highPriority);

        case imShutdown:
            aer::LRUFrameCache::instance().clear();
            return malNoError;

        case imGetSupports8:
            return malSupports8;

        case imGetIndFormat:
            return handleGetIndFormat(
                reinterpret_cast<csSDK_size_t>(param1),
                reinterpret_cast<imIndFormatRec*>(param2));

        case imGetIndPixelFormat:
            return handleGetIndPixelFormat(
                reinterpret_cast<csSDK_size_t>(param1),
                reinterpret_cast<imIndPixelFormatRec*>(param2));

        case imOpenFile8:
            return handleOpenFile8(
                stdParms,
                reinterpret_cast<imFileRef*>(param1),
                reinterpret_cast<imFileOpenRec8*>(param2),
                highPriority);

        case imQuietFile:
            return handleQuietFile(reinterpret_cast<imFileRef*>(param1), param2);

        case imCloseFile:
            return handleCloseFile(stdParms, reinterpret_cast<imFileRef*>(param1), param2);

        case imGetInfo8:
            return handleGetInfo8(
                stdParms,
                reinterpret_cast<imFileAccessRec8*>(param1),
                reinterpret_cast<imFileInfoRec8*>(param2),
                highPriority);

        case imGetPrefs8:
            return handleGetPrefs8(
                stdParms,
                reinterpret_cast<imGetPrefsRec*>(param2));

        case imGetPreferredFrameSize:
            return handlePreferredFrameSize(reinterpret_cast<imPreferredFrameSizeRec*>(param1));

        case imGetTimeInfo8:
            return handleGetTimeInfo8(reinterpret_cast<imTimeInfoRec8*>(param2));

        case imAnalysis:
            return handleAnalysis(reinterpret_cast<imAnalysisRec*>(param2));

        case imCalcSize8:
            return handleCalcSize8(
                reinterpret_cast<imCalcSizeRec*>(param1),
                reinterpret_cast<imFileAccessRec8*>(param2));

        case imGetSourceVideo:
            return handleGetSourceVideo(reinterpret_cast<imSourceVideoRec*>(param2));

        case imCreateAsyncImporter:
            return handleCreateAsyncImporter(reinterpret_cast<imAsyncImporterCreationRec*>(param1));

        case imImportAudio7:
            return handleImportAudio7(reinterpret_cast<imImportAudioRec7*>(param2));

        case imGetPeakAudio:
            return handleGetPeakAudio(reinterpret_cast<imPeakAudioRec*>(param2));

        case imGetAudioChannelLayout:
            return handleGetAudioChannelLayout(reinterpret_cast<imGetAudioChannelLayoutRec*>(param2));

        default:
            return imUnsupported;
    }
}
