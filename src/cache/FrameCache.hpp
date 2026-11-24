#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

namespace aer {

enum class PixelBufferFormat : uint8_t {
    BGRA_8u = 0,
    BGRA_32f = 1
};

struct CachedVideoFrame {
    int64_t frameNumber = 0;
    int width = 0;
    int height = 0;
    int rowBytes = 0;
    PixelBufferFormat format = PixelBufferFormat::BGRA_8u;
    std::vector<uint8_t> pixels;
};

struct CachedVramFrame {
    int64_t frameNumber = 0;
    int width = 0;
    int height = 0;
    size_t byteEstimate = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
};

struct FrameCacheKey {
    uint64_t sessionId = 0;
    int64_t frameNumber = 0;
    int width = 0;
    int height = 0;
    PixelBufferFormat format = PixelBufferFormat::BGRA_8u;

    bool operator==(const FrameCacheKey& o) const noexcept {
        return sessionId == o.sessionId &&
               frameNumber == o.frameNumber &&
               width == o.width &&
               height == o.height &&
               format == o.format;
    }
};

struct FrameCacheKeyHash {
    size_t operator()(const FrameCacheKey& k) const noexcept;
};

class LRUFrameCache {
public:
    static LRUFrameCache& instance();

    bool hasCachedFrame(const FrameCacheKey& key) const;

    bool tryReadCachedFrame(const FrameCacheKey& key,
                            uint8_t* dstBuffer,
                            int dstRowBytes,
                            bool flipVertical = false);

    void putVramTexture(const FrameCacheKey& key, Microsoft::WRL::ComPtr<ID3D11Texture2D> tex);
    void putRamFrame(const FrameCacheKey& key, std::shared_ptr<CachedVideoFrame> frame);

    void evictSession(uint64_t sessionId);
    void clear();

    size_t totalVramBytes() const;
    size_t totalVramEntries() const;
    size_t totalRamBytes() const;
    size_t totalRamEntries() const;
    static uint64_t computeDynamicRamBudgetBytes();

private:
    LRUFrameCache() = default;
    ~LRUFrameCache();
    void trimRamToBudgetLocked(uint64_t budgetBytes);

    mutable std::mutex mutex_;
    using VramListEntry = std::pair<FrameCacheKey, CachedVramFrame>;
    std::list<VramListEntry> vramLru_;
    std::unordered_map<FrameCacheKey, decltype(vramLru_)::iterator, FrameCacheKeyHash> vramIndex_;
    size_t currentVramBytes_ = 0;

    using RamListEntry = std::pair<FrameCacheKey, std::shared_ptr<CachedVideoFrame>>;
    std::list<RamListEntry> ramLru_;
    std::unordered_map<FrameCacheKey, decltype(ramLru_)::iterator, FrameCacheKeyHash> ramIndex_;
    size_t currentRamBytes_ = 0;
};

} // namespace aer
