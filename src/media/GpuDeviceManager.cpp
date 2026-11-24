#include "media/GpuDeviceManager.hpp"
#include "media/DemuxEngine.hpp"

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

    Microsoft::WRL::ComPtr<IDXGIAdapter1> bestAdapter;
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
            bestAdapter = cand;
            bestDesc = desc;
        }
    }

    if (!bestAdapter) {
        return false;
    }

    bestAdapter.As(&adapter3_);
    vendorId_ = bestDesc.VendorId;
    adapterName_ = DemuxEngine::wideToUtf8(std::wstring(bestDesc.Description));

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_11_0;
    const UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    if (FAILED(D3D11CreateDevice(
            bestAdapter.Get(),
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

Microsoft::WRL::ComPtr<ID3D11Texture2D> GpuDeviceManager::convertHwFrameToVramBgra8(
    const AVFrame* hwFrame,
    int dstWidth,
    int dstHeight) {
    if (!hwFrame || !hwFrame->data[0] || dstWidth <= 0 || dstHeight <= 0) {
        return nullptr;
    }
    if (!ensureInitialized()) {
        return nullptr;
    }

    auto* srcTex = reinterpret_cast<ID3D11Texture2D*>(hwFrame->data[0]);
    const UINT srcSlice = static_cast<UINT>(reinterpret_cast<intptr_t>(hwFrame->data[1]));

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ffHwDeviceRef_ || !videoDevice_ || !videoContext_ || !device_) {
        return nullptr;
    }

    auto* hwDevCtx = reinterpret_cast<AVHWDeviceContext*>(ffHwDeviceRef_->data);
    auto* d3d11Ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwDevCtx->hwctx);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outTex = acquireOutputTextureLocked(dstWidth, dstHeight);
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

bool GpuDeviceManager::ensureStagingTextureLocked(int width, int height) {
    if (stagingTex_ && stagingW_ == width && stagingH_ == height) {
        return true;
    }
    stagingTex_.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(device_->CreateTexture2D(&desc, nullptr, stagingTex_.GetAddressOf()))) {
        return false;
    }
    stagingW_ = width;
    stagingH_ = height;
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
    if (!device_ || !context_ || !ensureStagingTextureLocked(width, height)) {
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

Microsoft::WRL::ComPtr<ID3D11Texture2D> GpuDeviceManager::acquireOutputTextureLocked(int width, int height) {
    for (auto it = vramTexPool_.begin(); it != vramTexPool_.end(); ++it) {
        if (it->width == width && it->height == height && it->tex) {
            auto reused = std::move(it->tex);
            vramTexPool_.erase(it);
            return reused;
        }
    }

    D3D11_TEXTURE2D_DESC outDesc{};
    outDesc.Width = static_cast<UINT>(width);
    outDesc.Height = static_cast<UINT>(height);
    outDesc.MipLevels = 1;
    outDesc.ArraySize = 1;
    outDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outTex;
    if (FAILED(device_->CreateTexture2D(&outDesc, nullptr, outTex.GetAddressOf()))) {
        return nullptr;
    }
    return outTex;
}

void GpuDeviceManager::recycleVramTexture(Microsoft::WRL::ComPtr<ID3D11Texture2D> tex, int width, int height) {
    if (!tex || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr size_t kMaxPooledTextures = 6;
    if (vramTexPool_.size() >= kMaxPooledTextures) {
        vramTexPool_.erase(vramTexPool_.begin());
    }
    vramTexPool_.push_back(PooledVramTex{width, height, std::move(tex)});
}

void GpuDeviceManager::trimIdleResources() {
    std::lock_guard<std::mutex> lock(mutex_);
    vramTexPool_.clear();
    stagingTex_.Reset();
    stagingW_ = 0;
    stagingH_ = 0;
}

} // namespace aer
