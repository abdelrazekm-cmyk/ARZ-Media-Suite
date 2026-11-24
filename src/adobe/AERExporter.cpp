#include "export/AERExportEngine.hpp"
#include "media/DemuxEngine.hpp"

#ifndef PRWIN_ENV
#define PRWIN_ENV 1
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <PrSDKAudioSuite.h>
#include <PrSDKErrorSuite.h>
#include <PrSDKExport.h>
#include <PrSDKExportFileSuite.h>
#include <PrSDKExportInfoSuite.h>
#include <PrSDKExportParamSuite.h>
#include <PrSDKExportProgressSuite.h>
#include <PrSDKMALErrors.h>
#include <PrSDKMemoryManagerSuite.h>
#include <PrSDKPPixSuite.h>
#include <PrSDKPixelFormat.h>
#include <PrSDKQuality.h>
#include <PrSDKRenderCacheType.h>
#include <PrSDKSequenceAudioSuite.h>
#include <PrSDKSequenceRenderSuite.h>
#include <PrSDKTimeSuite.h>
#include <SPBasic.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr csSDK_uint32 makeFourCC(char a, char b, char c, char d) {
    return (static_cast<csSDK_uint32>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<csSDK_uint32>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<csSDK_uint32>(static_cast<uint8_t>(c)) << 8) |
           (static_cast<csSDK_uint32>(static_cast<uint8_t>(d)));
}

