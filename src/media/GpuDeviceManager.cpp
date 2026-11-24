#include "media/GpuDeviceManager.hpp"
#include "media/DemuxEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace aer {

GpuDeviceManager& GpuDeviceManager::instance() {
    static GpuDeviceManager s_mgr;
    return s_mgr;
}

GpuDeviceManager::~GpuDeviceManager() {
    for (auto& p : vramTexPool_) {
        p.tex.Detach();
    }
    vramTexPool_.clear();
    bestAdapter_.Detach();
    adapter3_.Detach();
    vpEnum_.Detach();
    videoProcessor_.Detach();
    stagingTex_.Detach();
    videoContext_.Detach();
    videoDevice_.Detach();
    context_.Detach();
    device_.Detach();
    ffHwDeviceRef_ = nullptr;
}

bool GpuDeviceManager::ensureInitialized() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return ffHwDeviceRef_ != nullptr;
    }
    initialized_ = true;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return false;
    }

    DXGI_ADAPTER_DESC1 bestDesc{};
    uint64_t bestScore = 0;

    for (UINT idx = 0;; ++idx) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> cand;
        if (factory->EnumAdapters1(idx, cand.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(cand->GetDesc1(&desc))) continue;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        if (desc.DedicatedVideoMemory < 256ULL * 1024ULL * 1024ULL) continue;

        uint64_t score = static_cast<uint64_t>(desc.DedicatedVideoMemory);
        if (desc.VendorId == 0x10DE) {
            score += (1ULL << 40);
        }
        if (score > bestScore) {
            bestScore = score;
            bestAdapter_ = cand;
            bestAdapterIndex_ = idx;
            bestDesc = desc;
        }
    }

    if (!bestAdapter_) {
        return false;
    }

    bestAdapter_.As(&adapter3_);
    vendorId_ = bestDesc.VendorId;
    adapterName_ = DemuxEngine::wideToUtf8(std::wstring(bestDesc.Description));

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_11_0;
    const UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    if (FAILED(D3D11CreateDevice(
            bestAdapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            createFlags,
            levels,
            2,
            D3D11_SDK_VERSION,
            device_.GetAddressOf(),
            &achieved,
            context_.GetAddressOf()))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
    }

    if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
        return false;
    }

    ffHwDeviceRef_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ffHwDeviceRef_) {
        return false;
    }

    auto* hwDevCtx = reinterpret_cast<AVHWDeviceContext*>(ffHwDeviceRef_->data);
    auto* d3d11Ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwDevCtx->hwctx);
    device_->AddRef();
    d3d11Ctx->device = device_.Get();
    context_->AddRef();
    d3d11Ctx->device_context = context_.Get();
    videoDevice_->AddRef();
    d3d11Ctx->video_device = videoDevice_.Get();
    videoContext_->AddRef();
    d3d11Ctx->video_context = videoContext_.Get();
    d3d11Ctx->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

    if (av_hwdevice_ctx_init(ffHwDeviceRef_) < 0) {
        av_buffer_unref(&ffHwDeviceRef_);
        return false;
    }
    return true;
}

