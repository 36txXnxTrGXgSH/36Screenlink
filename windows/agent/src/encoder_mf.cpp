#include <initguid.h>

#include "encoder_mf.h"
#include "logw.h"

#include <codecapi.h>
#include <d3d10.h>
#include <dxgi.h>
#include <mferror.h>

#include <chrono>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kInfiniteGop = 0xFFFFFFFF;

HRESULT setCodecValue(ICodecAPI* codec, const GUID& key, ULONG value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    return codec->SetValue(&key, &v);
}

ComPtr<IMFMediaType> videoType(const GUID& subtype, const EncoderConfig& cfg) {
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return type;
}

} // namespace

bool MediaFoundationEncoder::fail(const char* what, HRESULT hr) {
    char text[120];
    sprintf_s(text, "%s : 0x%08lx", what, static_cast<unsigned long>(hr));
    error_ = text;
    return false;
}

MediaFoundationEncoder::~MediaFoundationEncoder() {
    if (transform_ && started_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    if (activate_) activate_->ShutdownObject();
    transform_.Reset();
    events_.Reset();
    codec_.Reset();
    manager_.Reset();
    MFShutdown();
}

bool MediaFoundationEncoder::createTransform(const GUID& subtype, const LUID& adapter) {
    ComPtr<IMFAttributes> filter;
    MFCreateAttributes(&filter, 1);
    filter->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapter), sizeof(adapter));

    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, subtype};
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &output,
                          filter.Get(), &list, &count);
    if (FAILED(hr) || count == 0) {
        if (list) CoTaskMemFree(list);
        return fail("aucun encodeur matériel", FAILED(hr) ? hr : MF_E_NOT_FOUND);
    }
    activate_ = list[0];
    for (UINT32 i = 0; i < count; ++i) list[i]->Release();
    CoTaskMemFree(list);

    hr = activate_->ActivateObject(IID_PPV_ARGS(&transform_));
    if (FAILED(hr)) return fail("activation de l'encodeur", hr);

    wchar_t name[128] = {};
    activate_->GetString(MFT_FRIENDLY_NAME_Attribute, name, 128, nullptr);
    logw(L"Encodeur matériel : %ls\n", name);
    return true;
}

bool MediaFoundationEncoder::configure(const GUID& subtype) {
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = transform_->GetAttributes(&attributes);
    if (FAILED(hr)) return fail("attributs", hr);
    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

    hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.Get()));
    if (FAILED(hr)) return fail("liaison Direct3D", hr);

    if (SUCCEEDED(transform_.As(&codec_))) {
        // Réglages facultatifs : un pilote qui en refuse un reste utilisable.
        setCodecValue(codec_.Get(), CODECAPI_AVLowLatencyMode, 1);
        setCodecValue(codec_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setCodecValue(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, cfg_.bitrate);
        setCodecValue(codec_.Get(), CODECAPI_AVEncMPVGOPSize, kInfiniteGop >> 1);
        setCodecValue(codec_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    }

    auto output = videoType(subtype, cfg_);
    output->SetUINT32(MF_MT_AVG_BITRATE, cfg_.bitrate);
    if (subtype == MFVideoFormat_HEVC) output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
    else output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    hr = transform_->SetOutputType(0, output.Get(), 0);
    if (FAILED(hr)) return fail("format de sortie", hr);

    hr = transform_->SetInputType(0, videoType(MFVideoFormat_NV12, cfg_).Get(), 0);
    if (FAILED(hr)) return fail("format d'entrée", hr);

    DWORD inputCount = 0, outputCount = 0;
    if (SUCCEEDED(transform_->GetStreamCount(&inputCount, &outputCount)) && inputCount == 1 && outputCount == 1) {
        DWORD in = 0, out = 0;
        if (SUCCEEDED(transform_->GetStreamIDs(1, &in, 1, &out))) {
            inputStream_ = in;
            outputStream_ = out;
        }
    }
    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(transform_->GetOutputStreamInfo(outputStream_, &info)))
        samplesProvided_ = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    ComPtr<IMFMediaType> current;
    if (SUCCEEDED(transform_->GetOutputCurrentType(outputStream_, &current))) {
        UINT32 size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
            headers_.resize(size);
            current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, headers_.data(), size, nullptr);
        }
    }

    hr = transform_.As(&events_);
    if (FAILED(hr)) return fail("événements de l'encodeur", hr);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    started_ = true;
    return true;
}

