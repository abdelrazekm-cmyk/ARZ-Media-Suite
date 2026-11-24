#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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
    // Creates an isolated D3D11VA AVBufferRef on the primary GPU so concurrent decoders never contend!
    AVBufferRef* createIsolatedD3D11HwDeviceRef();

    const std::string& adapterName() const noexcept { return adapterName_; }
    uint32_t vendorId() const noexcept { return vendorId_; }

    VramStatus queryVramStatus(uint64_t aerCachedVramBytes = 0) const;

    // Zero-conversion GPU-to-GPU clone of native NV12 / P010 hardware decoder slice into pooled VRAM texture!
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copyHwFrameToVramYuv(
        const AVFrame* hwFrame,
        DXGI_FORMAT& outDxgiFormat,
        int& outAllocWidth,
        int& outAllocHeight);

    bool readbackVramYuvToCpu(
        ID3D11Texture2D* yuvTex,
        DXGI_FORMAT fmt,
        int visibleWidth,
        int visibleHeight,
        int texAllocWidth,
        int texAllocHeight,
        uint8_t* dstBase,
        int dstRowBytes);

    // 100% GPU Hardware Color Conversion & Scaling (NV12/P010 -> BGRA8 or RGBA16_UNORM for BGRA_32f!)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> convertHwFrameToVramBgra8(
        const AVFrame* hwFrame,
        int dstWidth,
        int dstHeight,
        DXGI_FORMAT targetDxgiFmt = DXGI_FORMAT_B8G8R8A8_UNORM);

    bool readbackVramTextureToCpu(
        ID3D11Texture2D* vramTex,
        int width,
        int height,
        uint8_t* dstCpuPixels,
        int dstRowBytes,
        bool flipVertical = false);

    bool readbackVramR10G10B10A2ToBgra32fCpu(
        ID3D11Texture2D* vramR10Tex,
        DXGI_FORMAT actualFmt,
        int width,
        int height,
        uint8_t* dstCpuPixels,
        int dstRowBytes,
        bool flipVertical = false);

    void recycleVramTexture(Microsoft::WRL::ComPtr<ID3D11Texture2D> tex,
                            int width,
                            int height,
                            DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM);
    void trimIdleResources();

private:
    GpuDeviceManager() = default;
    ~GpuDeviceManager();

    bool ensureStagingTextureLocked(int width, int height, DXGI_FORMAT fmt);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> acquireOutputTextureLocked(int width, int height, DXGI_FORMAT fmt, UINT bindFlags);

    struct PooledVramTex {
        int width = 0;
        int height = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    };

    mutable std::mutex mutex_;
    bool initialized_ = false;
    uint32_t vendorId_ = 0;
    uint32_t bestAdapterIndex_ = 0;
    std::string adapterName_ = "none";

    Microsoft::WRL::ComPtr<IDXGIAdapter1> bestAdapter_;
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
    DXGI_FORMAT stagingFmt_ = DXGI_FORMAT_UNKNOWN;
    std::vector<PooledVramTex> vramTexPool_;
};

} // namespace aer