AVBufferRef* GpuDeviceManager::acquireSharedD3D11HwDeviceRef() {
    if (!ensureInitialized()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return ffHwDeviceRef_ ? av_buffer_ref(ffHwDeviceRef_) : nullptr;
}

AVBufferRef* GpuDeviceManager::createIsolatedD3D11HwDeviceRef() {
    if (!ensureInitialized()) {
        return nullptr;
    }
    AVBufferRef* isolatedRef = nullptr;
    const std::string adapterIdxStr = std::to_string(bestAdapterIndex_);
    if (av_hwdevice_ctx_create(&isolatedRef, AV_HWDEVICE_TYPE_D3D11VA, adapterIdxStr.c_str(), nullptr, 0) == 0 &&
        isolatedRef) {
        return isolatedRef;
    }
    return acquireSharedD3D11HwDeviceRef();
}

VramStatus GpuDeviceManager::queryVramStatus(uint64_t aerCachedVramBytes) const {
    VramStatus st{};
    st.aerCachedVramBytes = aerCachedVramBytes;
    if (!adapter3_) {
        return st;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        st.budgetBytes = info.Budget;
        st.currentUsageBytes = info.CurrentUsage;
        st.valid = (info.Budget > 0);
    }
    return st;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> GpuDeviceManager::copyHwFrameToVramYuv(
    const AVFrame* hwFrame,
    DXGI_FORMAT& outDxgiFormat,
    int& outAllocWidth,
    int& outAllocHeight) {
    outDxgiFormat = DXGI_FORMAT_UNKNOWN;
    outAllocWidth = 0;
    outAllocHeight = 0;
    if (!hwFrame || !hwFrame->data[0] || !ensureInitialized()) {
        return nullptr;
    }

    auto* srcTex = reinterpret_cast<ID3D11Texture2D*>(hwFrame->data[0]);
    const UINT srcSlice = static_cast<UINT>(reinterpret_cast<intptr_t>(hwFrame->data[1]));

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);
    if (srcDesc.Format != DXGI_FORMAT_NV12 && srcDesc.Format != DXGI_FORMAT_P010) {
        return nullptr;
    }

    // Check that srcTex belongs to our shared D3D11 device
    Microsoft::WRL::ComPtr<ID3D11Device> srcDev;
    srcTex->GetDevice(srcDev.GetAddressOf());

    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !context_ || srcDev.Get() != device_.Get()) {
        return nullptr;
    }

    const int allocW = static_cast<int>(srcDesc.Width);
    const int allocH = static_cast<int>(srcDesc.Height);
    // BindFlags = 0 for NV12/P010 VRAM cache surfaces guarantees CreateTexture2D succeeds on all D3D11 drivers!
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outTex =
        acquireOutputTextureLocked(allocW, allocH, srcDesc.Format, 0);
    if (!outTex) {
        return nullptr;
    }

    auto* hwDevCtx = reinterpret_cast<AVHWDeviceContext*>(ffHwDeviceRef_->data);
    auto* d3d11Ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwDevCtx->hwctx);
    if (d3d11Ctx->lock) d3d11Ctx->lock(d3d11Ctx->lock_ctx);
    context_->CopySubresourceRegion(outTex.Get(), 0, 0, 0, 0, srcTex, srcSlice, nullptr);
    if (d3d11Ctx->unlock) d3d11Ctx->unlock(d3d11Ctx->lock_ctx);

    outDxgiFormat = srcDesc.Format;
    outAllocWidth = allocW;
    outAllocHeight = allocH;
    return outTex;
}

