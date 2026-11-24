#include "core/AERConfig.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

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
        else if (key == "maxpercentvrambudget") cfg.maxPercentVramBudget = static_cast<uint32_t>(std::stoul(val));
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
