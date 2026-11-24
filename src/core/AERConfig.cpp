#include "core/AERConfig.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aer {

namespace {

std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trimCopy(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n\"'");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n\"'");
    return s.substr(first, last - first + 1);
}

int queryHostLogicalThreads() {
#if defined(_WIN32)
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n > 0) return static_cast<int>(n);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? static_cast<int>(si.dwNumberOfProcessors) : 8;
#else
    return 8;
#endif
}

AERConfig loadConfigFromDisk() {
    AERConfig cfg;
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData) {
        return cfg;
    }
    std::string cfgPath = std::string(localAppData) + "\\ARZ\\ARZConfig.ini";
    std::ifstream in(cfgPath);
    if (!in.is_open()) {
        in.open(std::string(localAppData) + "\\AER\\AERConfig.ini");
        if (!in.is_open()) {
            return cfg;
        }
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto hashPos = line.find('#');
        if (hashPos != std::string::npos) line.erase(hashPos);
        const auto eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;
        std::string key = toLowerCopy(trimCopy(line.substr(0, eqPos)));
        std::string val = toLowerCopy(trimCopy(line.substr(eqPos + 1)));
        if (key == "enablehardwaredecode") cfg.enableHardwareDecode = (val == "1" || val == "true");
        else if (key == "enablegpucolorconvert") cfg.enableGpuColorConvert = (val == "1" || val == "true");
        else if (key == "enablemultithreading") cfg.enableMultithreading = (val == "1" || val == "true");
        else if (key == "enableframecache") cfg.enableFrameCache = (val == "1" || val == "true");
        else if (key == "enableseekingtable") cfg.enableSeekingTable = (val == "1" || val == "true");
        else if (key == "enablethumbnailcache") cfg.enableThumbnailCache = (val == "1" || val == "true");
        else if (key == "enablejsontelemetry") cfg.enableJsonTelemetry = (val == "1" || val == "true");
        else if (key == "selectiveoverride") cfg.selectiveOverride = (val == "1" || val == "true");
        else if (key == "forceoverrideall") cfg.forceOverrideAll = (val == "1" || val == "true");
        else if (key == "enable32bitfloatimport") cfg.enable32BitFloatImport = (val == "1" || val == "true");
        else if (key == "enableadaptivegovernor") cfg.enableAdaptiveGovernor = (val == "1" || val == "true");
        else if (key == "maxpercentvrambudget") cfg.maxPercentVramBudget = static_cast<uint32_t>(std::stoul(val));
        else if (key == "maxvramcachemb") cfg.maxVramCacheMB = static_cast<uint32_t>(std::stoul(val));
        else if (key == "maxramcachemb" || key == "maxframecachemb") cfg.maxRamCacheMB = static_cast<uint32_t>(std::stoul(val));
        else if (key == "maxpercentavailableram") cfg.maxPercentAvailableRam = static_cast<uint32_t>(std::stoul(val));
        else if (key == "preferredgpubackend") {
            cfg.preferredGpuBackend = val;
            if (val == "disabled" || val == "off" || val == "none") {
                cfg.enableHardwareDecode = false;
            }
        } else if (key == "overrideextensions") {
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                item = trimCopy(item);
                if (!item.empty() && item.front() == '.') item.erase(0, 1);
                if (!item.empty()) cfg.userOverrideExtensions.insert(item);
            }
        }
    }
    return cfg;
}

} // namespace

const AERConfig& AERConfig::instance() {
    static const AERConfig s_cfg = loadConfigFromDisk();
    return s_cfg;
}