bool GpuDeviceManager::readbackVramYuvToCpu(
    ID3D11Texture2D* yuvTex,
    DXGI_FORMAT fmt,
    int visibleWidth,
    int visibleHeight,
    int texAllocWidth,
    int texAllocHeight,
    uint8_t* dstBase,
    int dstRowBytes) {
    if (!yuvTex || !dstBase || visibleWidth <= 0 || visibleHeight <= 0 || dstRowBytes == 0) {
        return false;
    }
    if (fmt != DXGI_FORMAT_NV12 && fmt != DXGI_FORMAT_P010) {
        return false;
    }

    const int allocW = texAllocWidth > 0 ? texAllocWidth : visibleWidth;
    const int allocH = texAllocHeight > 0 ? texAllocHeight : visibleHeight;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !context_ || !ensureStagingTextureLocked(allocW, allocH, fmt)) {
        return false;
    }

    context_->CopyResource(stagingTex_.Get(), yuvTex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
        return false;
    }

    const int bytesPerSample = (fmt == DXGI_FORMAT_P010) ? 2 : 1;
    const size_t copyRowBytes = static_cast<size_t>(visibleWidth) * static_cast<size_t>(bytesPerSample);
    const size_t absDstStride = static_cast<size_t>(std::abs(dstRowBytes));
    const size_t srcStride = static_cast<size_t>(mapped.RowPitch);

    const auto* srcY = static_cast<const uint8_t*>(mapped.pData);
    const auto* srcUV = srcY + srcStride * static_cast<size_t>(allocH);

    uint8_t* dstY = dstBase;
    uint8_t* dstUV = dstBase + absDstStride * static_cast<size_t>(visibleHeight);

    // Plane 0: Y (Top-Down)
    if (absDstStride == srcStride && srcStride == copyRowBytes) {
        std::memcpy(dstY, srcY, copyRowBytes * static_cast<size_t>(visibleHeight));
    } else {
        for (int y = 0; y < visibleHeight; ++y) {
            std::memcpy(dstY + static_cast<size_t>(y) * absDstStride,
                        srcY + static_cast<size_t>(y) * srcStride,
                        copyRowBytes);
        }
    }

    // Plane 1: Interleaved UV (Top-Down, visibleHeight / 2 rows)
    const int uvRows = visibleHeight / 2;
    if (absDstStride == srcStride && srcStride == copyRowBytes) {
        std::memcpy(dstUV, srcUV, copyRowBytes * static_cast<size_t>(uvRows));
    } else {
        for (int y = 0; y < uvRows; ++y) {
            std::memcpy(dstUV + static_cast<size_t>(y) * absDstStride,
                        srcUV + static_cast<size_t>(y) * srcStride,
                        copyRowBytes);
        }
    }

    context_->Unmap(stagingTex_.Get(), 0);
    return true;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> GpuDeviceManager::convertHwFrameToVramBgra8(
    const AVFrame* hwFrame,
    int dstWidth,
    int dstHeight,
    DXGI_FORMAT targetDxgiFmt) {
    if (!hwFrame || !hwFrame->data[0] || dstWidth <= 0 || dstHeight <= 0) {
        return nullptr;
    }
    if (!ensureInitialized()) {
        return nullptr;
    }

    auto* srcTex = reinterpret_cast<ID3D11Texture2D*>(hwFrame->data[0]);
    const UINT srcSlice = static_cast<UINT>(reinterpret_cast<intptr_t>(hwFrame->data[1]));

    Microsoft::WRL::ComPtr<ID3D11Device> srcDev;
    srcTex->GetDevice(srcDev.GetAddressOf());

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ffHwDeviceRef_ || !videoDevice_ || !videoContext_ || !device_ || srcDev.Get() != device_.Get()) {
        return nullptr;
    }

    auto* hwDevCtx = reinterpret_cast<AVHWDeviceContext*>(ffHwDeviceRef_->data);
    auto* d3d11Ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwDevCtx->hwctx);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outTex = acquireOutputTextureLocked(
        dstWidth, dstHeight, targetDxgiFmt, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!outTex) {
        return nullptr;
    }

    const int srcW = hwFrame->width;
    const int srcH = hwFrame->height;

    if (!vpEnum_ || !videoProcessor_ || vpInW_ != srcW || vpInH_ != srcH ||
        vpOutW_ != dstWidth || vpOutH_ != dstHeight) {
        vpEnum_.Reset();
        videoProcessor_.Reset();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = static_cast<UINT>(srcW);
        contentDesc.InputHeight = static_cast<UINT>(srcH);
        contentDesc.OutputWidth = static_cast<UINT>(dstWidth);
        contentDesc.OutputHeight = static_cast<UINT>(dstHeight);
        contentDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

        if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, vpEnum_.GetAddressOf())) ||
            FAILED(videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, videoProcessor_.GetAddressOf()))) {
            vpEnum_.Reset();
            videoProcessor_.Reset();
            return nullptr;
        }
        vpInW_ = srcW;
        vpInH_ = srcH;
        vpOutW_ = dstWidth;
        vpOutH_ = dstHeight;
    }

    if (d3d11Ctx->lock) d3d11Ctx->lock(d3d11Ctx->lock_ctx);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inViewDesc{};
    inViewDesc.FourCC = 0;
    inViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inViewDesc.Texture2D.MipSlice = 0;
    inViewDesc.Texture2D.ArraySlice = srcSlice;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
        srcTex, vpEnum_.Get(), &inViewDesc, inView.GetAddressOf());

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
    if (SUCCEEDED(hr)) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc{};
        outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outViewDesc.Texture2D.MipSlice = 0;
        hr = videoDevice_->CreateVideoProcessorOutputView(
            outTex.Get(), vpEnum_.Get(), &outViewDesc, outView.GetAddressOf());
    }

    if (SUCCEEDED(hr)) {
        RECT srcRect{0, 0, srcW, srcH};
        RECT dstRect{0, 0, dstWidth, dstHeight};
        videoContext_->VideoProcessorSetStreamSourceRect(videoProcessor_.Get(), 0, TRUE, &srcRect);
        videoContext_->VideoProcessorSetStreamDestRect(videoProcessor_.Get(), 0, TRUE, &dstRect);
        videoContext_->VideoProcessorSetOutputTargetRect(videoProcessor_.Get(), TRUE, &dstRect);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
        inCs.Usage = 0;
        inCs.RGB_Range = 0;
        inCs.YCbCr_Matrix = (hwFrame->colorspace == AVCOL_SPC_SMPTE170M || hwFrame->colorspace == AVCOL_SPC_BT470BG) ? 0 : 1;
        inCs.Nominal_Range = (hwFrame->color_range == AVCOL_RANGE_JPEG) ? 2 : 1;
        videoContext_->VideoProcessorSetStreamColorSpace(videoProcessor_.Get(), 0, &inCs);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
        outCs.Usage = 0;
        outCs.RGB_Range = 0;
        videoContext_->VideoProcessorSetOutputColorSpace(videoProcessor_.Get(), &outCs);

        D3D11_VIDEO_PROCESSOR_STREAM streamData{};
        streamData.Enable = TRUE;
        streamData.pInputSurface = inView.Get();

        hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outView.Get(), 0, 1, &streamData);
    }

    if (d3d11Ctx->unlock) d3d11Ctx->unlock(d3d11Ctx->lock_ctx);

    if (FAILED(hr)) {
        return nullptr;
    }
    return outTex;
}

