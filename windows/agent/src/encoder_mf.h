#pragma once
#include <d3d11.h>
#include <d3d11_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include "encoder.h"

// Encodeur matériel du GPU via Media Foundation : AMD, Intel, processeurs avec partie
// graphique, et NVIDIA en repli. Windows choisit le pilote fourni par le fabricant.
class MediaFoundationEncoder : public Encoder {
public:
    ~MediaFoundationEncoder() override;
    bool open(ID3D11Device* device, ID3D11Texture2D* input, EncoderConfig& cfg) override;
    std::vector<uint8_t> sequenceHeaders() override { return headers_; }
    bool encode(uint64_t ptsUs, bool forceIdr, const Sink& sink) override;
    bool setBitrate(uint32_t bitrate) override;

private:
    bool createTransform(const GUID& subtype, const LUID& adapter);
    bool configure(const GUID& subtype);
    bool createConverter();
    bool convertFrame();
    bool pumpEvents(const Sink& sink, uint32_t waitMs, bool stopOnOutput);
    bool drainOutput(const Sink& sink);
    bool fail(const char* what, HRESULT hr);

    template <typename T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<ID3D11Texture2D> input_, nv12_;
    Ptr<ID3D11VideoDevice> videoDevice_;
    Ptr<ID3D11VideoContext> videoContext_;
    Ptr<ID3D11VideoProcessorEnumerator> processorEnum_;
    Ptr<ID3D11VideoProcessor> processor_;
    Ptr<ID3D11VideoProcessorInputView> inputView_;
    Ptr<ID3D11VideoProcessorOutputView> outputView_;
    Ptr<IMFDXGIDeviceManager> manager_;
    Ptr<IMFActivate> activate_;
    Ptr<IMFTransform> transform_;
    Ptr<IMFMediaEventGenerator> events_;
    Ptr<ICodecAPI> codec_;
    DWORD inputStream_ = 0, outputStream_ = 0;
    uint32_t pendingInputs_ = 0;
    bool started_ = false, samplesProvided_ = true;
    UINT resetToken_ = 0;
    EncoderConfig cfg_;
    std::vector<uint8_t> headers_;
};