bool MediaFoundationEncoder::createConverter() {
    HRESULT hr = device_.As(&videoDevice_);
    if (SUCCEEDED(hr)) hr = context_.As(&videoContext_);
    if (FAILED(hr)) return fail("processeur vidéo indisponible", hr);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = cfg_.width;
    desc.Height = cfg_.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
    for (int i = 0; i < kSlots; ++i) {
        hr = device_->CreateTexture2D(&desc, nullptr, &nv12_[i]);
        if (FAILED(hr) && desc.BindFlags != D3D11_BIND_RENDER_TARGET) {
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = device_->CreateTexture2D(&desc, nullptr, &nv12_[i]);
        }
        if (FAILED(hr)) return fail("texture NV12", hr);
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {cfg_.fps, 1};
    content.OutputFrameRate = {cfg_.fps, 1};
    content.InputWidth = content.OutputWidth = cfg_.width;
    content.InputHeight = content.OutputHeight = cfg_.height;
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    hr = videoDevice_->CreateVideoProcessorEnumerator(&content, &processorEnum_);
    if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessor(processorEnum_.Get(), 0, &processor_);
    if (FAILED(hr)) return fail("processeur vidéo", hr);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    hr = videoDevice_->CreateVideoProcessorInputView(input_.Get(), processorEnum_.Get(), &inputDesc, &inputView_);
    if (FAILED(hr)) return fail("vue d'entrée", hr);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (int i = 0; i < kSlots; ++i) {
        hr = videoDevice_->CreateVideoProcessorOutputView(nv12_[i].Get(), processorEnum_.Get(), &outputDesc, &outputView_[i]);
        if (FAILED(hr)) return fail("vue de sortie", hr);
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_[i].Get(), 0, FALSE, &buffer);
        if (FAILED(hr)) return fail("tampon d'image", hr);
        ComPtr<IMF2DBuffer> buffer2d;
        DWORD length = 0;
        if (SUCCEEDED(buffer.As(&buffer2d))) buffer2d->GetContiguousLength(&length);
        buffer->SetCurrentLength(length);
        MFCreateSample(&inputSamples_[i]);
        inputSamples_[i]->AddBuffer(buffer.Get());
    }

    // BT.709 limité : c'est ce que lit le décodeur de la tablette.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE source{};
    source.RGB_Range = 0;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &source);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE target{};
    target.YCbCr_Matrix = 1;
    target.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetOutputColorSpace(processor_.Get(), &target);
    return true;
}

bool MediaFoundationEncoder::open(ID3D11Device* device, ID3D11Texture2D* input, EncoderConfig& cfg) {
    device_ = device;
    input_ = input;
    cfg_ = cfg;
    device_->GetImmediateContext(&context_);

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return fail("Media Foundation", hr);

    // L'encodeur travaille sur son propre thread avec ce même périphérique.
    ComPtr<ID3D10Multithread> protection;
    if (SUCCEEDED(device_.As(&protection))) protection->SetMultithreadProtected(TRUE);

    hr = MFCreateDXGIDeviceManager(&resetToken_, &manager_);
    if (SUCCEEDED(hr)) hr = manager_->ResetDevice(device_.Get(), resetToken_);
    if (FAILED(hr)) return fail("gestionnaire Direct3D", hr);

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (FAILED(device_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&adapterDesc)))
        return fail("adaptateur", E_FAIL);

    if (!createConverter()) return false;

    // HEVC d'abord, H.264 sinon : tous les GPU récents proposent au moins le second.
    const GUID order[] = {MFVideoFormat_HEVC, MFVideoFormat_H264};
    for (const GUID& subtype : order) {
        if (subtype == MFVideoFormat_HEVC && !cfg.hevc) continue;
        activate_.Reset();
        transform_.Reset();
        codec_.Reset();
        if (createTransform(subtype, adapterDesc.AdapterLuid) && configure(subtype)) {
            cfg.hevc = subtype == MFVideoFormat_HEVC;
            cfg_.hevc = cfg.hevc;
            return true;
        }
        std::printf("Encodeur %s : %s\n", subtype == MFVideoFormat_HEVC ? "HEVC" : "H.264", error_.c_str());
        if (activate_) activate_->ShutdownObject();
    }
    return false;
}

