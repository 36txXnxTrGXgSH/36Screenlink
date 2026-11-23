#include "capture.h"

#include <cstring>
#include <cwchar>
#include <utility>

std::vector<OutputInfo> enumerateOutputs() {
    std::vector<OutputInfo> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC outDesc{};
            output->GetDesc(&outDesc);
            if (!outDesc.AttachedToDesktop) continue;
            OutputInfo info;
            info.adapter = adapter;
            output.As(&info.output);
            info.deviceName = outDesc.DeviceName;
            info.adapterName = desc.Description;
            info.adapterLuid = desc.AdapterLuid;
            info.vendorId = desc.VendorId;
            info.desktop = outDesc.DesktopCoordinates;
            info.primary = outDesc.DesktopCoordinates.left == 0 && outDesc.DesktopCoordinates.top == 0;
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            // Identifiant EDID fixé par le pilote : fabricant SXT, produit 0136.
            info.isVirtual = EnumDisplayDevicesW(outDesc.DeviceName, 0, &monitor, 0) && wcsstr(monitor.DeviceID, L"SXT0136");
            result.push_back(std::move(info));
        }
    }
    return result;
}

bool Capturer::open(const OutputInfo& info) {
    dupl_.Reset();
    if (!device_) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const HRESULT hr = D3D11CreateDevice(info.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                             device_.GetAddressOf(), nullptr, context_.GetAddressOf());
        if (FAILED(hr)) {
            error_ = "D3D11CreateDevice " + std::to_string(hr);
            return false;
        }
    }

    ComPtr<IDXGIOutput5> output5;
    HRESULT hr = E_FAIL;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
    if (SUCCEEDED(info.output.As(&output5)))
        hr = output5->DuplicateOutput1(device_.Get(), 0, 1, formats, dupl_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) hr = info.output->DuplicateOutput(device_.Get(), dupl_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error_ = "DuplicateOutput 0x" + std::to_string(static_cast<unsigned long>(hr));
        return false;
    }

    DXGI_OUTDUPL_DESC desc{};
    dupl_->GetDesc(&desc);
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;
    switch (desc.Rotation) {
    case DXGI_MODE_ROTATION_ROTATE90: rotation_ = 90; break;
    case DXGI_MODE_ROTATION_ROTATE180: rotation_ = 180; break;
    case DXGI_MODE_ROTATION_ROTATE270: rotation_ = 270; break;
    default: rotation_ = 0; break;
    }
    // Les dimensions annoncées suivent le bureau pivoté, mais la texture reste celle de la
    // sortie. La capture garde donc ce sens et la tablette applique la rotation.
    if (rotation_ == 90 || rotation_ == 270) std::swap(width_, height_);
    // Réouverture après un changement de mode : une réduction préparée pour l'ancienne taille
    // ne vaut plus.
    if (full_) {
        D3D11_TEXTURE2D_DESC fd{};
        full_->GetDesc(&fd);
        if (fd.Width != width_ || fd.Height != height_) {
            vout_.Reset(); vin_.Reset(); vproc_.Reset(); venum_.Reset(); full_.Reset();
            voutTarget_ = nullptr;
        }
    }
    return true;
}

bool Capturer::setScaledSize(UINT w, UINT h, RECT content) {
    if (!device_ || rotation_ != 0 || w == 0 || h == 0 || content.right <= content.left || content.bottom <= content.top) return false;
    if (FAILED(device_.As(&vdev_)) || FAILED(context_.As(&vctx_))) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, full_.ReleaseAndGetAddressOf()))) return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = width_;
    cd.InputHeight = height_;
    cd.OutputWidth = w;
    cd.OutputHeight = h;
    cd.InputFrameRate = {60, 1};
    cd.OutputFrameRate = {60, 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, venum_.ReleaseAndGetAddressOf())) ||
        FAILED(vdev_->CreateVideoProcessor(venum_.Get(), 0, vproc_.ReleaseAndGetAddressOf())) ||
        FAILED(vdev_->CreateVideoProcessorInputView(full_.Get(), venum_.Get(), &ivd, vin_.ReleaseAndGetAddressOf()))) {
        vin_.Reset(); vproc_.Reset(); venum_.Reset(); full_.Reset();
        return false;
    }
    // Bureau en RVB plein, sortie en RVB plein : aucune conversion, aucun traitement d'image.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
    cs.RGB_Range = 0;
    vctx_->VideoProcessorSetStreamColorSpace(vproc_.Get(), 0, &cs);
    vctx_->VideoProcessorSetOutputColorSpace(vproc_.Get(), &cs);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vproc_.Get(), 0, FALSE);
    vctx_->VideoProcessorSetStreamFrameFormat(vproc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    const RECT src{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    const RECT frame{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    vctx_->VideoProcessorSetStreamSourceRect(vproc_.Get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vproc_.Get(), 0, TRUE, &content);
    vctx_->VideoProcessorSetOutputTargetRect(vproc_.Get(), TRUE, &frame);
    D3D11_VIDEO_COLOR black{};
    black.RGBA.A = 1.0f;
    vctx_->VideoProcessorSetOutputBackgroundColor(vproc_.Get(), FALSE, &black);
    content_ = content;
    voutTarget_ = nullptr;
    return true;
}

bool Capturer::blitScaled(ID3D11Texture2D* target) {
    if (target != voutTarget_) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev_->CreateVideoProcessorOutputView(target, venum_.Get(), &ovd, vout_.ReleaseAndGetAddressOf()))) return false;
        voutTarget_ = target;
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = vin_.Get();
    return SUCCEEDED(vctx_->VideoProcessorBlt(vproc_.Get(), vout_.Get(), 0, 1, &stream));
}