void copyWideToPrUtf16(const wchar_t* src, prUTF16Char* dst, size_t maxChars = 256) {
    if (!dst || maxChars == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    wcsncpy_s(reinterpret_cast<wchar_t*>(dst), maxChars, src, _TRUNCATE);
}

void launchARZDesktopFromExporter() {
    HMODULE selfMod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&launchARZDesktopFromExporter), &selfMod)) {
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

// Custom ARZ Exporter Parameter Identifiers
constexpr const char* kParamARZQuickPreset   = "ARZQuickPreset";
constexpr const char* kParamARZContainerExt  = "ARZContainerExt";
constexpr const char* kParamARZBitDepthAlpha = "ARZBitDepthAlpha";
constexpr const char* kParamARZColorSpace    = "ARZColorSpace";
constexpr const char* kParamARZRateMode      = "ARZRateMode";
constexpr const char* kParamARZCrfQuality    = "ARZCrfQuality";
constexpr const char* kParamARZBitrateMbps   = "ARZBitrateMbps";
constexpr const char* kParamARZSpeedPreset   = "ARZSpeedPreset";

// 7 Top-Level ARZ Format Profiles visible directly in Premiere Pro's "Format" dropdown (Ctrl+M)
struct ARZFormatProfile {
    csSDK_uint32 fileType;
    csSDK_uint32 classID;
    const wchar_t* displayName;
    const wchar_t* defaultExt;
    int32_t defaultPresetId;
    aer::ExportVideoCodec defaultVidCodec;
    aer::ExportAudioCodec defaultAudCodec;
    aer::ExportChromaBitDepth defaultBitDepth;
    bool supportsVideo;
    bool supportsAudio;
};

static const ARZFormatProfile kFormatProfiles[] = {
    {makeFourCC('A', 'R', 'V', '1'), makeFourCC('A', 'R', 'C', '1'),
     L"ARZ - AV1 Hardware / SVT (.mp4 / .mkv)", L"mp4",
     1, aer::ExportVideoCodec::AV1_NVENC, aer::ExportAudioCodec::AAC, aer::ExportChromaBitDepth::Bit8_420, true, true},

    {makeFourCC('A', 'R', 'M', 'K'), makeFourCC('A', 'R', 'C', '2'),
     L"ARZ - Matroska Universal (.mkv)", L"mkv",
     3, aer::ExportVideoCodec::HEVC_NVENC, aer::ExportAudioCodec::OPUS, aer::ExportChromaBitDepth::Bit10_420_HDR, true, true},

    {makeFourCC('A', 'R', 'W', 'M'), makeFourCC('A', 'R', 'C', '3'),
     L"ARZ - WebM / VP9 + Alpha (.webm)", L"webm",
     5, aer::ExportVideoCodec::VP9, aer::ExportAudioCodec::OPUS, aer::ExportChromaBitDepth::Alpha_4444, true, true},

    {makeFourCC('A', 'R', 'P', 'R'), makeFourCC('A', 'R', 'C', '4'),
     L"ARZ - ProRes / DNxHR / CineForm (.mov)", L"mov",
     6, aer::ExportVideoCodec::PRORES_4444_ALPHA, aer::ExportAudioCodec::PCM_S24, aer::ExportChromaBitDepth::Alpha_4444, true, true},

    {makeFourCC('A', 'R', 'F', 'F'), makeFourCC('A', 'R', 'C', '5'),
     L"ARZ - FFV1 Archival Lossless (.mkv)", L"mkv",
     9, aer::ExportVideoCodec::FFV1_LOSSLESS, aer::ExportAudioCodec::FLAC, aer::ExportChromaBitDepth::Bit10_422, true, true},

    {makeFourCC('A', 'R', 'G', 'F'), makeFourCC('A', 'R', 'C', '6'),
     L"ARZ - Animated GIF / WebP / APNG (.gif)", L"gif",
     10, aer::ExportVideoCodec::GIF, aer::ExportAudioCodec::OPUS, aer::ExportChromaBitDepth::Bit8_420, true, false},

    {makeFourCC('A', 'R', 'A', 'U'), makeFourCC('A', 'R', 'C', '7'),
     L"ARZ - Audio Master Opus / FLAC / WAV (.opus)", L"opus",
     11, aer::ExportVideoCodec::AV1_NVENC, aer::ExportAudioCodec::OPUS, aer::ExportChromaBitDepth::Bit8_420, false, true},
};

constexpr size_t kNumFormatProfiles = sizeof(kFormatProfiles) / sizeof(kFormatProfiles[0]);

const ARZFormatProfile& findFormatProfile(csSDK_uint32 fileType) {
    for (const auto& p : kFormatProfiles) {
        if (p.fileType == fileType) return p;
    }
    return kFormatProfiles[0];
}

enum ContainerExtId : int32_t {
    Ext_Auto = 0,
    Ext_MP4 = 1,
    Ext_MKV = 2,
    Ext_WebM = 3,
    Ext_MOV = 4,
    Ext_AVI = 5,
    Ext_GIF = 6,
    Ext_WebP = 7,
    Ext_APNG = 8,
    Ext_Opus = 9,
    Ext_FLAC = 10,
    Ext_WAV = 11,
    Ext_MP3 = 12,
    Ext_AAC = 13,
    Ext_AIFF = 14
};

const wchar_t* extIdToWide(int32_t extId, const ARZFormatProfile& prof) {
    switch (extId) {
        case Ext_MP4:  return L"mp4";
        case Ext_MKV:  return L"mkv";
        case Ext_WebM: return L"webm";
        case Ext_MOV:  return L"mov";
        case Ext_AVI:  return L"avi";
        case Ext_GIF:  return L"gif";
        case Ext_WebP: return L"webp";
        case Ext_APNG: return L"apng";
        case Ext_Opus: return L"opus";
        case Ext_FLAC: return L"flac";
        case Ext_WAV:  return L"wav";
        case Ext_MP3:  return L"mp3";
        case Ext_AAC:  return L"aac";
        case Ext_AIFF: return L"aiff";
        default:       return prof.defaultExt;
    }
}

std::string wideExtToAscii(const wchar_t* w) {
    std::string out;
    while (w && *w) {
        out.push_back(static_cast<char>(*w++));
    }
    return out;
}

const wchar_t* videoCodecDisplayName(int32_t c) {
    switch (static_cast<aer::ExportVideoCodec>(c)) {
        case aer::ExportVideoCodec::AV1_NVENC:         return L"AV1 - NVIDIA RTX NVENC Hardware [ARZ]";
        case aer::ExportVideoCodec::AV1_SVT:           return L"AV1 - SVT-AV1 High Efficiency [ARZ]";
        case aer::ExportVideoCodec::HEVC_NVENC:        return L"HEVC (H.265) - NVIDIA RTX NVENC [ARZ]";
        case aer::ExportVideoCodec::H264_NVENC:        return L"H.264 (AVC) - NVIDIA RTX NVENC [ARZ]";
        case aer::ExportVideoCodec::H264_SW:           return L"H.264 (AVC) - OpenH264 Software [ARZ]";
        case aer::ExportVideoCodec::VP9:               return L"VP9 - WebM / MKV + Alpha [ARZ]";
        case aer::ExportVideoCodec::PRORES_422_HQ:     return L"Apple ProRes 422 HQ 10-bit [ARZ]";
        case aer::ExportVideoCodec::PRORES_4444_ALPHA: return L"Apple ProRes 4444 + Alpha 10-bit [ARZ]";
        case aer::ExportVideoCodec::DNXHR_HQ:          return L"Avid DNxHR HQ / HQX [ARZ]";
        case aer::ExportVideoCodec::CINEFORM:          return L"GoPro CineForm HD 10-bit [ARZ]";
        case aer::ExportVideoCodec::FFV1_LOSSLESS:     return L"FFV1 Archival Lossless [ARZ]";
        case aer::ExportVideoCodec::MJPEG:             return L"Motion JPEG [ARZ]";
        case aer::ExportVideoCodec::GIF:               return L"Animated GIF [ARZ]";
        case aer::ExportVideoCodec::WEBP_ANIM:         return L"Animated WebP + Alpha [ARZ]";
        case aer::ExportVideoCodec::APNG:              return L"Animated PNG (APNG) + Alpha [ARZ]";
    }
    return L"AV1 [ARZ]";
}

const wchar_t* audioCodecDisplayName(int32_t c) {
    switch (static_cast<aer::ExportAudioCodec>(c)) {
        case aer::ExportAudioCodec::OPUS:    return L"Opus High Efficiency [ARZ]";
        case aer::ExportAudioCodec::FLAC:    return L"FLAC Lossless [ARZ]";
        case aer::ExportAudioCodec::AAC:     return L"AAC-LC [ARZ]";
        case aer::ExportAudioCodec::VORBIS:  return L"Vorbis [ARZ]";
        case aer::ExportAudioCodec::MP3:     return L"MP3 (LAME) [ARZ]";
        case aer::ExportAudioCodec::ALAC:    return L"Apple Lossless ALAC [ARZ]";
        case aer::ExportAudioCodec::AC3:     return L"Dolby Digital AC-3 [ARZ]";
        case aer::ExportAudioCodec::EAC3:    return L"Dolby Digital Plus E-AC-3 [ARZ]";
        case aer::ExportAudioCodec::PCM_S24: return L"PCM 24-bit Uncompressed [ARZ]";
        case aer::ExportAudioCodec::PCM_F32: return L"PCM 32-bit Float Master [ARZ]";
    }
    return L"Opus [ARZ]";
}

struct ARZExporterInstance {
    csSDK_uint32 fileType = 0;
    SPBasicSuite* spBasic = nullptr;
    PrSDKMemoryManagerSuite* memorySuite = nullptr;
    PrSDKExportParamSuite* paramSuite = nullptr;
    PrSDKExportProgressSuite* progressSuite = nullptr;
    PrSDKExportFileSuite* fileSuite = nullptr;
    PrSDKExportInfoSuite* infoSuite = nullptr;
    PrSDKErrorSuite3* errorSuite = nullptr;
    PrSDKPPixSuite* ppixSuite = nullptr;
    PrSDKSequenceAudioSuite1* audioSuite = nullptr;
    PrSDKSequenceRenderSuite* renderSuite = nullptr;
    PrSDKTimeSuite* timeSuite = nullptr;
};

int channelTypeToCount(csSDK_int32 chType) {
    if (chType == kPrAudioChannelType_Mono) return 1;
    if (chType == kPrAudioChannelType_51) return 6;
    return 2;
}

void setIntParamValue(PrSDKExportParamSuite* ps, csSDK_uint32 exID, const char* id, csSDK_int32 v) {
    exParamValues pv{};
    if (ps->GetParamValue(exID, 0, id, &pv) == suiteError_NoError) {
        pv.value.intValue = v;
        ps->ChangeParam(exID, 0, id, &pv);
    }
}

void applyQuickPreset(PrSDKExportParamSuite* ps, csSDK_uint32 exID, csSDK_int32 presetId) {
    if (!ps || presetId <= 0) return;
    switch (presetId) {
        case 1:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MP4);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::AV1_NVENC));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Bit8_420));
            setIntParamValue(ps, exID, kParamARZCrfQuality, 23);
            setIntParamValue(ps, exID, kParamARZSpeedPreset, 4);
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::AAC));
            break;
        case 2:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MKV);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::AV1_SVT));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Bit10_420_HDR));
            setIntParamValue(ps, exID, kParamARZCrfQuality, 22);
            setIntParamValue(ps, exID, kParamARZSpeedPreset, 5);
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::OPUS));
            break;
        case 3:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MKV);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::HEVC_NVENC));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Bit10_420_HDR));
            setIntParamValue(ps, exID, kParamARZCrfQuality, 22);
            setIntParamValue(ps, exID, kParamARZSpeedPreset, 5);
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::OPUS));
            break;
        case 4:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MP4);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::H264_NVENC));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Bit8_420));
            setIntParamValue(ps, exID, kParamARZCrfQuality, 20);
            setIntParamValue(ps, exID, kParamARZSpeedPreset, 4);
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::AAC));
            break;
        case 5:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_WebM);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::VP9));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Alpha_4444));
            setIntParamValue(ps, exID, kParamARZCrfQuality, 24);
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::OPUS));
            break;
        case 6:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MOV);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::PRORES_4444_ALPHA));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Alpha_4444));
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::PCM_S24));
            break;
        case 7:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MOV);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::PRORES_422_HQ));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Bit10_422));
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::PCM_S24));
            break;
        case 8:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MOV);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::CINEFORM));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Bit10_422));
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::PCM_S24));
            break;
        case 9:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MKV);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::FFV1_LOSSLESS));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Bit10_422));
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::FLAC));
            break;
        case 10:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_GIF);
            setIntParamValue(ps, exID, ADBEVideoCodec, static_cast<int32_t>(aer::ExportVideoCodec::GIF));
            setIntParamValue(ps, exID, kParamARZBitDepthAlpha, static_cast<int32_t>(aer::ExportChromaBitDepth::Bit8_420));
            break;
        case 11:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_Opus);
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::OPUS));
            break;
        case 12:
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_WAV);
            setIntParamValue(ps, exID, ADBEAudioCodec, static_cast<int32_t>(aer::ExportAudioCodec::PCM_S24));
            break;
        default:
            break;
    }
}

prMALError handleStartup(exportStdParms* /*stdParms*/, exExporterInfoRec* infoRec) {
    if (!infoRec) return exportReturn_ErrOther;
    const size_t idx = static_cast<size_t>(infoRec->exportReqIndex);
    if (idx >= kNumFormatProfiles) {
        return exportReturn_IterateExporterDone;
    }

    const auto& prof = kFormatProfiles[idx];
    infoRec->fileType = prof.fileType;
    copyWideToPrUtf16(prof.displayName, infoRec->fileTypeName);
    copyWideToPrUtf16(prof.defaultExt, infoRec->fileTypeDefaultExtension);
    infoRec->classID = prof.classID;
    infoRec->wantsNoProgressBar = kPrFalse;
    infoRec->hideInUI = kPrFalse;
    infoRec->doesNotSupportAudioOnly = prof.supportsAudio ? kPrFalse : kPrTrue;
    infoRec->canExportVideo = prof.supportsVideo ? kPrTrue : kPrFalse;
    infoRec->canExportAudio = prof.supportsAudio ? kPrTrue : kPrFalse;
    infoRec->singleFrameOnly = kPrFalse;
    infoRec->interfaceVersion = EXPORTMOD_VERSION;
    infoRec->isCacheable = kPrTrue;
    infoRec->canConformToMatchParams = kPrTrue;

    return (idx + 1 < kNumFormatProfiles)
               ? exportReturn_IterateExporter
               : exportReturn_IterateExporterDone;
}

