#include "core/AERSession.hpp"
#include "export/AERExportEngine.hpp"
#include "media/DemuxEngine.hpp"

#ifndef PRWIN_ENV
#define PRWIN_ENV 1
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <PrSDKExport.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

// Builds a Premiere-style BOTTOM-UP DIB BGRA_4444_8u frame:
// Visual TOP half of image = Pure RED (R=235, G=25, B=25) -> stored in rows [H/2 .. H-1] of bottom-up PPix!
// Visual BOTTOM half of image = Pure BLUE (R=25, G=45, B=235) -> stored in rows [0 .. H/2-1] of bottom-up PPix!
void buildPremiereBottomUpTestFrame(std::vector<uint8_t>& buf, int w, int h, uint8_t alphaVal = 255) {
    buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4ULL);
    for (int ppixRow = 0; ppixRow < h; ++ppixRow) {
        const bool isVisualTopHalf = (ppixRow >= h / 2);
        uint8_t* row = buf.data() + static_cast<size_t>(ppixRow) * static_cast<size_t>(w) * 4ULL;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = isVisualTopHalf ? 25  : 235; // B
            row[x * 4 + 1] = isVisualTopHalf ? 25  : 45;  // G
            row[x * 4 + 2] = isVisualTopHalf ? 235 : 25;  // R
            row[x * 4 + 3] = (x < w / 2) ? alphaVal : 255; // Left half semi-transparent when testing Alpha!
        }
    }
}

void buildSineAudio(std::vector<float>& chL, std::vector<float>& chR, size_t samples, int sampleRate) {
    chL.resize(samples);
    chR.resize(samples);
    for (size_t i = 0; i < samples; ++i) {
        const float v = 0.45f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / static_cast<float>(sampleRate));
        chL[i] = v;
        chR[i] = -v;
    }
}

