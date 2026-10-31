#pragma once
#include <windows.h>

#include "encoder.h"
#include "nvEncodeAPI.h"

class NvencEncoder : public Encoder {
public:
    ~NvencEncoder() override;
    bool open(ID3D11Device* device, ID3D11Texture2D* input, EncoderConfig& cfg) override;
    std::vector<uint8_t> sequenceHeaders() override;
    bool encode(uint64_t ptsUs, bool forceIdr, const Sink& sink) override;
    bool setBitrate(uint32_t bitrate) override;

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
};
