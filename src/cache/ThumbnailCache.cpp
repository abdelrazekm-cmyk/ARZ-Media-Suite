#include "cache/ThumbnailCache.hpp"
#include "core/AERConfig.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace aer {

namespace {

constexpr uint32_t kThumbMagic = 0x545A5241; // 'ARZT'

std::filesystem::path buildThumbFilePath(const std::wstring& mediaPath,
                                         int width,
                                         int height,
                                         int64_t frameNumber) {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData) {
        return {};
    }
    std::error_code ec;
    const auto modTime = std::filesystem::last_write_time(mediaPath, ec).time_since_epoch().count();

    uint64_t h = 14695981039346656037ULL;
    for (wchar_t ch : mediaPath) {
        h ^= static_cast<uint64_t>(ch);
        h *= 1099511628211ULL;
    }
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(static_cast<uint64_t>(modTime));
    mix(static_cast<uint64_t>(width));
    mix(static_cast<uint64_t>(height));
    mix(static_cast<uint64_t>(frameNumber));

    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0') << h << ".arzt";
    std::filesystem::path dir = std::filesystem::path(localAppData) / "ARZ" / "Thumbnails";
    std::filesystem::create_directories(dir, ec);
    return dir / name.str();
}

} // namespace

bool ThumbnailCache::tryLoadBGRA8(const std::wstring& mediaPath,
                                  int width,
                                  int height,
                                  int64_t frameNumber,
                                  std::vector<uint8_t>& outPixels) {
    if (!AERConfig::instance().enableThumbnailCache || width <= 0 || height <= 0 || width > 640 || height > 640) {
        return false;
    }
    const auto path = buildThumbFilePath(mediaPath, width, height, frameNumber);
    if (path.empty()) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    uint32_t magic = 0, w = 0, h = 0, size = 0;
    in.read(reinterpret_cast<char*>(&magic), 4);
    in.read(reinterpret_cast<char*>(&w), 4);
    in.read(reinterpret_cast<char*>(&h), 4);
    in.read(reinterpret_cast<char*>(&size), 4);
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4ULL;
    if (!in || magic != kThumbMagic || w != static_cast<uint32_t>(width) ||
        h != static_cast<uint32_t>(height) || size != expected) {
        return false;
    }
    outPixels.resize(expected);
    in.read(reinterpret_cast<char*>(outPixels.data()), static_cast<std::streamsize>(expected));
    return static_cast<bool>(in);
}

void ThumbnailCache::storeBGRA8(const std::wstring& mediaPath,
                                int width,
                                int height,
                                int64_t frameNumber,
                                const uint8_t* pixels,
                                size_t byteCount) {
    if (!AERConfig::instance().enableThumbnailCache || !pixels || width <= 0 || height <= 0 ||
        width > 640 || height > 640) {
        return;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4ULL;
    if (byteCount != expected) return;

    const auto path = buildThumbFilePath(mediaPath, width, height, frameNumber);
    if (path.empty()) return;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;

    const uint32_t magic = kThumbMagic;
    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);
    const uint32_t sz = static_cast<uint32_t>(expected);
    out.write(reinterpret_cast<const char*>(&magic), 4);
    out.write(reinterpret_cast<const char*>(&w), 4);
    out.write(reinterpret_cast<const char*>(&h), 4);
    out.write(reinterpret_cast<const char*>(&sz), 4);
    out.write(reinterpret_cast<const char*>(pixels), static_cast<std::streamsize>(expected));
}

} // namespace aer