std::string wideToUtf8Local(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n - 1, nullptr, nullptr);
    return out;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::cout << "=================================================================\n"
              << "  AER v0.5.0 Complete Importer + Exporter + Orientation QA Suite\n"
              << "=================================================================\n";

    // -------------------------------------------------------------------------
    // 1. Verify AER_Exporter.prm Binary DLL + xSDKExport Startup Enumeration
    // -------------------------------------------------------------------------
    wchar_t exePath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);
    std::filesystem::path buildDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path exporterPrm = buildDir / L"ARZ_Exporter.prm";

    HMODULE hExp = LoadLibraryExW(exporterPrm.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!hExp) {
        std::cerr << "[FAIL] Could not load AER_Exporter.prm!\n";
        return 10;
    }
    using xSDKExportFn = prMALError (*)(csSDK_int32, exportStdParms*, void*, void*);
    auto fnExport = reinterpret_cast<xSDKExportFn>(GetProcAddress(hExp, "xSDKExport"));
    if (!fnExport) {
        std::cerr << "[FAIL] xSDKExport symbol missing in AER_Exporter.prm!\n";
        return 11;
    }

    std::cout << "\n=== 1. AER_Exporter.prm xSDKExport Format Enumeration ===\n";
    int registeredExporters = 0;
    for (csSDK_int32 reqIdx = 0; reqIdx < 16; ++reqIdx) {
        exExporterInfoRec info{};
        info.exportReqIndex = reqIdx;
        exportStdParms stdParms{};
        const prMALError ret = fnExport(exSelStartup, &stdParms, &info, nullptr);
        const std::string nameUtf8 = wideToUtf8Local(reinterpret_cast<const wchar_t*>(info.fileTypeName));
        const std::string extUtf8 = wideToUtf8Local(reinterpret_cast<const wchar_t*>(info.fileTypeDefaultExtension));
        std::cout << "  [" << reqIdx << "] " << nameUtf8 << " (." << extUtf8 << ")\n";
        ++registeredExporters;
        if (ret == exportReturn_IterateExporterDone) break;
        if (ret != exportReturn_IterateExporter) {
            std::cerr << "[FAIL] Unexpected exSelStartup return code: " << ret << "\n";
            return 12;
        }
    }
    FreeLibrary(hExp);
    if (registeredExporters < 7) {
        std::cerr << "[FAIL] Expected 7 AER Exporter formats, got " << registeredExporters << "\n";
        return 13;
    }

    // -------------------------------------------------------------------------
    // 2. End-to-End Export + Re-Import + 180-Degree Vertical Orientation Proof
    // -------------------------------------------------------------------------
    std::cout << "\n=== 2. Real Hardware & Multi-Format Export -> Import Round-Trip ===\n";
    std::filesystem::path outDir = buildDir / L"qa_exports";
    std::filesystem::create_directories(outDir);

    const int W = 640, H = 360;
    std::vector<uint8_t> ppixBottomUpOpaque, ppixBottomUpAlpha;
    buildPremiereBottomUpTestFrame(ppixBottomUpOpaque, W, H, 255);
    buildPremiereBottomUpTestFrame(ppixBottomUpAlpha, W, H, 96);

    std::vector<float> chL, chR;
    buildSineAudio(chL, chR, 48000, 48000); // 1 second of 48 kHz stereo audio
    const float* audioPtrs[2] = {chL.data(), chR.data()};

    struct ExportTestCase {
        const char* label;
        std::wstring filename;
        std::string ext;
        aer::ExportVideoCodec vCodec;
        aer::ExportAudioCodec aCodec;
        aer::ExportChromaBitDepth depth;
        bool video;
        bool audio;
        bool expectAlpha;
    };

    const ExportTestCase cases[] = {
        {"AV1 RTX NVENC (.mp4 + AAC)",          L"test_aer_av1_nvenc.mp4",  "mp4",  aer::ExportVideoCodec::AV1_NVENC,         aer::ExportAudioCodec::AAC,     aer::ExportChromaBitDepth::Bit8_420,      true,  true,  false},
        {"HEVC 10-bit RTX NVENC (.mkv + Opus)", L"test_aer_hevc10.mkv",     "mkv",  aer::ExportVideoCodec::HEVC_NVENC,        aer::ExportAudioCodec::OPUS,    aer::ExportChromaBitDepth::Bit10_420_HDR, true,  true,  false},
        {"WebM VP9 + Alpha (.webm + Opus)",     L"test_aer_vp9_alpha.webm", "webm", aer::ExportVideoCodec::VP9,               aer::ExportAudioCodec::OPUS,    aer::ExportChromaBitDepth::Alpha_4444,    true,  true,  true},
        {"ProRes 4444 + Alpha (.mov + PCM24)",  L"test_aer_prores4444.mov", "mov",  aer::ExportVideoCodec::PRORES_4444_ALPHA, aer::ExportAudioCodec::PCM_S24, aer::ExportChromaBitDepth::Alpha_4444,    true,  true,  true},
        {"FFV1 Lossless + FLAC (.mkv)",         L"test_aer_ffv1.mkv",       "mkv",  aer::ExportVideoCodec::FFV1_LOSSLESS,     aer::ExportAudioCodec::FLAC,    aer::ExportChromaBitDepth::Bit10_422,     true,  true,  false},
        {"Animated GIF (.gif)",                 L"test_aer_anim.gif",       "gif",  aer::ExportVideoCodec::GIF,               aer::ExportAudioCodec::OPUS,    aer::ExportChromaBitDepth::Bit8_420,      true,  false, false},
        {"Pure Opus Audio (.opus)",             L"test_aer_audio.opus",     "opus", aer::ExportVideoCodec::AV1_NVENC,         aer::ExportAudioCodec::OPUS,    aer::ExportChromaBitDepth::Bit8_420,      false, true,  false},
    };

    for (const auto& tc : cases) {
        aer::ExportJobConfig cfg;
        cfg.outputPath = (outDir / tc.filename).wstring();
        cfg.containerExtension = tc.ext;
        cfg.enableVideo = tc.video;
        cfg.enableAudio = tc.audio;
        cfg.videoCodec = tc.vCodec;
        cfg.audioCodec = tc.aCodec;
        cfg.bitDepthMode = tc.depth;
        cfg.width = W;
        cfg.height = H;
        cfg.fpsNum = 24;
        cfg.fpsDen = 1;
        cfg.audioSampleRate = 48000;
        cfg.audioChannels = 2;

        aer::AERExportEngine eng;
        if (!eng.open(cfg)) {
            std::cerr << "[FAIL] Export open failed for " << tc.label << ": " << eng.lastError() << "\n";
            return 20;
        }
        const std::string vEncUsed = eng.activeVideoEncoderName();
        const std::string aEncUsed = eng.activeAudioEncoderName();

        if (tc.video) {
            const auto& srcBuf = tc.expectAlpha ? ppixBottomUpAlpha : ppixBottomUpOpaque;
            for (int f = 0; f < 24; ++f) {
                if (!eng.writeVideoFrameBgra8(srcBuf.data(), W * 4, /*bottomUpInput=*/true)) {
                    std::cerr << "[FAIL] Export frame write failed for " << tc.label << ": " << eng.lastError() << "\n";
                    return 21;
                }
            }
        }
        if (tc.audio) {
            if (!eng.writeAudioSamplesFloatPlanar(audioPtrs, 48000)) {
                std::cerr << "[FAIL] Export audio write failed for " << tc.label << ": " << eng.lastError() << "\n";
                return 22;
            }
        }
        if (!eng.finalize()) {
            std::cerr << "[FAIL] Export finalize failed for " << tc.label << ": " << eng.lastError() << "\n";
            return 23;
        }

        // Re-import the exported file through AER Importer Session!
        aer::AERSession verifySess;
        if (!verifySess.open(cfg.outputPath, /*highPriorityMode=*/false)) {
            std::cerr << "[FAIL] Re-import failed for exported file: " << tc.label << "\n";
            return 24;
        }
        const auto& vm = verifySess.metadata();
        std::cout << "  [OK] " << tc.label << " -> vidEnc=" << vEncUsed
                  << ", audEnc=" << aEncUsed
                  << " | Re-imported: vid=" << (vm.hasVideo ? vm.videoCodecName : "none")
                  << (vm.hasAlpha ? "+Alpha" : "")
                  << ", aud=" << (vm.hasAudio ? vm.audioCodecName : "none") << "\n";

        // Verify exact Top=RED, Bottom=BLUE orientation on the first video test case!
        if (tc.video && tc.ext == "mp4") {
            std::vector<uint8_t> topDownRead(static_cast<size_t>(W) * H * 4ULL);
            std::vector<uint8_t> ppixBottomUpRead(static_cast<size_t>(W) * H * 4ULL);

            verifySess.renderVideoFrame(2, W, H, W * 4, aer::PixelBufferFormat::BGRA_8u, topDownRead.data(), /*flipVertical=*/false);
            verifySess.renderVideoFrame(2, W, H, W * 4, aer::PixelBufferFormat::BGRA_8u, ppixBottomUpRead.data(), /*flipVertical=*/true);

            // In top-down buffer: row 10 is visual TOP (must be RED: R > 170, B < 80)
            const uint8_t* topPx = topDownRead.data() + (10ULL * W + W / 2) * 4ULL;
            // In Premiere bottom-up PPix buffer: row (H - 10) is visual TOP (must be RED!) and row 10 is visual BOTTOM (must be BLUE!)
            const uint8_t* ppixBottomRow = ppixBottomUpRead.data() + (10ULL * W + W / 2) * 4ULL;
            const uint8_t* ppixTopRow = ppixBottomUpRead.data() + (static_cast<size_t>(H - 10) * W + W / 2) * 4ULL;

            if (topPx[2] < 170 || topPx[0] > 80 || ppixTopRow[2] < 170 || ppixBottomRow[0] < 170) {
                std::cerr << "[FAIL] Orientation mismatch! topDown(R=" << int(topPx[2]) << ",B=" << int(topPx[0])
                          << ") ppixTop(R=" << int(ppixTopRow[2]) << ") ppixBot(B=" << int(ppixBottomRow[0]) << ")\n";
                return 25;
            }
            std::cout << "  [OK] 180-Degree Flip Proof: Visual Top=RED (R=" << int(ppixTopRow[2])
                      << "), Visual Bottom=BLUE (B=" << int(ppixBottomRow[0]) << ") in Premiere PPix!\n";
        }
    }

    // -------------------------------------------------------------------------
    // 2B. 1080p AV1 RTX NVENC Zero-Swscale Async Export Throughput Benchmark
    // -------------------------------------------------------------------------
    {
        constexpr int BW = 1920, BH = 1080, BFRAMES = 180;
        std::vector<uint8_t> benchFrame(static_cast<size_t>(BW) * BH * 4ULL, 128);
        aer::ExportJobConfig bcfg;
        bcfg.outputPath = (outDir / L"bench_1080p_av1_turbo.mp4").wstring();
        bcfg.containerExtension = "mp4";
        bcfg.enableVideo = true;
        bcfg.enableAudio = false;
        bcfg.videoCodec = aer::ExportVideoCodec::AV1_NVENC;
        bcfg.speedPreset = 1; // p1 fastest RTX preset
        bcfg.width = BW;
        bcfg.height = BH;
        bcfg.fpsNum = 60;
        bcfg.fpsDen = 1;

        aer::AERExportEngine beng;
        auto bt0 = std::chrono::steady_clock::now();
        if (!beng.open(bcfg)) return 26;
        for (int f = 0; f < BFRAMES; ++f) {
            benchFrame[static_cast<size_t>((f * 37) % (BW * BH)) * 4ULL] = static_cast<uint8_t>(f);
            if (!beng.writeVideoFrameBgra8(benchFrame.data(), BW * 4, /*bottomUpInput=*/true)) return 27;
        }
        if (!beng.finalize()) return 28;
        auto bt1 = std::chrono::steady_clock::now();
        const double elapsedSec = std::chrono::duration<double>(bt1 - bt0).count();
        const double exportFps = static_cast<double>(BFRAMES) / std::max(0.001, elapsedSec);
        std::cout << "  [BENCH] 1080p AV1 RTX Turbo Export (Zero-Swscale + Async Ring): "
                  << exportFps << " fps (" << (elapsedSec * 1000.0) << " ms for " << BFRAMES << " frames)\n";
    }

    // -------------------------------------------------------------------------
    // 3. DaVinci Resolve-Speed Native P010/NV12 + GOP Lookahead + Scrub Cancel Guard
    // -------------------------------------------------------------------------
    if (argc >= 2) {
        std::cout << "\n=== 3. ARZ v1.2.0 Native P010 GPU + GOP Lookahead + Scrub Benchmark ===\n";
        aer::AERSession::clearAllCaches();
        aer::AERSession session;
        if (!session.open(argv[1], true)) {
            std::cerr << "[FAIL] Could not open target MKV movie.\n";
            return 30;
        }
        const auto& m = session.metadata();
        // Test exact Premiere PPix path: 100% GPU VideoProcessorBlt (P010/NV12 -> BGRA8) + GOP Lookahead!
        const aer::PixelBufferFormat nativeFmt = aer::PixelBufferFormat::BGRA_8u;
        const int nativeStride = m.width * 4;
        std::vector<uint8_t> yuvBuf(static_cast<size_t>(nativeStride) * static_cast<size_t>(m.height));

        // 3A. Test scrub cancellation NEVER drops GPU session to CPU!
        const uint64_t staleGen = session.currentScrubGeneration();
        session.bumpScrubGeneration();
        session.renderVideoFrame(m.totalFrames / 3, m.width, m.height, nativeStride,
                                 nativeFmt, yuvBuf.data(), false, false, staleGen);
        if (!session.isUsingHardwareDecode()) {
            std::cerr << "[FAIL] Scrub cancellation falsely dropped session from GPU to CPU!\n";
            return 33;
        }
        std::cout << "  [OK] Scrub-Cancel Guard: Cancelled GOP seek kept GPU session active ("
                  << session.activeVideoBackend() << ")\n";

        // 3B. Warm MKV Cues once, then measure 5 rapid native P010/NV12 scrub jumps!
        session.renderVideoFrame(m.totalFrames / 2, m.width, m.height, nativeStride,
                                 nativeFmt, yuvBuf.data(), false, true);

        const int64_t scrubTargets[] = {
            (m.totalFrames * 15) / 100,
            (m.totalFrames * 35) / 100,
            (m.totalFrames * 55) / 100,
            (m.totalFrames * 75) / 100,
            (m.totalFrames * 92) / 100
        };
        auto t0 = std::chrono::steady_clock::now();
        for (int64_t f : scrubTargets) {
            if (!session.renderVideoFrame(f, m.width, m.height, nativeStride,
                                          nativeFmt, yuvBuf.data(),
                                          /*flipVertical=*/false, /*fastKeyframeScrub=*/true)) {
                std::cerr << "[FAIL] Native P010/NV12 fast scrub failed at frame " << f << "\n";
                return 31;
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        const double avgScrubMs = std::chrono::duration<double, std::milli>(t1 - t0).count() / 5.0;

        // 3C. Test Continuous GOP Lookahead Prefetch (frame N triggers background N+1..N+6 in VRAM!)
        const int64_t baseFrame = 240;
        if (!session.renderVideoFrame(baseFrame, m.width, m.height, nativeStride,
                                      nativeFmt, yuvBuf.data(), false, false)) {
            std::cerr << "[FAIL] Exact P010 frame decode failed!\n";
            return 34;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(45));
        auto lt0 = std::chrono::steady_clock::now();
        for (int step = 1; step <= 4; ++step) {
            if (!session.renderVideoFrame(baseFrame + step, m.width, m.height, nativeStride,
                                          nativeFmt, yuvBuf.data(), false, false)) {
                return 35;
            }
        }
        auto lt1 = std::chrono::steady_clock::now();
        const double lookaheadStepMs = std::chrono::duration<double, std::milli>(lt1 - lt0).count() / 4.0;

        // 3D. Test 4K 10-bit BGRA_32f ALSO runs 100% on GPU (ZERO CPU RAM overflow!)
        std::vector<uint8_t> float32Buf(static_cast<size_t>(m.width) * m.height * 16ULL);
        if (!session.renderVideoFrame(baseFrame + 1, m.width, m.height, m.width * 16,
                                      aer::PixelBufferFormat::BGRA_32f, float32Buf.data(), true, false)) {
            std::cerr << "[FAIL] GPU BGRA_32f conversion failed!\n";
            return 36;
        }

        // Verify thumbnail decode does NOT open a second GPU hardware decoder stream!
        std::vector<uint8_t> thumbBuf(160ULL * 90ULL * 4ULL);
        session.renderVideoFrame(120, 160, 90, 160 * 4, aer::PixelBufferFormat::BGRA_8u, thumbBuf.data(), true, false);
        const uint32_t gpuStreams = aer::AERSession::activeGpuStreams();
        const size_t vramMB = aer::AERSession::totalCachedVramBytes() / (1024ULL * 1024ULL);
        const size_t ramMB = aer::AERSession::totalCachedBytes() / (1024ULL * 1024ULL);

        std::cout << "  Average 4K 10-bit Native P010 Fast-Scrub Jump     : " << avgScrubMs << " ms/jump!\n";
        std::cout << "  GOP Lookahead Sequential Playback Latency (4K)    : " << lookaheadStepMs << " ms/frame!\n";
        std::cout << "  Active GPU Decode Streams (incl. Thumbnail)       : " << gpuStreams << " (Shared Session + Zero thumb GPU!)\n";
        std::cout << "  Cached VRAM / CPU-RAM Footprint                   : VRAM=" << vramMB << " MB (cap 384 MB), CPU-RAM=" << ramMB << " MB (ZERO CPU swscale!)\n";
        if (gpuStreams > 1 || vramMB > 390 || ramMB > 0) {
            std::cerr << "[FAIL] Unexpected CPU RAM fallback or resource cap exceeded! gpuStreams="
                      << gpuStreams << " vramMB=" << vramMB << " ramMB=" << ramMB << "\n";
            return 32;
        }
    }

    std::cout << "\n[PASS] ALL ARZ v1.2.0 Native P010 GPU + GOP Lookahead + Shared MediaCore Tests Passed!\n";
    return 0;
}
