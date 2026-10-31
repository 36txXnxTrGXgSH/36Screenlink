#pragma once
#include <d3d11.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nvEncodeAPI.h"

struct EncoderConfig {
    uint32_t width = 0, height = 0, fps = 60, bitrate = 60'000'000;
    bool hevc = true;
};

class NvencEncoder {
public:
    using Sink = std::function<bool(const uint8_t* data, size_t n, bool key)>;

    ~NvencEncoder();
    bool open(ID3D11Device* device, ID3D11Texture2D* input, const EncoderConfig& cfg);
    std::vector<uint8_t> sequenceHeaders();
    bool encode(uint64_t ptsUs, bool forceIdr, const Sink& sink);
    // Change le débit sans image clé : aucun à-coup visible pendant l'adaptation.
    bool setBitrate(uint32_t bitrate);
    const std::string& lastError() const { return error_; }

private:
    bool fail(const char* what, NVENCSTATUS st);

    HMODULE lib_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* session_ = nullptr;
    NV_ENC_REGISTERED_PTR registered_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    NV_ENC_INITIALIZE_PARAMS init_{};
    NV_ENC_CONFIG config_{};
    EncoderConfig cfg_;
    std::string error_;
};
