#pragma once
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct OutputInfo {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    std::wstring deviceName;
    std::wstring adapterName;
    LUID adapterLuid{};
    UINT vendorId = 0;
    RECT desktop{};
    bool primary = false;
    bool isVirtual = false; // moniteur créé par notre pilote
};

std::vector<OutputInfo> enumerateOutputs();

struct CursorShape {
    uint16_t width = 0, height = 0, hotX = 0, hotY = 0;
    std::vector<uint8_t> bgra;
};

struct CapturedFrame {
    bool imageUpdated = false;
    uint64_t presentQpc = 0;
    bool cursorMoved = false;
    bool cursorVisible = false;
    int cursorX = 0, cursorY = 0;
    bool cursorShapeChanged = false;
    CursorShape cursorShape;
};

class Capturer {
public:
    enum class Result { Ok, Timeout, Lost, Error };

    bool open(const OutputInfo& info);
    // Copie l'image dans `target`, de mêmes dimensions et format BGRA, créée sur device().
    Result acquire(UINT timeoutMs, ID3D11Texture2D* target, CapturedFrame& frame);
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    UINT width() const { return width_; }
    UINT height() const { return height_; }
    // Rotation que le lecteur doit appliquer, en degrés.
    UINT rotationDegrees() const { return rotation_; }
    const std::string& lastError() const { return error_; }

private:
    void readCursorShape(const DXGI_OUTDUPL_FRAME_INFO& info, CapturedFrame& frame);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    std::vector<uint8_t> shapeBuf_;
    UINT width_ = 0, height_ = 0, rotation_ = 0;
    std::string error_;
};