prMALError handleBeginInstance(exportStdParms* stdParms, exExporterInstanceRec* instRec) {
    if (!stdParms || !instRec) return exportReturn_ErrMemory;
    SPBasicSuite* spBasic = stdParms->getSPBasicSuite();
    if (!spBasic) return exportReturn_ErrMemory;

    PrSDKMemoryManagerSuite* memSuite = nullptr;
    if (spBasic->AcquireSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion,
                              const_cast<const void**>(reinterpret_cast<void**>(&memSuite))) != kSPNoError ||
        !memSuite) {
        return exportReturn_ErrMemory;
    }

    auto* inst = reinterpret_cast<ARZExporterInstance*>(memSuite->NewPtrClear(sizeof(ARZExporterInstance)));
    if (!inst) {
        spBasic->ReleaseSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion);
        return exportReturn_ErrMemory;
    }

    inst->fileType = instRec->fileType;
    inst->spBasic = spBasic;
    inst->memorySuite = memSuite;

    spBasic->AcquireSuite(kPrSDKExportParamSuite, kPrSDKExportParamSuiteVersion,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->paramSuite)));
    spBasic->AcquireSuite(kPrSDKExportProgressSuite, kPrSDKExportProgressSuiteVersion,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->progressSuite)));
    spBasic->AcquireSuite(kPrSDKExportFileSuite, kPrSDKExportFileSuiteVersion,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->fileSuite)));
    spBasic->AcquireSuite(kPrSDKExportInfoSuite, kPrSDKExportInfoSuiteVersion,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->infoSuite)));
    spBasic->AcquireSuite(kPrSDKErrorSuite, kPrSDKErrorSuiteVersion3,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->errorSuite)));
    spBasic->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->ppixSuite)));
    spBasic->AcquireSuite(kPrSDKSequenceAudioSuite, kPrSDKSequenceAudioSuiteVersion1,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->audioSuite)));
    spBasic->AcquireSuite(kPrSDKSequenceRenderSuite, kPrSDKSequenceRenderSuiteVersion,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->renderSuite)));
    spBasic->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion,
                          const_cast<const void**>(reinterpret_cast<void**>(&inst->timeSuite)));

    instRec->privateData = inst;
    return malNoError;
}

prMALError handleEndInstance(exportStdParms* stdParms, exExporterInstanceRec* instRec) {
    if (!stdParms || !instRec || !instRec->privateData) return malNoError;
    auto* inst = reinterpret_cast<ARZExporterInstance*>(instRec->privateData);
    SPBasicSuite* spBasic = inst->spBasic;
    if (spBasic) {
        if (inst->paramSuite) spBasic->ReleaseSuite(kPrSDKExportParamSuite, kPrSDKExportParamSuiteVersion);
        if (inst->progressSuite) spBasic->ReleaseSuite(kPrSDKExportProgressSuite, kPrSDKExportProgressSuiteVersion);
        if (inst->fileSuite) spBasic->ReleaseSuite(kPrSDKExportFileSuite, kPrSDKExportFileSuiteVersion);
        if (inst->infoSuite) spBasic->ReleaseSuite(kPrSDKExportInfoSuite, kPrSDKExportInfoSuiteVersion);
        if (inst->errorSuite) spBasic->ReleaseSuite(kPrSDKErrorSuite, kPrSDKErrorSuiteVersion3);
        if (inst->ppixSuite) spBasic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        if (inst->audioSuite) spBasic->ReleaseSuite(kPrSDKSequenceAudioSuite, kPrSDKSequenceAudioSuiteVersion1);
        if (inst->renderSuite) spBasic->ReleaseSuite(kPrSDKSequenceRenderSuite, kPrSDKSequenceRenderSuiteVersion);
        if (inst->timeSuite) spBasic->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
        if (inst->memorySuite) {
            auto* ms = inst->memorySuite;
            ms->PrDisposePtr(reinterpret_cast<PrMemoryPtr>(inst));
            spBasic->ReleaseSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion);
        }
    }
    instRec->privateData = nullptr;
    return malNoError;
}

void addIntParam(PrSDKExportParamSuite* ps,
                 csSDK_int32 exID,
                 csSDK_int32 mgroup,
                 const char* group,
                 const char* id,
                 csSDK_int32 defVal,
                 csSDK_int32 minVal,
                 csSDK_int32 maxVal,
                 exParamFlags flags = exParamFlag_none) {
    exNewParamInfo info{};
    strcpy_s(info.identifier, sizeof(info.identifier), id);
    info.paramType = exParamType_int;
    info.flags = flags;
    info.paramValues.rangeMin.intValue = minVal;
    info.paramValues.rangeMax.intValue = maxVal;
    info.paramValues.value.intValue = defVal;
    info.paramValues.disabled = kPrFalse;
    info.paramValues.hidden = kPrFalse;
    ps->AddParam(exID, mgroup, group, &info);
}