bool GpuDeviceManager::ensureStagingTextureLocked(int width, int height, DXGI_FORMAT fmt) {
    if (stagingTex_ && stagingW_ == width && stagingH_ == height && stagingFmt_ == fmt) {
        return true;
    }
    stagingTex_.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(device_->CreateTexture2D(&desc, nullptr, stagingTex_.GetAddressOf()))) {
        return false;
    }
    stagingW_ = width;
    stagingH_ = height;
    stagingFmt_ = fmt;
    return true;
}

bool GpuDeviceManager::readbackVramTextureToCpu(
    ID3D11Texture2D* vramTex,
    int width,
    int height,
    uint8_t* dstCpuPixels,
    int dstRowBytes,
    bool flipVertical) {
    if (!vramTex || !dstCpuPixels || width <= 0 || height <= 0 || dstRowBytes == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !context_ || !ensureStagingTextureLocked(width, height, DXGI_FORMAT_B8G8R8A8_UNORM)) {
        return false;
    }

    context_->CopyResource(stagingTex_.Get(), vramTex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
        return false;
    }

    const auto* srcBase = static_cast<const uint8_t*>(mapped.pData);
    const size_t copyBytes = static_cast<size_t>(width) * 4ULL;
    const ptrdiff_t signedDstStride = static_cast<ptrdiff_t>(dstRowBytes);

    if (!flipVertical && signedDstStride > 0 &&
        static_cast<UINT>(signedDstStride) == mapped.RowPitch && mapped.RowPitch == copyBytes) {
        std::memcpy(dstCpuPixels, srcBase, copyBytes * static_cast<size_t>(height));
    } else {
        for (int y = 0; y < height; ++y) {
            const int dstY = flipVertical ? (height - 1 - y) : y;
            std::memcpy(dstCpuPixels + static_cast<ptrdiff_t>(dstY) * signedDstStride,
                        srcBase + static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch),
                        copyBytes);
        }
    }

    context_->Unmap(stagingTex_.Get(), 0);
    return true;
}