bool MediaFoundationEncoder::convertFrame(int slot) {
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView_.Get();
    const HRESULT hr = videoContext_->VideoProcessorBlt(processor_.Get(), outputView_[slot].Get(), 0, 1, &stream);
    return SUCCEEDED(hr) || fail("conversion NV12", hr);
}

bool MediaFoundationEncoder::drainOutput(const Sink& sink) {
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = outputStream_;
    if (!samplesProvided_) {
        // Tampon de sortie fourni par nous : créé une fois, réutilisé à chaque image.
        if (!outputSample_) {
            MFT_OUTPUT_STREAM_INFO info{};
            transform_->GetOutputStreamInfo(outputStream_, &info);
            ComPtr<IMFMediaBuffer> buffer;
            MFCreateSample(&outputSample_);
            MFCreateMemoryBuffer(info.cbSize, &buffer);
            outputSample_->AddBuffer(buffer.Get());
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(outputSample_->GetBufferByIndex(0, &buffer))) buffer->SetCurrentLength(0);
        outputSample_->DeleteAllItems();
        output.pSample = outputSample_.Get();
    }
    DWORD status = 0;
    HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) return true;
    if (FAILED(hr)) return fail("sortie de l'encodeur", hr);

    ComPtr<IMFSample> sample;
    if (samplesProvided_) sample.Attach(output.pSample);
    else sample = outputSample_;
    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) return fail("tampon de sortie", hr);
    BYTE* data = nullptr;
    DWORD length = 0;
    hr = buffer->Lock(&data, nullptr, &length);
    if (FAILED(hr)) return fail("lecture du tampon", hr);
    const bool key = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != 0;
    const bool sent = sink(data, length, key);
    buffer->Unlock();
    if (!sent) error_ = "envoi interrompu";
    return sent;
}

// Encodeur asynchrone : il signale quand il veut une image et quand il en a produit une.
bool MediaFoundationEncoder::pumpEvents(const Sink& sink, uint32_t waitMs, bool stopOnOutput) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    for (;;) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (std::chrono::steady_clock::now() >= deadline) return true;
            Sleep(1);
            continue;
        }
        if (FAILED(hr)) return fail("événement de l'encodeur", hr);
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformNeedInput) {
            ++pendingInputs_;
            if (!stopOnOutput) return true;
        } else if (type == METransformHaveOutput) {
            if (!drainOutput(sink)) return false;
            if (stopOnOutput) return true;
        }
    }
}

bool MediaFoundationEncoder::encode(uint64_t ptsUs, bool forceIdr, const Sink& sink) {
    if (pendingInputs_ == 0 && !pumpEvents(sink, 100, false)) return false;
    if (pendingInputs_ == 0) return true; // encodeur occupé : l'image est sautée, pas la session
    const int slot = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % kSlots;
    if (!convertFrame(slot)) return false;

    IMFSample* sample = inputSamples_[slot].Get();
    // Les attributs de l'image précédente (image clé forcée...) ne doivent pas rester.
    sample->DeleteAllItems();
    sample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * 10);
    sample->SetSampleDuration(10'000'000 / cfg_.fps);

    if (forceIdr && codec_) setCodecValue(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    const HRESULT hr = transform_->ProcessInput(inputStream_, sample, 0);
    if (FAILED(hr)) return fail("entrée de l'encodeur", hr);
    --pendingInputs_;
    return pumpEvents(sink, 30, true);
}

bool MediaFoundationEncoder::setBitrate(uint32_t bitrate) {
    if (!codec_) return false;
    cfg_.bitrate = bitrate;
    const HRESULT hr = setCodecValue(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
    return SUCCEEDED(hr) || fail("changement de débit", hr);
}