prMALError handleGenerateDefaultParams(exportStdParms* /*stdParms*/, exGenerateDefaultParamRec* rec) {
    if (!rec || !rec->privateData) return exportReturn_ErrOther;
    auto* inst = reinterpret_cast<ARZExporterInstance*>(rec->privateData);
    auto* ps = inst->paramSuite;
    auto* is = inst->infoSuite;
    if (!ps) return exportReturn_ErrOther;

    const auto& prof = findFormatProfile(rec->fileType);
    const csSDK_int32 exID = rec->exporterPluginID;

    PrParam seqW{}, seqH{}, seqPARN{}, seqPARD{}, seqFPS{}, seqField{}, seqCh{}, seqSR{};
    seqW.mInt32 = 1920;
    seqH.mInt32 = 1080;
    seqPARN.mInt32 = 1;
    seqPARD.mInt32 = 1;
    seqField.mInt32 = prFieldsNone;
    seqCh.mInt32 = kPrAudioChannelType_Stereo;
    seqSR.mFloat64 = 48000.0;
    PrTime ticksPerSec = 254016000000LL;
    if (inst->timeSuite) inst->timeSuite->GetTicksPerSecond(&ticksPerSec);
    seqFPS.mInt64 = (ticksPerSec * 1001LL) / 24000LL;

    if (is) {
        is->GetExportSourceInfo(exID, kExportInfo_VideoWidth, &seqW);
        is->GetExportSourceInfo(exID, kExportInfo_VideoHeight, &seqH);
        if (seqW.mInt32 <= 0) seqW.mInt32 = 1920;
        if (seqH.mInt32 <= 0) seqH.mInt32 = 1080;
        is->GetExportSourceInfo(exID, kExportInfo_PixelAspectNumerator, &seqPARN);
        is->GetExportSourceInfo(exID, kExportInfo_PixelAspectDenominator, &seqPARD);
        if (seqPARN.mInt32 <= 0) seqPARN.mInt32 = 1;
        if (seqPARD.mInt32 <= 0) seqPARD.mInt32 = 1;
        is->GetExportSourceInfo(exID, kExportInfo_VideoFrameRate, &seqFPS);
        if (seqFPS.mInt64 <= 0) seqFPS.mInt64 = (ticksPerSec * 1001LL) / 24000LL;
        is->GetExportSourceInfo(exID, kExportInfo_VideoFieldType, &seqField);
        is->GetExportSourceInfo(exID, kExportInfo_AudioChannelsType, &seqCh);
        if (seqCh.mInt32 <= 0) seqCh.mInt32 = kPrAudioChannelType_Stereo;
        is->GetExportSourceInfo(exID, kExportInfo_AudioSampleRate, &seqSR);
        if (seqSR.mFloat64 <= 0.0) seqSR.mFloat64 = 48000.0;
    }

    csSDK_int32 mgroup = 0;
    ps->AddMultiGroup(exID, &mgroup);

    prUTF16Char tmp[256]{};

    copyWideToPrUtf16(L"ARZ Video Settings", tmp);
    ps->AddParamGroup(exID, mgroup, ADBETopParamGroup, ADBEVideoTabGroup, tmp, kPrFalse, kPrFalse, kPrFalse);

    copyWideToPrUtf16(L"ARZ Preset & Video Codec", tmp);
    ps->AddParamGroup(exID, mgroup, ADBEVideoTabGroup, ADBEVideoCodecGroup, tmp, kPrFalse, kPrFalse, kPrFalse);

    // Button to open ARZ Control Center (with ARZ Logo & GPU Monitor)
    exNewParamInfo btnParam{};
    strcpy_s(btnParam.identifier, sizeof(btnParam.identifier), ADBEVideoCodecPrefsButton);
    btnParam.paramType = exParamType_button;
    btnParam.flags = exParamFlag_none;
    ps->AddParam(exID, mgroup, ADBEVideoCodecGroup, &btnParam);

    addIntParam(ps, exID, mgroup, ADBEVideoCodecGroup, kParamARZQuickPreset, prof.defaultPresetId, 0, 12);
    addIntParam(ps, exID, mgroup, ADBEVideoCodecGroup, kParamARZContainerExt, Ext_Auto, 0, 14);
    addIntParam(ps, exID, mgroup, ADBEVideoCodecGroup, ADBEVideoCodec, static_cast<int32_t>(prof.defaultVidCodec), 0, 14);
    addIntParam(ps, exID, mgroup, ADBEVideoCodecGroup, kParamARZBitDepthAlpha, static_cast<int32_t>(prof.defaultBitDepth), 0, 3);
    addIntParam(ps, exID, mgroup, ADBEVideoCodecGroup, kParamARZColorSpace, 0, 0, 2);

    copyWideToPrUtf16(L"Basic Video Dimensions & Timing", tmp);
    ps->AddParamGroup(exID, mgroup, ADBEVideoTabGroup, ADBEBasicVideoGroup, tmp, kPrFalse, kPrFalse, kPrFalse);

    addIntParam(ps, exID, mgroup, ADBEBasicVideoGroup, ADBEVideoWidth, seqW.mInt32, 16, 8192);
    addIntParam(ps, exID, mgroup, ADBEBasicVideoGroup, ADBEVideoHeight, seqH.mInt32, 16, 8192);

    exNewParamInfo parParam{};
    strcpy_s(parParam.identifier, sizeof(parParam.identifier), ADBEVideoAspect);
    parParam.paramType = exParamType_ratio;
    parParam.paramValues.rangeMin.ratioValue.numerator = 10;
    parParam.paramValues.rangeMin.ratioValue.denominator = 11;
    parParam.paramValues.rangeMax.ratioValue.numerator = 2;
    parParam.paramValues.rangeMax.ratioValue.denominator = 1;
    parParam.paramValues.value.ratioValue.numerator = seqPARN.mInt32;
    parParam.paramValues.value.ratioValue.denominator = seqPARD.mInt32;
    ps->AddParam(exID, mgroup, ADBEBasicVideoGroup, &parParam);

    exNewParamInfo fpsParam{};
    strcpy_s(fpsParam.identifier, sizeof(fpsParam.identifier), ADBEVideoFPS);
    fpsParam.paramType = exParamType_ticksFrameRate;
    fpsParam.paramValues.rangeMin.timeValue = 1;
    fpsParam.paramValues.rangeMax.timeValue = ticksPerSec;
    fpsParam.paramValues.value.timeValue = seqFPS.mInt64;
    ps->AddParam(exID, mgroup, ADBEBasicVideoGroup, &fpsParam);

    addIntParam(ps, exID, mgroup, ADBEBasicVideoGroup, ADBEVideoFieldType, seqField.mInt32, 0, 2);

    copyWideToPrUtf16(L"ARZ Quality & Speed Control", tmp);
    ps->AddParamGroup(exID, mgroup, ADBEVideoTabGroup, ADBEVideoBitrateGroup, tmp, kPrFalse, kPrFalse, kPrFalse);

    addIntParam(ps, exID, mgroup, ADBEVideoBitrateGroup, kParamARZRateMode, 0, 0, 1);
    addIntParam(ps, exID, mgroup, ADBEVideoBitrateGroup, kParamARZCrfQuality, 23, 0, 51, exParamFlag_slider);

    exNewParamInfo brParam{};
    strcpy_s(brParam.identifier, sizeof(brParam.identifier), kParamARZBitrateMbps);
    brParam.paramType = exParamType_float;
    brParam.flags = exParamFlag_slider;
    brParam.paramValues.rangeMin.floatValue = 0.5f;
    brParam.paramValues.rangeMax.floatValue = 200.0f;
    brParam.paramValues.value.floatValue = 18.0f;
    ps->AddParam(exID, mgroup, ADBEVideoBitrateGroup, &brParam);

    addIntParam(ps, exID, mgroup, ADBEVideoBitrateGroup, kParamARZSpeedPreset, 4, 1, 7, exParamFlag_slider);

    copyWideToPrUtf16(L"ARZ Audio Settings", tmp);
    ps->AddParamGroup(exID, mgroup, ADBETopParamGroup, ADBEAudioTabGroup, tmp, kPrFalse, kPrFalse, kPrFalse);

    copyWideToPrUtf16(L"ARZ Audio Codec & Format", tmp);
    ps->AddParamGroup(exID, mgroup, ADBEAudioTabGroup, ADBEBasicAudioGroup, tmp, kPrFalse, kPrFalse, kPrFalse);

    addIntParam(ps, exID, mgroup, ADBEBasicAudioGroup, ADBEAudioCodec, static_cast<int32_t>(prof.defaultAudCodec), 0, 9);

    exNewParamInfo srParam{};
    strcpy_s(srParam.identifier, sizeof(srParam.identifier), ADBEAudioRatePerSecond);
    srParam.paramType = exParamType_float;
    srParam.paramValues.value.floatValue = static_cast<float>(seqSR.mFloat64);
    ps->AddParam(exID, mgroup, ADBEBasicAudioGroup, &srParam);

    addIntParam(ps, exID, mgroup, ADBEBasicAudioGroup, ADBEAudioNumChannels, seqCh.mInt32, 1, 6);
    addIntParam(ps, exID, mgroup, ADBEBasicAudioGroup, ADBEAudioBitrate, 192, 64, 512, exParamFlag_slider);

    ps->SetParamsVersion(exID, 1);
    return malNoError;
}

void addIntOption(PrSDKExportParamSuite* ps, csSDK_uint32 exID, const char* paramId, csSDK_int32 val, const wchar_t* label) {
    exOneParamValueRec v{};
    v.intValue = val;
    prUTF16Char u16[256]{};
    copyWideToPrUtf16(label, u16);
    ps->AddConstrainedValuePair(exID, 0, paramId, &v, u16);
}

void setParamLabel(PrSDKExportParamSuite* ps, csSDK_uint32 exID, const char* id, const wchar_t* label) {
    prUTF16Char u16[256]{};
    copyWideToPrUtf16(label, u16);
    ps->SetParamName(exID, 0, id, u16);
}

