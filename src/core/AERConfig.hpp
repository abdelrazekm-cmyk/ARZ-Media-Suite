#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace aer {

struct AdaptiveHardwareBudgets {
    std::string tierName = "Balanced (Auto)";
    uint64_t l1VramCacheBytes = 512ULL * 1024ULL * 1024ULL;
    uint64_t l2RamCacheBytes = 768ULL * 1024ULL * 1024ULL;
    int lookaheadFrames = 6;
    int maxAuxCpuThreads = 4;
    int maxSwDecodeThreads = 6;
};

struct AERConfig {
    bool enableHardwareDecode = true;
    bool enableGpuColorConvert = true;
    bool enableMultithreading = true;
    bool enableFrameCache = true;
    bool enableSeekingTable = true;
    bool enableThumbnailCache = true;
    bool enableJsonTelemetry = true;
    bool selectiveOverride = true;
    bool forceOverrideAll = false;
    bool enable32BitFloatImport = false;
    bool enableAdaptiveGovernor = true; // Auto-customizes VRAM, RAM, CPU threads & Lookahead per client PC!

    uint32_t maxPercentVramBudget = 78;
    uint32_t maxVramCacheMB = 0;        // 0 = Auto-computed by Adaptive Hardware Governor
    uint32_t maxPercentAvailableRam = 14;
    uint32_t maxRamCacheMB = 0;         // 0 = Auto-computed by Adaptive Hardware Governor
    std::string preferredGpuBackend = "auto";
    std::unordered_set<std::string> userOverrideExtensions;

    static const AERConfig& instance();
    bool isCodecAutoOverride(const std::string& codecName) const;
    bool isExtensionForced(const std::string& extLower) const;

    // Live per-machine adaptive hardware governor (re-evaluates live VRAM & RAM pressure dynamically)
    AdaptiveHardwareBudgets computeAdaptiveBudgets(uint64_t totalVramBudgetBytes,
                                                   uint64_t currentVramUsedBytes,
                                                   uint64_t currentAerVramCacheBytes) const;
};

} // namespace aer
