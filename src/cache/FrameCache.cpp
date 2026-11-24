#include "cache/FrameCache.hpp"
#include "core/AERConfig.hpp"
#include "media/GpuDeviceManager.hpp"

#include <algorithm>
#include <cstring>

namespace aer {

namespace {

void copyCpuRows(uint8_t* dst,
                 int dstRowBytes,
                 const uint8_t* src,
                 int srcRowBytes,
                 int rowCopyBytes,
                 int height,
                 bool flipVertical) {
    const ptrdiff_t signedDstStride = static_cast<ptrdiff_t>(dstRowBytes);
    if (!flipVertical && signedDstStride == srcRowBytes && srcRowBytes == rowCopyBytes) {
        std::memcpy(dst, src, static_cast<size_t>(rowCopyBytes) * static_cast<size_t>(height));
        return;
    }
    for (int y = 0; y < height; ++y) {
        const int dstY = flipVertical ? (height - 1 - y) : y;
        std::memcpy(dst + static_cast<ptrdiff_t>(dstY) * signedDstStride,
                    src + static_cast<size_t>(y) * static_cast<size_t>(srcRowBytes),
                    static_cast<size_t>(rowCopyBytes));
    }
}

} // namespace

size_t FrameCacheKeyHash::operator()(const FrameCacheKey& k) const noexcept {
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(k.sessionId);
    mix(static_cast<uint64_t>(k.frameNumber));
    mix(static_cast<uint64_t>(k.width));
    mix(static_cast<uint64_t>(k.height));
    mix(static_cast<uint64_t>(k.format));
    return static_cast<size_t>(h);
}

LRUFrameCache& LRUFrameCache::instance() {
    static LRUFrameCache s_cache;
    return s_cache;
}

LRUFrameCache::~LRUFrameCache() {
    for (auto& entry : vramLru_) {
        entry.second.texture.Detach();
    }
}

uint64_t LRUFrameCache::computeDynamicRamBudgetBytes() {
    const auto& cfg = AERConfig::instance();
    const uint64_t configuredMax = static_cast<uint64_t>(cfg.maxRamCacheMB) * 1024ULL * 1024ULL;
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        const uint64_t pct = std::clamp<uint64_t>(cfg.maxPercentAvailableRam, 10ULL, 90ULL);
        const uint64_t dynamicCap = (mem.ullAvailPhys * pct) / 100ULL;
        return std::min(configuredMax, dynamicCap);
    }
    return configuredMax;
}

bool LRUFrameCache::hasCachedFrame(const FrameCacheKey& key) const {
    if (!AERConfig::instance().enableFrameCache) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return vramIndex_.find(key) != vramIndex_.end() || ramIndex_.find(key) != ramIndex_.end();
}

bool LRUFrameCache::tryReadCachedFrame(const FrameCacheKey& key,
                                       uint8_t* dstBuffer,
                                       int dstRowBytes,
                                       bool flipVertical) {
    if (!AERConfig::instance().enableFrameCache || !dstBuffer) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> vramHitTex;
    std::shared_ptr<CachedVideoFrame> ramHitFrame;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto vit = vramIndex_.find(key);
        if (vit != vramIndex_.end()) {
            vramLru_.splice(vramLru_.begin(), vramLru_, vit->second);
            vramHitTex = vit->second->second.texture;
        } else {
            auto rit = ramIndex_.find(key);
            if (rit != ramIndex_.end()) {
                ramLru_.splice(ramLru_.begin(), ramLru_, rit->second);
                ramHitFrame = rit->second->second;
            }
        }
    }

    if (vramHitTex) {
        return GpuDeviceManager::instance().readbackVramTextureToCpu(
            vramHitTex.Get(), key.width, key.height, dstBuffer, dstRowBytes, flipVertical);
    }

    if (ramHitFrame) {
        const int tightBytes = key.width * (key.format == PixelBufferFormat::BGRA_32f ? 16 : 4);
        copyCpuRows(dstBuffer, dstRowBytes, ramHitFrame->pixels.data(),
                    ramHitFrame->rowBytes, tightBytes, key.height, flipVertical);
        return true;
    }

    return false;
}