prMALError handlePostProcessParams(exportStdParms* /*stdParms*/, exPostProcessParamsRec* rec) {
    if (!rec || !rec->privateData) return exportReturn_ErrOther;
    auto* inst = reinterpret_cast<ARZExporterInstance*>(rec->privateData);
    auto* ps = inst->paramSuite;
    if (!ps) return exportReturn_ErrOther;
    const csSDK_uint32 exID = rec->exporterPluginID;

    setParamLabel(ps, exID, ADBEVideoCodecGroup, L"ARZ Preset & Codec Selection");
    setParamLabel(ps, exID, ADBEVideoCodecPrefsButton, L"Open ARZ Control Center...");
    setParamLabel(ps, exID, kParamARZQuickPreset, L"ARZ Preset");
    setParamLabel(ps, exID, kParamARZContainerExt, L"Container Format [ARZ]");
    setParamLabel(ps, exID, ADBEVideoCodec, L"Video Codec [ARZ]");
    setParamLabel(ps, exID, kParamARZBitDepthAlpha, L"Bit Depth & Alpha");
    setParamLabel(ps, exID, kParamARZColorSpace, L"Color Space");

    setParamLabel(ps, exID, ADBEBasicVideoGroup, L"Dimensions & Frame Rate");
    setParamLabel(ps, exID, ADBEVideoWidth, L"Width");
    setParamLabel(ps, exID, ADBEVideoHeight, L"Height");
    setParamLabel(ps, exID, ADBEVideoAspect, L"Pixel Aspect Ratio");
    setParamLabel(ps, exID, ADBEVideoFPS, L"Frame Rate (fps)");
    setParamLabel(ps, exID, ADBEVideoFieldType, L"Field Order");

    setParamLabel(ps, exID, ADBEVideoBitrateGroup, L"ARZ Quality & Speed");
    setParamLabel(ps, exID, kParamARZRateMode, L"Rate Control Mode");
    setParamLabel(ps, exID, kParamARZCrfQuality, L"Constant Quality (CRF / CQ)");
    setParamLabel(ps, exID, kParamARZBitrateMbps, L"Target Bitrate (Mbps)");
    setParamLabel(ps, exID, kParamARZSpeedPreset, L"Encoder Speed (1=Fastest, 7=Best)");

    setParamLabel(ps, exID, ADBEBasicAudioGroup, L"ARZ Audio Codec & Channels");
    setParamLabel(ps, exID, ADBEAudioCodec, L"Audio Codec [ARZ]");
    setParamLabel(ps, exID, ADBEAudioRatePerSecond, L"Sample Rate");
    setParamLabel(ps, exID, ADBEAudioNumChannels, L"Channels");
    setParamLabel(ps, exID, ADBEAudioBitrate, L"Audio Bitrate (kbps)");

    // 1. ARZ Quick Presets
    ps->ClearConstrainedValues(exID, 0, kParamARZQuickPreset);
    addIntOption(ps, exID, kParamARZQuickPreset, 0,  L"Custom (ARZ Manual Settings)");
    addIntOption(ps, exID, kParamARZQuickPreset, 1,  L"ARZ - AV1 RTX NVENC Fast (.mp4)");
    addIntOption(ps, exID, kParamARZQuickPreset, 2,  L"ARZ - AV1 SVT 10-bit High Quality (.mkv)");
    addIntOption(ps, exID, kParamARZQuickPreset, 3,  L"ARZ - HEVC 10-bit HDR RTX NVENC (.mkv)");
    addIntOption(ps, exID, kParamARZQuickPreset, 4,  L"ARZ - H.264 RTX NVENC YouTube / Social (.mp4)");
    addIntOption(ps, exID, kParamARZQuickPreset, 5,  L"ARZ - WebM VP9 + Alpha Transparency (.webm)");
    addIntOption(ps, exID, kParamARZQuickPreset, 6,  L"ARZ - Apple ProRes 4444 + Alpha Master (.mov)");
    addIntOption(ps, exID, kParamARZQuickPreset, 7,  L"ARZ - Apple ProRes 422 HQ Broadcast (.mov)");
    addIntOption(ps, exID, kParamARZQuickPreset, 8,  L"ARZ - GoPro CineForm 10-bit Master (.mov)");
    addIntOption(ps, exID, kParamARZQuickPreset, 9,  L"ARZ - FFV1 Archival Lossless + FLAC (.mkv)");
    addIntOption(ps, exID, kParamARZQuickPreset, 10, L"ARZ - Animated GIF High Quality (.gif)");
    addIntOption(ps, exID, kParamARZQuickPreset, 11, L"ARZ - Opus Audio Master (.opus)");
    addIntOption(ps, exID, kParamARZQuickPreset, 12, L"ARZ - WAV 24-bit Studio Lossless (.wav)");

    // 2. Container Extensions
    ps->ClearConstrainedValues(exID, 0, kParamARZContainerExt);
    addIntOption(ps, exID, kParamARZContainerExt, Ext_Auto, L"Auto (Match Format Default)");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_MP4,  L"MPEG-4 (.mp4) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_MKV,  L"Matroska (.mkv) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_WebM, L"WebM (.webm) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_MOV,  L"QuickTime (.mov) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_AVI,  L"AVI (.avi) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_GIF,  L"Animated GIF (.gif) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_WebP, L"Animated WebP (.webp) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_APNG, L"Animated PNG (.apng) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_Opus, L"Opus Audio (.opus) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_FLAC, L"FLAC Audio (.flac) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_WAV,  L"WAV Audio (.wav) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_MP3,  L"MP3 Audio (.mp3) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_AAC,  L"AAC Audio (.aac) [ARZ]");
    addIntOption(ps, exID, kParamARZContainerExt, Ext_AIFF, L"AIFF Audio (.aiff) [ARZ]");

    // 3. Video Codecs ([ARZ] branded)
    ps->ClearConstrainedValues(exID, 0, ADBEVideoCodec);
    for (int32_t c = 0; c <= 14; ++c) {
        addIntOption(ps, exID, ADBEVideoCodec, c, videoCodecDisplayName(c));
    }

    ps->ClearConstrainedValues(exID, 0, kParamARZBitDepthAlpha);
    addIntOption(ps, exID, kParamARZBitDepthAlpha, 0, L"8-bit YUV 4:2:0 (Standard)");
    addIntOption(ps, exID, kParamARZBitDepthAlpha, 1, L"10-bit YUV 4:2:0 (HDR / Wide Gamut)");
    addIntOption(ps, exID, kParamARZBitDepthAlpha, 2, L"10-bit YUV 4:2:2 (Broadcast / Mastering)");
    addIntOption(ps, exID, kParamARZBitDepthAlpha, 3, L"4:4:4:4 + Straight Alpha Channel (Transparency)");

    ps->ClearConstrainedValues(exID, 0, kParamARZColorSpace);
    addIntOption(ps, exID, kParamARZColorSpace, 0, L"Rec. 709 (SDR)");
    addIntOption(ps, exID, kParamARZColorSpace, 1, L"Rec. 2020 PQ (HDR10)");
    addIntOption(ps, exID, kParamARZColorSpace, 2, L"Rec. 2020 HLG (HDR)");

    ps->ClearConstrainedValues(exID, 0, kParamARZRateMode);
    addIntOption(ps, exID, kParamARZRateMode, 0, L"Constant Quality (CRF / CQ)");
    addIntOption(ps, exID, kParamARZRateMode, 1, L"Target Bitrate (Mbps VBR)");

    ps->ClearConstrainedValues(exID, 0, ADBEVideoAspect);
    const int32_t pars[][2] = {{1, 1}, {10, 11}, {40, 33}, {4, 3}, {2, 1}};
    const wchar_t* parNames[] = {
        L"Square Pixels (1.0)", L"DV NTSC (0.9091)", L"DV Widescreen (1.2121)",
        L"HD Anamorphic (1.333)", L"Anamorphic 2:1 (2.0)"};
    for (int i = 0; i < 5; ++i) {
        exOneParamValueRec pv{};
        pv.ratioValue.numerator = pars[i][0];
        pv.ratioValue.denominator = pars[i][1];
        prUTF16Char u16[256]{};
        copyWideToPrUtf16(parNames[i], u16);
        ps->AddConstrainedValuePair(exID, 0, ADBEVideoAspect, &pv, u16);
    }

    PrTime ticksPerSec = 254016000000LL;
    if (inst->timeSuite) inst->timeSuite->GetTicksPerSecond(&ticksPerSec);
    ps->ClearConstrainedValues(exID, 0, ADBEVideoFPS);
    const PrTime fpsND[][2] = {{12, 1}, {15, 1}, {24000, 1001}, {24, 1}, {25, 1},
                               {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}, {120, 1}};
    const wchar_t* fpsLabels[] = {L"12", L"15", L"23.976", L"24", L"25 (PAL)",
                                  L"29.97 (NTSC)", L"30", L"50", L"59.94", L"60", L"120"};
    for (int i = 0; i < 11; ++i) {
        exOneParamValueRec fv{};
        fv.timeValue = (ticksPerSec * fpsND[i][1]) / fpsND[i][0];
        prUTF16Char u16[256]{};
        copyWideToPrUtf16(fpsLabels[i], u16);
        ps->AddConstrainedValuePair(exID, 0, ADBEVideoFPS, &fv, u16);
    }

    ps->ClearConstrainedValues(exID, 0, ADBEVideoFieldType);
    addIntOption(ps, exID, ADBEVideoFieldType, prFieldsNone,       L"Progressive (None)");
    addIntOption(ps, exID, ADBEVideoFieldType, prFieldsUpperFirst, L"Upper First");
    addIntOption(ps, exID, ADBEVideoFieldType, prFieldsLowerFirst, L"Lower First");

    // 8. Audio Codecs ([ARZ] branded)
    ps->ClearConstrainedValues(exID, 0, ADBEAudioCodec);
    for (int32_t ac = 0; ac <= 9; ++ac) {
        addIntOption(ps, exID, ADBEAudioCodec, ac, audioCodecDisplayName(ac));
    }

    ps->ClearConstrainedValues(exID, 0, ADBEAudioRatePerSecond);
    const float srs[] = {44100.0f, 48000.0f, 96000.0f};
    const wchar_t* srNames[] = {L"44100 Hz", L"48000 Hz", L"96000 Hz"};
    for (int i = 0; i < 3; ++i) {
        exOneParamValueRec sv{};
        sv.floatValue = srs[i];
        prUTF16Char u16[256]{};
        copyWideToPrUtf16(srNames[i], u16);
        ps->AddConstrainedValuePair(exID, 0, ADBEAudioRatePerSecond, &sv, u16);
    }

    ps->ClearConstrainedValues(exID, 0, ADBEAudioNumChannels);
    addIntOption(ps, exID, ADBEAudioNumChannels, kPrAudioChannelType_Mono,   L"Mono");
    addIntOption(ps, exID, ADBEAudioNumChannels, kPrAudioChannelType_Stereo, L"Stereo");
    addIntOption(ps, exID, ADBEAudioNumChannels, kPrAudioChannelType_51,     L"5.1 Surround");

    return malNoError;
}

