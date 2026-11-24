#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace aer {

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
    // Lean VRAM & RAM cache budgets (Premiere caches rendered PPix frames independently)
    uint32_t maxPercentVramBudget = 78;
    uint32_t maxVramCacheMB = 384;
    uint32_t maxPercentAvailableRam = 15;
    uint32_t maxRamCacheMB = 512;
    std::string preferredGpuBackend = "auto";
    std::unordered_set<std::string> userOverrideExtensions;

    static const AERConfig& instance();
    bool isCodecAutoOverride(const std::string& codecName) const;
    bool isExtensionForced(const std::string& extLower) const;
};

} // namespace aer