bool GpuDeviceManager::readbackVramR10G10B10A2ToBgra32fCpu(
    ID3D11Texture2D* vramR10Tex,
    DXGI_FORMAT actualFmt,
    int width,
    int height,
    uint8_t* dstCpuPixels,
    int dstRowBytes,
    bool flipVertical) {
    if (!vramR10Tex || !dstCpuPixels || width <= 0 || height <= 0 || dstRowBytes == 0) {
        return false;
    }
    if (actualFmt != DXGI_FORMAT_R10G10B10A2_UNORM && actualFmt != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !context_ || !ensureStagingTextureLocked(width, height, actualFmt)) {
        return false;
    }

    context_->CopyResource(stagingTex_.Get(), vramR10Tex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
        return false;
    }

    const auto* srcBase = static_cast<const uint8_t*>(mapped.pData);
    const ptrdiff_t signedDstStride = static_cast<ptrdiff_t>(dstRowBytes);

    if (actualFmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
        constexpr float kInv1023 = 1.0f / 1023.0f;
        constexpr float kInv3 = 1.0f / 3.0f;
        for (int y = 0; y < height; ++y) {
            const int dstY = flipVertical ? (height - 1 - y) : y;
            const auto* srcRow = reinterpret_cast<const uint32_t*>(srcBase + static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch));
            auto* dstRow = reinterpret_cast<float*>(dstCpuPixels + static_cast<ptrdiff_t>(dstY) * signedDstStride);
            for (int x = 0; x < width; ++x) {
                const uint32_t p = srcRow[x];
                dstRow[x * 4 + 0] = static_cast<float>((p >> 20) & 0x3FFu) * kInv1023; // B
                dstRow[x * 4 + 1] = static_cast<float>((p >> 10) & 0x3FFu) * kInv1023; // G
                dstRow[x * 4 + 2] = static_cast<float>(p & 0x3FFu) * kInv1023;         // R
                dstRow[x * 4 + 3] = static_cast<float>((p >> 30) & 0x3u) * kInv3;      // A
            }
        }
    } else {
        constexpr float kInv255 = 1.0f / 255.0f;
        for (int y = 0; y < height; ++y) {
            const int dstY = flipVertical ? (height - 1 - y) : y;
            const auto* srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch);
            auto* dstRow = reinterpret_cast<float*>(dstCpuPixels + static_cast<ptrdiff_t>(dstY) * signedDstStride);
            for (int x = 0; x < width; ++x) {
                dstRow[x * 4 + 0] = static_cast<float>(srcRow[x * 4 + 0]) * kInv255;
                dstRow[x * 4 + 1] = static_cast<float>(srcRow[x * 4 + 1]) * kInv255;
                dstRow[x * 4 + 2] = static_cast<float>(srcRow[x * 4 + 2]) * kInv255;
                dstRow[x * 4 + 3] = static_cast<float>(srcRow[x * 4 + 3]) * kInv255;
            }
        }
    }

    context_->Unmap(stagingTex_.Get(), 0);
    return true;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> GpuDeviceManager::acquireOutputTextureLocked(
    int width, int height, DXGI_FORMAT fmt, UINT bindFlags) {
    for (auto it = vramTexPool_.begin(); it != vramTexPool_.end(); ++it) {
        if (it->width == width && it->height == height && it->fmt == fmt && it->tex) {
            D3D11_TEXTURE2D_DESC existingDesc{};
            it->tex->GetDesc(&existingDesc);
            if (existingDesc.BindFlags == bindFlags) {
                auto reused = std::move(it->tex);
                vramTexPool_.erase(it);
                return reused;
            }
        }
    }

    D3D11_TEXTURE2D_DESC outDesc{};
    outDesc.Width = static_cast<UINT>(width);
    outDesc.Height = static_cast<UINT>(height);
    outDesc.MipLevels = 1;
    outDesc.ArraySize = 1;
    outDesc.Format = fmt;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = bindFlags;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outTex;
    if (FAILED(device_->CreateTexture2D(&outDesc, nullptr, outTex.GetAddressOf()))) {
        return nullptr;
    }
    return outTex;
}

void GpuDeviceManager::recycleVramTexture(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex, int width, int height, DXGI_FORMAT fmt) {
    if (!tex || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr size_t kMaxPooledTextures = 10;
    if (vramTexPool_.size() >= kMaxPooledTextures) {
        vramTexPool_.erase(vramTexPool_.begin());
    }
    vramTexPool_.push_back(PooledVramTex{width, height, fmt, std::move(tex)});
}

void GpuDeviceManager::trimIdleResources() {
    std::lock_guard<std::mutex> lock(mutex_);
    vramTexPool_.clear();
    stagingTex_.Reset();
    stagingW_ = 0;
    stagingH_ = 0;
    stagingFmt_ = DXGI_FORMAT_UNKNOWN;
}

} // namespace aer