prMALError handleValidateParamChanged(exportStdParms* /*stdParms*/, exParamChangedRec* rec) {
    if (!rec || !rec->privateData) return malNoError;
    auto* inst = reinterpret_cast<ARZExporterInstance*>(rec->privateData);
    auto* ps = inst->paramSuite;
    if (!ps) return malNoError;
    const csSDK_uint32 exID = rec->exporterPluginID;

    if (std::strcmp(rec->changedParamIdentifier, kParamARZQuickPreset) == 0) {
        exParamValues pv{};
        ps->GetParamValue(exID, 0, kParamARZQuickPreset, &pv);
        applyQuickPreset(ps, exID, pv.value.intValue);
        rec->rebuildAllParams = kPrTrue;
        return malNoError;
    }

    if (std::strcmp(rec->changedParamIdentifier, ADBEVideoCodec) == 0) {
        exParamValues vc{};
        ps->GetParamValue(exID, 0, ADBEVideoCodec, &vc);
        const auto codec = static_cast<aer::ExportVideoCodec>(vc.value.intValue);
        if (codec == aer::ExportVideoCodec::GIF) {
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_GIF);
        } else if (codec == aer::ExportVideoCodec::WEBP_ANIM) {
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_WebP);
        } else if (codec == aer::ExportVideoCodec::APNG) {
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_APNG);
        } else if (codec == aer::ExportVideoCodec::PRORES_422_HQ ||
                   codec == aer::ExportVideoCodec::PRORES_4444_ALPHA ||
                   codec == aer::ExportVideoCodec::DNXHR_HQ ||
                   codec == aer::ExportVideoCodec::CINEFORM) {
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MOV);
        } else if (codec == aer::ExportVideoCodec::FFV1_LOSSLESS) {
            setIntParamValue(ps, exID, kParamARZContainerExt, Ext_MKV);
        }
    }
    return malNoError;
}

prMALError handleGetParamSummary(exportStdParms* /*stdParms*/, exParamSummaryRec* rec) {
    if (!rec || !rec->privateData) return malNoError;
    auto* inst = reinterpret_cast<ARZExporterInstance*>(rec->privateData);
    auto* ps = inst->paramSuite;
    if (!ps) return malNoError;
    const csSDK_uint32 exID = rec->exporterPluginID;
    const auto& prof = findFormatProfile(inst->fileType);

    exParamValues w{}, h{}, fps{}, vc{}, ac{}, sr{}, ch{}, crf{}, mode{}, br{}, ext{};
    ps->GetParamValue(exID, 0, ADBEVideoWidth, &w);
    ps->GetParamValue(exID, 0, ADBEVideoHeight, &h);
    ps->GetParamValue(exID, 0, ADBEVideoFPS, &fps);
    ps->GetParamValue(exID, 0, ADBEVideoCodec, &vc);
    ps->GetParamValue(exID, 0, ADBEAudioCodec, &ac);
    ps->GetParamValue(exID, 0, ADBEAudioRatePerSecond, &sr);
    ps->GetParamValue(exID, 0, ADBEAudioNumChannels, &ch);
    ps->GetParamValue(exID, 0, kParamARZCrfQuality, &crf);
    ps->GetParamValue(exID, 0, kParamARZRateMode, &mode);
    ps->GetParamValue(exID, 0, kParamARZBitrateMbps, &br);
    ps->GetParamValue(exID, 0, kParamARZContainerExt, &ext);

    PrTime ticksPerSec = 254016000000LL;
    if (inst->timeSuite) inst->timeSuite->GetTicksPerSecond(&ticksPerSec);
    const double fpsVal = (fps.value.timeValue > 0)
                              ? (static_cast<double>(ticksPerSec) / static_cast<double>(fps.value.timeValue))
                              : 23.976;

    wchar_t vidSum[256]{};
    swprintf_s(vidSum, L"%s | %dx%d @ %.2f fps (.%s)",
               videoCodecDisplayName(vc.value.intValue),
               w.value.intValue, h.value.intValue, fpsVal,
               extIdToWide(ext.value.intValue, prof));
    copyWideToPrUtf16(vidSum, rec->videoSummary);

    wchar_t audSum[256]{};
    swprintf_s(audSum, L"%s | %.0f Hz, %d ch",
               audioCodecDisplayName(ac.value.intValue),
               sr.value.floatValue,
               channelTypeToCount(ch.value.intValue));
    copyWideToPrUtf16(audSum, rec->audioSummary);

    wchar_t brSum[256]{};
    if (mode.value.intValue == 0) {
        swprintf_s(brSum, L"ARZ Constant Quality CRF/CQ = %d", crf.value.intValue);
    } else {
        swprintf_s(brSum, L"ARZ VBR Target = %.1f Mbps", br.value.floatValue);
    }
    copyWideToPrUtf16(brSum, rec->bitrateSummary);
    return malNoError;
}