void LRUFrameCache::putVramTexture(const FrameCacheKey& key, Microsoft::WRL::ComPtr<ID3D11Texture2D> tex) {
    if (!tex || !AERConfig::instance().enableFrameCache || key.format != PixelBufferFormat::BGRA_8u) {
        return;
    }

    const size_t texBytes = static_cast<size_t>(key.width) * static_cast<size_t>(key.height) * 4ULL;
    const auto& cfg = AERConfig::instance();
    const uint32_t maxVramPct = cfg.maxPercentVramBudget;
    const uint64_t ownCacheCapBytes = static_cast<uint64_t>(std::max<uint32_t>(64, cfg.maxVramCacheMB)) * 1024ULL * 1024ULL;

    std::vector<CachedVramFrame> evicted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = vramIndex_.find(key);
        if (existing != vramIndex_.end()) {
            currentVramBytes_ -= existing->second->second.byteEstimate;
            evicted.push_back(std::move(existing->second->second));
            vramLru_.erase(existing->second);
            vramIndex_.erase(existing);
        }

        const VramStatus vst = GpuDeviceManager::instance().queryVramStatus(currentVramBytes_);
        const uint64_t sysCeiling = vst.valid ? vst.effectiveCeilingBytes(maxVramPct) : UINT64_MAX;
        uint64_t projectedSysUsage = vst.valid ? (vst.currentUsageBytes + texBytes) : 0;

        while (!vramLru_.empty() &&
               ((currentVramBytes_ + texBytes > ownCacheCapBytes) ||
                (vst.valid && projectedSysUsage > sysCeiling))) {
            auto victim = std::move(vramLru_.back());
            vramLru_.pop_back();
            vramIndex_.erase(victim.first);
            currentVramBytes_ -= victim.second.byteEstimate;
            if (projectedSysUsage >= victim.second.byteEstimate) {
                projectedSysUsage -= victim.second.byteEstimate;
            }
            evicted.push_back(std::move(victim.second));
        }

        CachedVramFrame entry;
        entry.frameNumber = key.frameNumber;
        entry.width = key.width;
        entry.height = key.height;
        entry.byteEstimate = texBytes;
        entry.texture = std::move(tex);

        currentVramBytes_ += texBytes;
        vramLru_.emplace_front(key, std::move(entry));
        vramIndex_[key] = vramLru_.begin();
    }

    // Recycle evicted GPU surfaces directly back to D3D11 pool (ZERO PCIe readback or RAM bloat!)
    auto& gpuMgr = GpuDeviceManager::instance();
    for (auto& v : evicted) {
        if (v.texture) {
            gpuMgr.recycleVramTexture(std::move(v.texture), v.width, v.height);
        }
    }
}

void LRUFrameCache::putRamFrame(const FrameCacheKey& key, std::shared_ptr<CachedVideoFrame> frame) {
    if (!frame || frame->pixels.empty() || !AERConfig::instance().enableFrameCache) {
        return;
    }
    const uint64_t budget = computeDynamicRamBudgetBytes();
    if (frame->pixels.size() > budget) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = ramIndex_.find(key);
    if (existing != ramIndex_.end()) {
        currentRamBytes_ -= existing->second->second->pixels.size();
        ramLru_.erase(existing->second);
        ramIndex_.erase(existing);
    }
    currentRamBytes_ += frame->pixels.size();
    ramLru_.emplace_front(key, std::move(frame));
    ramIndex_[key] = ramLru_.begin();
    trimRamToBudgetLocked(budget);
}

void LRUFrameCache::evictSession(uint64_t sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = vramLru_.begin(); it != vramLru_.end();) {
        if (it->first.sessionId == sessionId) {
            currentVramBytes_ -= it->second.byteEstimate;
            vramIndex_.erase(it->first);
            it = vramLru_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = ramLru_.begin(); it != ramLru_.end();) {
        if (it->first.sessionId == sessionId) {
            currentRamBytes_ -= it->second->pixels.size();
            ramIndex_.erase(it->first);
            it = ramLru_.erase(it);
        } else {
            ++it;
        }
    }
}

void LRUFrameCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    vramIndex_.clear();
    vramLru_.clear();
    currentVramBytes_ = 0;
    ramIndex_.clear();
    ramLru_.clear();
    currentRamBytes_ = 0;
}

size_t LRUFrameCache::totalVramBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentVramBytes_;
}

size_t LRUFrameCache::totalVramEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vramIndex_.size();
}

size_t LRUFrameCache::totalRamBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentRamBytes_;
}

size_t LRUFrameCache::totalRamEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ramIndex_.size();
}

void LRUFrameCache::trimRamToBudgetLocked(uint64_t budgetBytes) {
    while (currentRamBytes_ > budgetBytes && !ramLru_.empty()) {
        const auto& victim = ramLru_.back();
        currentRamBytes_ -= victim.second->pixels.size();
        ramIndex_.erase(victim.first);
        ramLru_.pop_back();
    }
}

} // namespace aer
