#include "audio.h"

#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "store.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr REFERENCE_TIME kBufferDuration = 2'000'000; // 200 ms
constexpr DWORD kPollMs = 5;

std::wstring muteMarker() { return dataFilePath(L"audio-muted"); }

ComPtr<IMMDevice> defaultRender() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
    return device;
}

bool isFloat(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE) return false;
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

int16_t toPcm16(float v) { return static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }

} // namespace

void AudioStreamer::recoverMute() {
    if (!std::filesystem::exists(muteMarker())) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IAudioEndpointVolume> volume;
        if (auto device = defaultRender()) device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
        if (volume) volume->SetMute(FALSE, nullptr);
    }
    CoUninitialize();
    std::error_code ignored;
    std::filesystem::remove(muteMarker(), ignored);
}

void AudioStreamer::start(Sink sink) {
    stop();
    running_ = true;
    thread_ = std::thread([this, sink = std::move(sink)] { run(sink); });
}

void AudioStreamer::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioStreamer::run(const Sink& sink) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Boucle : changer de sortie par défaut invalide la capture, qui repart sur la nouvelle.
    while (running_) {
        captureUntilStopped(sink);
        for (int i = 0; i < 20 && running_; ++i) Sleep(50);
    }
    CoUninitialize();
}

void AudioStreamer::captureUntilStopped(const Sink& sink) {
    auto device = defaultRender();
    if (!device) return;

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return;
    WAVEFORMATEX* format = nullptr;
    if (FAILED(client->GetMixFormat(&format))) return;
    const bool floating = isFloat(format);
    const UINT channels = format->nChannels, bits = format->wBitsPerSample, rate = format->nSamplesPerSec;
    const HRESULT init = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferDuration, 0, format, nullptr);
    CoTaskMemFree(format);
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(init) || FAILED(client->GetService(IID_PPV_ARGS(&capture))) || (!floating && bits != 16)) {
        std::printf("Capture du son impossible : 0x%08lx\n", static_cast<unsigned long>(init));
        return;
    }

    ComPtr<IAudioEndpointVolume> volume;
    device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
    BOOL wasMuted = FALSE;
    if (volume) {
        volume->GetMute(&wasMuted);
        // Repère posé avant de couper : il survit à un arrêt brutal.
        if (!wasMuted) std::ofstream(muteMarker()) << 1;
        volume->SetMute(TRUE, nullptr);
    }

    std::vector<int16_t> stereo;
    client->Start();
    bool healthy = true;
    while (running_ && healthy) {
        Sleep(kPollMs);
        UINT32 packet = 0;
        while (running_ && healthy && SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                healthy = false;
                break;
            }
            stereo.assign(static_cast<size_t>(frames) * 2, 0);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const UINT right = channels > 1 ? 1 : 0;
                for (UINT32 i = 0; i < frames; ++i) {
                    if (floating) {
                        const float* f = reinterpret_cast<const float*>(data) + static_cast<size_t>(i) * channels;
                        stereo[i * 2u] = toPcm16(f[0]);
                        stereo[i * 2u + 1] = toPcm16(f[right]);
                    } else {
                        const int16_t* s = reinterpret_cast<const int16_t*>(data) + static_cast<size_t>(i) * channels;
                        stereo[i * 2u] = s[0];
                        stereo[i * 2u + 1] = s[right];
                    }
                }
            }
            capture->ReleaseBuffer(frames);
            if (!sink(rate, stereo.data(), frames)) healthy = false;
        }
        if (FAILED(capture->GetNextPacketSize(&packet))) healthy = false;
    }
    client->Stop();
    if (volume) volume->SetMute(wasMuted, nullptr);
    std::error_code ignored;
    std::filesystem::remove(muteMarker(), ignored);
}
