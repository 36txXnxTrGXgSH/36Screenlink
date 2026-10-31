#pragma once
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct EncoderConfig {
    uint32_t width = 0, height = 0, fps = 60, bitrate = 60'000'000;
    bool hevc = true;
};

class Encoder {
public:
    using Sink = std::function<bool(const uint8_t* data, size_t n, bool key)>;

    virtual ~Encoder() = default;
    // `input` est la texture BGRA que la capture remplit, sur le même périphérique.
    virtual bool open(ID3D11Device* device, ID3D11Texture2D* input, EncoderConfig& cfg) = 0;
    virtual std::vector<uint8_t> sequenceHeaders() = 0;
    virtual bool encode(uint64_t ptsUs, bool forceIdr, const Sink& sink) = 0;
    // Change le débit sans image clé : aucun à-coup visible pendant l'adaptation.
    virtual bool setBitrate(uint32_t bitrate) = 0;
    const std::string& lastError() const { return error_; }

protected:
    std::string error_;
};