prMALError handleQueryOutputSettings(exportStdParms* /*stdParms*/, exQueryOutputSettingsRec* rec) {
    if (!rec || !rec->privateData) return exportReturn_ErrOther;
    auto* inst = reinterpret_cast<ARZExporterInstance*>(rec->privateData);
    auto* ps = inst->paramSuite;
    if (!ps) return exportReturn_ErrOther;
    const csSDK_uint32 exID = rec->exporterPluginID;

    exParamValues w{}, h{}, fps{}, par{}, fld{}, sr{}, ch{}, br{}, mode{}, crf{}, ab{};
    if (rec->inExportVideo) {
        ps->GetParamValue(exID, 0, ADBEVideoWidth, &w);
        ps->GetParamValue(exID, 0, ADBEVideoHeight, &h);
        ps->GetParamValue(exID, 0, ADBEVideoFPS, &fps);
        ps->GetParamValue(exID, 0, ADBEVideoAspect, &par);
        ps->GetParamValue(exID, 0, ADBEVideoFieldType, &fld);
        ps->GetParamValue(exID, 0, kParamARZBitrateMbps, &br);
        ps->GetParamValue(exID, 0, kParamARZRateMode, &mode);
        ps->GetParamValue(exID, 0, kParamARZCrfQuality, &crf);

        rec->outVideoWidth = w.value.intValue;
        rec->outVideoHeight = h.value.intValue;
        rec->outVideoFrameRate = fps.value.timeValue;
        rec->outVideoAspectNum = par.value.ratioValue.numerator;
        rec->outVideoAspectDen = par.value.ratioValue.denominator;
        rec->outVideoFieldType = fld.value.intValue;
    }
    if (rec->inExportAudio) {
        ps->GetParamValue(exID, 0, ADBEAudioRatePerSecond, &sr);
        ps->GetParamValue(exID, 0, ADBEAudioNumChannels, &ch);
        ps->GetParamValue(exID, 0, ADBEAudioBitrate, &ab);
        rec->outAudioSampleRate = sr.value.floatValue;
        rec->outAudioSampleType = kPrAudioSampleType_32BitFloat;
        rec->outAudioChannelType = static_cast<PrAudioChannelType>(ch.value.intValue);
    }

    uint32_t kbps = 0;
    if (rec->inExportVideo) {
        if (mode.value.intValue == 1) {
            kbps += static_cast<uint32_t>(std::max<double>(0.5, static_cast<double>(br.value.floatValue)) * 1000.0);
        } else {
            kbps += static_cast<uint32_t>(std::max(1000, 35000 - crf.value.intValue * 600));
        }
    }
    if (rec->inExportAudio) {
        kbps += static_cast<uint32_t>(std::max(64, ab.value.intValue));
    }
    rec->outBitratePerSecond = kbps;
    return malNoError;
}

prMALError handleFileExtension(exportStdParms* /*stdParms*/, exQueryExportFileExtensionRec* rec) {
    if (!rec || !rec->privateData) return exportReturn_ErrOther;
    auto* inst = reinterpret_cast<ARZExporterInstance*>(rec->privateData);
    const auto& prof = findFormatProfile(inst->fileType);
    exParamValues ext{};
    if (inst->paramSuite) {
        inst->paramSuite->GetParamValue(rec->exporterPluginID, 0, kParamARZContainerExt, &ext);
    }
    copyWideToPrUtf16(extIdToWide(ext.value.intValue, prof), rec->outFileExtension);
    return malNoError;
}

prMALError handleParamButton(exportStdParms* /*stdParms*/, exParamButtonRec* /*btnRec*/) {
    launchARZDesktopFromExporter();
    return exportReturn_ParamButtonCancel; // Keep user's selected preset unchanged
}

