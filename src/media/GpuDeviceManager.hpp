#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

struct AVBufferRef;
struct AVFrame;

namespace aer {

struct VramStatus {
    uint64_t budgetBytes = 0;
    uint64_t currentUsageBytes = 0;
    uint64_t aerCachedVramBytes = 0;
    bool valid = false;

    uint64_t effectiveCeilingBytes(uint32_t maxPercent) const noexcept {
        return (budgetBytes * static_cast<uint64_t>(maxPercent)) / 100ULL;
    }
    bool hasHeadroomFor(uint64_t additionalBytes, uint32_t maxPercent) const noexcept {
        if (!valid || budgetBytes == 0) return false;
        return (currentUsageBytes + additionalBytes) <= effectiveCeilingBytes(maxPercent);
    }
};

class GpuDeviceManager {
public:
    static GpuDeviceManager& instance();

    bool ensureInitialized();
    AVBufferRef* acquireSharedD3D11HwDeviceRef();
    const std::string& adapterName() const noexcept { return adapterName_; }
    uint32_t vendorId() const noexcept { return vendorId_; }

    VramStatus queryVramStatus(uint64_t aerCachedVramBytes = 0) const;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> convertHwFrameToVramBgra8(
        const AVFrame* hwFrame,
        int dstWidth,
        int dstHeight);

    bool readbackVramTextureToCpu(
        ID3D11Texture2D* vramTex,
        int width,
        int height,
        uint8_t* dstCpuPixels,
        int dstRowBytes,
        bool flipVertical = false);

private:
    GpuDeviceManager() = default;
    ~GpuDeviceManager();

    bool ensureStagingTextureLocked(int width, int height);

    mutable std::mutex mutex_;
    bool initialized_ = false;
    uint32_t vendorId_ = 0;
    std::string adapterName_ = "none";

    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    AVBufferRef* ffHwDeviceRef_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    int vpInW_ = 0;
    int vpInH_ = 0;
    int vpOutW_ = 0;
    int vpOutH_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTex_;
    int stagingW_ = 0;
    int stagingH_ = 0;
};

} // namespace aer