void Capturer::readCursorShape(const DXGI_OUTDUPL_FRAME_INFO& info, CapturedFrame& frame) {
    if (info.PointerShapeBufferSize == 0) return;
    shapeBuf_.resize(info.PointerShapeBufferSize);
    UINT needed = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    if (FAILED(dupl_->GetFramePointerShape(info.PointerShapeBufferSize, shapeBuf_.data(), &needed, &shape))) return;

    CursorShape& out = frame.cursorShape;
    const bool mono = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const UINT h = mono ? shape.Height / 2 : shape.Height;
    out.width = static_cast<uint16_t>(shape.Width);
    out.height = static_cast<uint16_t>(h);
    out.hotX = static_cast<uint16_t>(shape.HotSpot.x);
    out.hotY = static_cast<uint16_t>(shape.HotSpot.y);
    out.bgra.assign(static_cast<size_t>(shape.Width) * h * 4, 0);

    for (UINT y = 0; y < h; ++y) {
        uint8_t* dst = out.bgra.data() + static_cast<size_t>(y) * shape.Width * 4;
        if (mono) {
            const uint8_t* andRow = shapeBuf_.data() + static_cast<size_t>(y) * shape.Pitch;
            const uint8_t* xorRow = andRow + static_cast<size_t>(h) * shape.Pitch;
            for (UINT x = 0; x < shape.Width; ++x) {
                const uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
                const bool a = andRow[x / 8] & bit, xr = xorRow[x / 8] & bit;
                const uint8_t v = xr ? 255 : 0;
                dst[x * 4] = dst[x * 4 + 1] = dst[x * 4 + 2] = v;
                dst[x * 4 + 3] = (a && !xr) ? 0 : 255;
            }
        } else {
            const uint8_t* src = shapeBuf_.data() + static_cast<size_t>(y) * shape.Pitch;
            std::memcpy(dst, src, static_cast<size_t>(shape.Width) * 4);
            // En MASKED_COLOR, l'alpha sert de masque : 0 = opaque.
            if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR)
                for (UINT x = 0; x < shape.Width; ++x) dst[x * 4 + 3] = dst[x * 4 + 3] ? 0 : 255;
        }
    }
    frame.cursorShapeChanged = true;
}

Capturer::Result Capturer::acquire(UINT timeoutMs, ID3D11Texture2D* target, CapturedFrame& frame) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, resource.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::Timeout;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) return Result::Lost;
    if (FAILED(hr)) {
        error_ = "AcquireNextFrame " + std::to_string(hr);
        return Result::Error;
    }

    if (info.LastMouseUpdateTime.QuadPart != 0) {
        frame.cursorMoved = true;
        frame.cursorVisible = info.PointerPosition.Visible != FALSE;
        frame.cursorX = info.PointerPosition.Position.x;
        frame.cursorY = info.PointerPosition.Position.y;
        if (vproc_) {
            frame.cursorX = content_.left + static_cast<int>(static_cast<int64_t>(frame.cursorX) * (content_.right - content_.left) / width_);
            frame.cursorY = content_.top + static_cast<int>(static_cast<int64_t>(frame.cursorY) * (content_.bottom - content_.top) / height_);
        }
    }
    readCursorShape(info, frame);

    if (info.LastPresentTime.QuadPart != 0 && target) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture))) {
            // Région plutôt que CopyResource : la cible peut être plus petite d'un pixel,
            // l'encodeur exigeant des dimensions paires.
            if (vproc_) {
                context_->CopySubresourceRegion(full_.Get(), 0, 0, 0, 0, texture.Get(), 0, nullptr);
                frame.imageUpdated = blitScaled(target);
            } else {
                context_->CopySubresourceRegion(target, 0, 0, 0, 0, texture.Get(), 0, nullptr);
                frame.imageUpdated = true;
            }
            frame.presentQpc = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
        }
    }
    dupl_->ReleaseFrame();
    return Result::Ok;
}
