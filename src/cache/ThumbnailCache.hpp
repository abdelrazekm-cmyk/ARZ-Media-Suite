#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aer {

class ThumbnailCache {
public:
    static bool tryLoadBGRA8(const std::wstring& mediaPath,
                             int width,
                             int height,
                             int64_t frameNumber,
                             std::vector<uint8_t>& outPixels);

    static void storeBGRA8(const std::wstring& mediaPath,
                           int width,
                           int height,
                           int64_t frameNumber,
                           const uint8_t* pixels,
                           size_t byteCount);
};

} // namespace aer
