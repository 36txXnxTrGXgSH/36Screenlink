#include "encoder_nvenc.h"

namespace {
using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
}

bool NvencEncoder::fail(const char* what, NVENCSTATUS st) {
    error_ = std::string(what) + " : NVENCSTATUS " + std::to_string(static_cast<int>(st));
    return false;
}

NvencEncoder::~NvencEncoder() {
    if (session_) {
        NV_ENC_PIC_PARAMS eos{};
        eos.version = NV_ENC_PIC_PARAMS_VER;
        eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        api_.nvEncEncodePicture(session_, &eos);
        if (bitstream_) api_.nvEncDestroyBitstreamBuffer(session_, bitstream_);
        if (registered_) api_.nvEncUnregisterResource(session_, registered_);
        api_.nvEncDestroyEncoder(session_);
    }
    if (lib_) FreeLibrary(lib_);
}

bool NvencEncoder::open(ID3D11Device* device, ID3D11Texture2D* input, EncoderConfig& cfg) {
    cfg_ = cfg;
    // System32 uniquement : une DLL du même nom déposée à côté de l'exe serait sinon chargée.
    lib_ = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib_) {
        error_ = "nvEncodeAPI64.dll introuvable : pilote NVIDIA requis";
        return false;
    }
    auto create = reinterpret_cast<CreateInstanceFn>(GetProcAddress(lib_, "NvEncodeAPICreateInstance"));
    if (!create) {
        error_ = "NvEncodeAPICreateInstance absent";
        return false;
    }
    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = create(&api_);
    if (st != NV_ENC_SUCCESS) return fail("création de l'API", st);

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams{};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device = device;
    sessionParams.apiVersion = NVENCAPI_VERSION;
    st = api_.nvEncOpenEncodeSessionEx(&sessionParams, &session_);
    if (st != NV_ENC_SUCCESS) return fail("ouverture de session", st);

    const GUID codec = cfg.hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    st = api_.nvEncGetEncodePresetConfigEx(session_, codec, NV_ENC_PRESET_P1_GUID,
                                           NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    if (st != NV_ENC_SUCCESS) return fail("preset", st);

    NV_ENC_CONFIG config = preset.presetCfg;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;
    config.frameIntervalP = 1;
    auto& rc = config.rcParams;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.averageBitRate = cfg.bitrate;
    rc.maxBitRate = cfg.bitrate;
    // Une marge de deux images laisse passer un défilement de texte sans perte de qualité brutale.
    rc.vbvBufferSize = cfg.bitrate / cfg.fps * 2;
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.enableAQ = 1;

    if (cfg.hevc) {
        auto& h = config.encodeCodecConfig.hevcConfig;
        h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h.repeatSPSPPS = 1;
        auto& vui = h.hevcVUIParameters;
        vui.videoSignalTypePresentFlag = 1;
        vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        vui.colourDescriptionPresentFlag = 1;
        vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    } else {
        auto& h = config.encodeCodecConfig.h264Config;
        h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h.repeatSPSPPS = 1;
        auto& vui = h.h264VUIParameters;
        vui.videoSignalTypePresentFlag = 1;
        vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        vui.colourDescriptionPresentFlag = 1;
        vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    }

    config_ = config;
    NV_ENC_INITIALIZE_PARAMS& init = init_;
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = codec;
    init.presetGUID = NV_ENC_PRESET_P1_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = init.darWidth = cfg.width;
    init.encodeHeight = init.darHeight = cfg.height;
    init.frameRateNum = cfg.fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig = &config_;
    st = api_.nvEncInitializeEncoder(session_, &init);
    if (st != NV_ENC_SUCCESS) return fail("initialisation", st);

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = cfg.width;
    reg.height = cfg.height;
    reg.resourceToRegister = input;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    st = api_.nvEncRegisterResource(session_, &reg);
    if (st != NV_ENC_SUCCESS) return fail("enregistrement de la texture", st);
    registered_ = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER out{};
    out.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = api_.nvEncCreateBitstreamBuffer(session_, &out);
    if (st != NV_ENC_SUCCESS) return fail("tampon de sortie", st);
    bitstream_ = out.bitstreamBuffer;
    return true;
}

bool NvencEncoder::setBitrate(uint32_t bitrate) {
    auto& rc = config_.rcParams;
    rc.averageBitRate = bitrate;
    rc.maxBitRate = bitrate;
    rc.vbvBufferSize = bitrate / cfg_.fps * 2;
    rc.vbvInitialDelay = rc.vbvBufferSize;

    NV_ENC_RECONFIGURE_PARAMS params{};
    params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    params.reInitEncodeParams = init_;
    const NVENCSTATUS st = api_.nvEncReconfigureEncoder(session_, &params);
    return st == NV_ENC_SUCCESS || fail("changement de débit", st);
}

std::vector<uint8_t> NvencEncoder::sequenceHeaders() {
    std::vector<uint8_t> buf(4096);
    uint32_t size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = static_cast<uint32_t>(buf.size());
    payload.spsppsBuffer = buf.data();
    payload.outSPSPPSPayloadSize = &size;
    if (api_.nvEncGetSequenceParams(session_, &payload) != NV_ENC_SUCCESS) size = 0;
    buf.resize(size);
    return buf;
}

bool NvencEncoder::encode(uint64_t ptsUs, bool forceIdr, const Sink& sink) {
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered_;
    NVENCSTATUS st = api_.nvEncMapInputResource(session_, &map);
    if (st != NV_ENC_SUCCESS) return fail("map", st);

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = cfg_.width;
    pic.inputHeight = cfg_.height;
    pic.inputPitch = 0;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.outputBitstream = bitstream_;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = ptsUs;
    if (forceIdr) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    st = api_.nvEncEncodePicture(session_, &pic);
    bool ok = st == NV_ENC_SUCCESS;
    if (!ok) fail("encodage", st);

    if (ok) {
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = bitstream_;
        st = api_.nvEncLockBitstream(session_, &lock);
        ok = st == NV_ENC_SUCCESS;
        if (ok) {
            const bool key = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
            ok = sink(static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes, key);
            api_.nvEncUnlockBitstream(session_, bitstream_);
            if (!ok) error_ = "envoi interrompu";
        } else {
            fail("lock", st);
        }
    }
    api_.nvEncUnmapInputResource(session_, map.mappedResource);
    return ok;
}
