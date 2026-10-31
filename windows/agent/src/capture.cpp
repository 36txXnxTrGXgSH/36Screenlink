#include "capture.h"

#include <cstring>
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
    return true;
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
    }
    readCursorShape(info, frame);

    if (info.LastPresentTime.QuadPart != 0 && target) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource.As(&texture))) {
            // Région plutôt que CopyResource : la cible peut être plus petite d'un pixel,
            // l'encodeur exigeant des dimensions paires.
            context_->CopySubresourceRegion(target, 0, 0, 0, 0, texture.Get(), 0, nullptr);
            frame.imageUpdated = true;
            frame.presentQpc = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
        }
    }
    dupl_->ReleaseFrame();
    return Result::Ok;
}