AdaptiveHardwareBudgets AERConfig::computeAdaptiveBudgets(
    uint64_t totalVramBudgetBytes,
    uint64_t currentVramUsedBytes,
    uint64_t currentAerVramCacheBytes) const {
    AdaptiveHardwareBudgets out{};
    constexpr uint64_t kMB = 1024ULL * 1024ULL;
    constexpr uint64_t kGB = 1024ULL * kMB;

    // 1. CPU Thread Topology Partitioning (always keeps >= 50-67% of threads free for Premiere UI & Playhead!)
    const int logicalThreads = queryHostLogicalThreads();
    out.maxAuxCpuThreads = std::clamp(logicalThreads / 3, 2, 8);
    out.maxSwDecodeThreads = std::clamp((logicalThreads * 5) / 10, 2, 12);

    // 2. Elastic L1 VRAM Hot Cache + GOP Lookahead Depth (scales from 2GB laptops up to 24GB+ workstations)
    if (!enableAdaptiveGovernor && maxVramCacheMB > 0) {
        out.l1VramCacheBytes = static_cast<uint64_t>(maxVramCacheMB) * kMB;
    } else if (totalVramBudgetBytes > 0) {
        const uint64_t safeVramCeiling = (totalVramBudgetBytes * std::clamp<uint64_t>(maxPercentVramBudget, 50ULL, 88ULL)) / 100ULL;
        const uint64_t nonAerVramUsed = (currentVramUsedBytes > currentAerVramCacheBytes)
                                            ? (currentVramUsedBytes - currentAerVramCacheBytes)
                                            : 0ULL;
        const uint64_t freeHeadroom = (safeVramCeiling > nonAerVramUsed)
                                          ? (safeVramCeiling - nonAerVramUsed)
                                          : (128ULL * kMB);

        // Take 18% of live free GPU headroom, clamped cleanly by GPU class
        const uint64_t maxClassCap = (totalVramBudgetBytes >= 14ULL * kGB) ? (1792ULL * kMB)
                                   : (totalVramBudgetBytes >= 7ULL * kGB)  ? (768ULL * kMB)
                                                                           : (256ULL * kMB);
        out.l1VramCacheBytes = std::clamp<uint64_t>((freeHeadroom * 18ULL) / 100ULL, 160ULL * kMB, maxClassCap);
    } else {
        out.l1VramCacheBytes = 384ULL * kMB;
    }

    if (totalVramBudgetBytes >= 11ULL * kGB) {
        out.lookaheadFrames = 12;
        out.tierName = "High-VRAM Workstation (Auto)";
    } else if (totalVramBudgetBytes >= 6ULL * kGB) {
        out.lookaheadFrames = 6;
        out.tierName = "Balanced Creator (Auto)";
    } else {
        out.lookaheadFrames = 3;
        out.tierName = "Eco / Compact GPU (Auto)";
    }

    // 3. Elastic L2 Bi-Directional System RAM Scrub Cache (14% of live Available RAM, keeping >= 4GB free for OS/Adobe)
#if defined(_WIN32)
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        if (!enableAdaptiveGovernor && maxRamCacheMB > 0) {
            out.l2RamCacheBytes = static_cast<uint64_t>(maxRamCacheMB) * kMB;
        } else {
            const uint64_t reserveOsAdobe = 4ULL * kGB;
            const uint64_t usableAvail = (mem.ullAvailPhys > reserveOsAdobe) ? (mem.ullAvailPhys - reserveOsAdobe) : (256ULL * kMB);
            const uint64_t pct = std::clamp<uint64_t>(maxPercentAvailableRam, 8ULL, 35ULL);
            const uint64_t maxRamClassCap = (mem.ullTotalPhys >= 28ULL * kGB) ? (2560ULL * kMB)
                                          : (mem.ullTotalPhys >= 14ULL * kGB) ? (1024ULL * kMB)
                                                                              : (320ULL * kMB);
            out.l2RamCacheBytes = std::clamp<uint64_t>((usableAvail * pct) / 100ULL, 192ULL * kMB, maxRamClassCap);
        }
    }
#endif

    return out;
}

bool AERConfig::isCodecAutoOverride(const std::string& codecName) const {
    const std::string c = toLowerCopy(codecName);
    static const std::unordered_set<std::string> kOverrideCodecs = {
        "av1", "libdav1d", "libaom-av1",
        "hevc", "h264", "vp9", "libvpx-vp9", "vp8", "libvpx",
        "prores", "prores_raw", "dnxhd", "cfhd", "dxv",
        "ffv1", "opus", "libopus", "vorbis", "libvorbis",
        "flac", "alac", "hap", "qoi", "jpegxl", "libjxl", "webp",
        "gif", "mjpeg", "binkvideo", "apng", "speex",
        "dts", "dca", "ac3", "eac3",
        "hq_hqa", "hqx", "cllc", "vvc", "apv", "wmv3", "vc1", "rv40"
    };
    return kOverrideCodecs.count(c) > 0;
}

bool AERConfig::isExtensionForced(const std::string& extLower) const {
    std::string clean = toLowerCopy(extLower);
    if (!clean.empty() && clean.front() == '.') clean.erase(0, 1);
    static const std::unordered_set<std::string> kForcedExts = {
        "arz", "aer", "mkv", "mk3d", "mka", "mks",
        "webm", "weba", "webp", "ivf",
        "ogv", "ogg", "opus", "spx", "flac",
        "wmv", "asf", "rm", "rmvb", "flv", "f4v",
        "y4m", "qoi", "apng", "gif", "mjpeg", "mjpg",
        "bik", "bk2", "jxl", "dts", "dtshd", "ac3", "eac3", "vob", "3gp"
    };
    if (kForcedExts.count(clean) > 0) {
        return true;
    }
    return userOverrideExtensions.count(clean) > 0;
}

} // namespace aer