prMALError handleDoExport(exportStdParms* /*stdParms*/, exDoExportRec* exportRec) {
    if (!exportRec || !exportRec->privateData) return exportReturn_ErrOther;
    auto* inst = reinterpret_cast<ARZExporterInstance*>(exportRec->privateData);
    auto* ps = inst->paramSuite;
    if (!ps || !inst->fileSuite || !inst->timeSuite) return exportReturn_ErrOther;

    const csSDK_uint32 exID = exportRec->exporterPluginID;
    const auto& prof = findFormatProfile(exportRec->fileType);

    csSDK_int32 pathLen = 0;
    inst->fileSuite->GetPlatformPath(exportRec->fileObject, &pathLen, nullptr);
    if (pathLen <= 0) return exportReturn_ErrIo;
    std::vector<prUTF16Char> rawPath(static_cast<size_t>(pathLen) + 4, 0);
    if (inst->fileSuite->GetPlatformPath(exportRec->fileObject, &pathLen, rawPath.data()) != suiteError_NoError) {
        return exportReturn_ErrIo;
    }

    exParamValues w{}, h{}, fps{}, par{}, vc{}, ac{}, sr{}, ch{}, ab{}, crf{}, mode{}, br{}, spd{}, bd{}, cs{}, ext{};
    ps->GetParamValue(exID, 0, ADBEVideoWidth, &w);
    ps->GetParamValue(exID, 0, ADBEVideoHeight, &h);
    ps->GetParamValue(exID, 0, ADBEVideoFPS, &fps);
    ps->GetParamValue(exID, 0, ADBEVideoAspect, &par);
    ps->GetParamValue(exID, 0, ADBEVideoCodec, &vc);
    ps->GetParamValue(exID, 0, ADBEAudioCodec, &ac);
    ps->GetParamValue(exID, 0, ADBEAudioRatePerSecond, &sr);
    ps->GetParamValue(exID, 0, ADBEAudioNumChannels, &ch);
    ps->GetParamValue(exID, 0, ADBEAudioBitrate, &ab);
    ps->GetParamValue(exID, 0, kParamARZCrfQuality, &crf);
    ps->GetParamValue(exID, 0, kParamARZRateMode, &mode);
    ps->GetParamValue(exID, 0, kParamARZBitrateMbps, &br);
    ps->GetParamValue(exID, 0, kParamARZSpeedPreset, &spd);
    ps->GetParamValue(exID, 0, kParamARZBitDepthAlpha, &bd);
    ps->GetParamValue(exID, 0, kParamARZColorSpace, &cs);
    ps->GetParamValue(exID, 0, kParamARZContainerExt, &ext);

    PrTime ticksPerSec = 254016000000LL;
    inst->timeSuite->GetTicksPerSecond(&ticksPerSec);
    const PrTime ticksPerFrame = std::max<PrTime>(1, fps.value.timeValue);

    aer::ExportJobConfig cfg;
    cfg.outputPath = std::wstring(reinterpret_cast<const wchar_t*>(rawPath.data()));
    cfg.containerExtension = wideExtToAscii(extIdToWide(ext.value.intValue, prof));
    cfg.enableVideo = (exportRec->exportVideo != kPrFalse) && prof.supportsVideo;
    cfg.enableAudio = (exportRec->exportAudio != kPrFalse) && prof.supportsAudio;
    cfg.videoCodec = static_cast<aer::ExportVideoCodec>(vc.value.intValue);
    cfg.width = std::max(16, w.value.intValue);
    cfg.height = std::max(16, h.value.intValue);
    cfg.parNum = std::max(1, par.value.ratioValue.numerator);
    cfg.parDen = std::max(1, par.value.ratioValue.denominator);

    const double exactFps = static_cast<double>(ticksPerSec) / static_cast<double>(ticksPerFrame);
    if (std::abs(exactFps - (24000.0 / 1001.0)) < 0.01) { cfg.fpsNum = 24000; cfg.fpsDen = 1001; }
    else if (std::abs(exactFps - (30000.0 / 1001.0)) < 0.01) { cfg.fpsNum = 30000; cfg.fpsDen = 1001; }
    else if (std::abs(exactFps - (60000.0 / 1001.0)) < 0.01) { cfg.fpsNum = 60000; cfg.fpsDen = 1001; }
    else { cfg.fpsNum = static_cast<int64_t>(std::lround(exactFps * 1000.0)); cfg.fpsDen = 1000; }

    cfg.useCrfMode = (mode.value.intValue == 0);
    cfg.crfQuality = crf.value.intValue;
    cfg.targetBitrateMbps = br.value.floatValue;
    cfg.speedPreset = spd.value.intValue;
    cfg.bitDepthMode = static_cast<aer::ExportChromaBitDepth>(bd.value.intValue);
    cfg.colorSpace = static_cast<aer::ExportColorSpace>(cs.value.intValue);

    cfg.audioCodec = static_cast<aer::ExportAudioCodec>(ac.value.intValue);
    cfg.audioSampleRate = static_cast<int>(sr.value.floatValue > 0 ? sr.value.floatValue : 48000.0f);
    cfg.audioChannels = channelTypeToCount(ch.value.intValue);
    cfg.audioBitrateKbps = ab.value.intValue;

    aer::AERExportEngine engine;
    if (!engine.open(cfg)) {
        if (inst->errorSuite) {
            std::wstring werr(engine.lastError().begin(), engine.lastError().end());
            prUTF16Char t[64]{}, d[256]{};
            copyWideToPrUtf16(L"ARZ Export Error", t, 64);
            copyWideToPrUtf16(werr.c_str(), d, 256);
            inst->errorSuite->SetEventStringUnicode(PrSDKErrorSuite3::kEventTypeError, t, d);
        }
        return exportReturn_ErrIo;
    }

    const PrTime totalDuration = std::max<PrTime>(ticksPerFrame, exportRec->endTime - exportRec->startTime);
    prMALError result = malNoError;

    if (cfg.enableVideo && inst->renderSuite && inst->ppixSuite) {
        csSDK_uint32 videoRenderID = 0;
        inst->renderSuite->MakeVideoRenderer(exID, &videoRenderID, ticksPerFrame);

        PrPixelFormat reqPixFmts[] = {PrPixelFormat_BGRA_4444_8u};
        SequenceRender_ParamsRec rParms{};
        rParms.inRequestedPixelFormatArray = reqPixFmts;
        rParms.inRequestedPixelFormatArrayCount = 1;
        rParms.inWidth = cfg.width;
        rParms.inHeight = cfg.height;
        rParms.inPixelAspectRatioNumerator = cfg.parNum;
        rParms.inPixelAspectRatioDenominator = cfg.parDen;
        rParms.inRenderQuality = (exportRec->maximumRenderQuality != 0) ? kPrRenderQuality_Max : kPrRenderQuality_High;
        rParms.inFieldType = prFieldsNone;
        rParms.inDeinterlace = kPrFalse;
        rParms.inDeinterlaceQuality = kPrRenderQuality_High;
        const bool wantAlpha = (cfg.bitDepthMode == aer::ExportChromaBitDepth::Alpha_4444 ||
                                cfg.videoCodec == aer::ExportVideoCodec::PRORES_4444_ALPHA ||
                                cfg.videoCodec == aer::ExportVideoCodec::APNG);
        rParms.inCompositeOnBlack = wantAlpha ? kPrFalse : kPrTrue;

        const float videoProgressWeight = cfg.enableAudio ? 0.88f : 1.0f;

        for (PrTime t = exportRec->startTime; t <= (exportRec->endTime - ticksPerFrame); t += ticksPerFrame) {
            SequenceRender_GetFrameReturnRec frameRet{};
            const prSuiteError rErr = inst->renderSuite->RenderVideoFrame(
                videoRenderID, t, &rParms, kRenderCacheType_None, &frameRet);
            if (rErr == suiteError_CompilerCompileAbort) {
                result = exportReturn_Abort;
                break;
            }
            if (frameRet.outFrame) {
                char* pixPtr = nullptr;
                csSDK_int32 rb = 0;
                inst->ppixSuite->GetPixels(frameRet.outFrame, PrPPixBufferAccess_ReadOnly, &pixPtr);
                inst->ppixSuite->GetRowBytes(frameRet.outFrame, &rb);
                const bool bottomUp = (rb > 0);
                const bool encOk = pixPtr && engine.writeVideoFrameBgra8(
                    reinterpret_cast<const uint8_t*>(pixPtr), rb, bottomUp);
                inst->ppixSuite->Dispose(frameRet.outFrame);
                if (!encOk) {
                    result = exportReturn_ErrIo;
                    break;
                }
            }

            if (inst->progressSuite) {
                const float pct = (static_cast<float>(t - exportRec->startTime) / static_cast<float>(totalDuration)) * videoProgressWeight;
                const prSuiteError pRes = inst->progressSuite->UpdateProgressPercent(exID, pct);
                if (pRes == suiteError_ExporterSuspended) {
                    inst->progressSuite->WaitForResume(exID);
                } else if (pRes == exportReturn_Abort) {
                    result = exportReturn_Abort;
                    break;
                }
            }
        }
        inst->renderSuite->ReleaseVideoRenderer(exID, videoRenderID);
    }

    if (result == malNoError && cfg.enableAudio && inst->audioSuite && inst->memorySuite) {
        PrTime ticksPerSample = 1;
        inst->timeSuite->GetTicksPerAudioSample(static_cast<float>(cfg.audioSampleRate), &ticksPerSample);
        csSDK_uint32 audioRenderID = 0;
        const PrAudioChannelType chEnum = (cfg.audioChannels == 1)
                                              ? kPrAudioChannelType_Mono
                                              : ((cfg.audioChannels == 6) ? kPrAudioChannelType_51 : kPrAudioChannelType_Stereo);
        if (inst->audioSuite->MakeAudioRenderer(
                exID, exportRec->startTime, chEnum, kPrAudioSampleType_32BitFloat,
                static_cast<float>(cfg.audioSampleRate), &audioRenderID) == suiteError_NoError) {
            PrAudioSample samplesRemaining = std::max<PrAudioSample>(0, totalDuration / std::max<PrTime>(1, ticksPerSample));
            csSDK_int32 maxBlip = 4096;
            inst->audioSuite->GetMaxBlip(audioRenderID, ticksPerFrame, &maxBlip);
            const csSDK_int32 allocSamples = std::clamp<csSDK_int32>(maxBlip, 1024, 65536);

            float* chPtrs[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            for (int c = 0; c < cfg.audioChannels; ++c) {
                chPtrs[c] = reinterpret_cast<float*>(inst->memorySuite->NewPtr(allocSamples * sizeof(float)));
            }

            while (samplesRemaining > 0 && result == malNoError) {
                const csSDK_uint32 req = static_cast<csSDK_uint32>(
                    std::min<PrAudioSample>(samplesRemaining, allocSamples));
                if (inst->audioSuite->GetAudio(audioRenderID, req, chPtrs, kPrFalse) != suiteError_NoError) {
                    break;
                }
                if (!engine.writeAudioSamplesFloatPlanar(chPtrs, req)) {
                    result = exportReturn_ErrIo;
                    break;
                }
                samplesRemaining -= req;
            }

            for (int c = 0; c < cfg.audioChannels; ++c) {
                if (chPtrs[c]) inst->memorySuite->PrDisposePtr(reinterpret_cast<PrMemoryPtr>(chPtrs[c]));
            }
            inst->audioSuite->ReleaseAudioRenderer(exID, audioRenderID);
        }
    }

    if (result == malNoError) {
        if (!engine.finalize()) {
            result = exportReturn_ErrIo;
        } else if (inst->progressSuite) {
            inst->progressSuite->UpdateProgressPercent(exID, 1.0f);
        }
    } else {
        engine.abortAndClose();
    }

    return result;
}

} // namespace

extern "C" AER_API int32_t xAERExportEntry(
    int32_t selector,
    void* stdParmsPtr,
    void* param1,
    void* /*param2*/) {
    auto* stdParms = reinterpret_cast<exportStdParms*>(stdParmsPtr);
    switch (selector) {
        case exSelStartup:
            return handleStartup(stdParms, reinterpret_cast<exExporterInfoRec*>(param1));
        case exSelBeginInstance:
            return handleBeginInstance(stdParms, reinterpret_cast<exExporterInstanceRec*>(param1));
        case exSelEndInstance:
            return handleEndInstance(stdParms, reinterpret_cast<exExporterInstanceRec*>(param1));
        case exSelGenerateDefaultParams:
            return handleGenerateDefaultParams(stdParms, reinterpret_cast<exGenerateDefaultParamRec*>(param1));
        case exSelPostProcessParams:
            return handlePostProcessParams(stdParms, reinterpret_cast<exPostProcessParamsRec*>(param1));
        case exSelGetParamSummary:
            return handleGetParamSummary(stdParms, reinterpret_cast<exParamSummaryRec*>(param1));
        case exSelQueryOutputSettings:
            return handleQueryOutputSettings(stdParms, reinterpret_cast<exQueryOutputSettingsRec*>(param1));
        case exSelQueryExportFileExtension:
            return handleFileExtension(stdParms, reinterpret_cast<exQueryExportFileExtensionRec*>(param1));
        case exSelParamButton:
            return handleParamButton(stdParms, reinterpret_cast<exParamButtonRec*>(param1));
        case exSelValidateParamChanged:
            return handleValidateParamChanged(stdParms, reinterpret_cast<exParamChangedRec*>(param1));
        case exSelValidateOutputSettings:
        case exSelShutdown:
            return malNoError;
        case exSelExport:
            return handleDoExport(stdParms, reinterpret_cast<exDoExportRec*>(param1));
        default:
            return exportReturn_Unsupported;
    }
}
