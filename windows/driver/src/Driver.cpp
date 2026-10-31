// Pilote d'affichage indirect : expose l'écran de la tablette comme un moniteur Windows.
// Dérivé du sample Microsoft IndirectDisplay (MS-PL), voir windows/driver/NOTICE.md.
#include "Driver.h"

#include <algorithm>
#include <array>
#include <cstring>

using namespace Screenlink;
using Microsoft::WRL::ComPtr;

namespace {

struct Mode {
    DWORD Width, Height, VSync;
};

// Résolutions courantes de tablettes et téléphones : l'agent choisit la plus proche de l'appareil.
constexpr Mode kModes[] = {
    {2944, 1840, 60}, {2944, 1840, 120}, {2944, 1840, 144},
    {2960, 1848, 60}, {2960, 1848, 120}, {2880, 1800, 60}, {2880, 1800, 120},
    {2800, 1752, 60}, {2800, 1752, 120}, {2560, 1600, 60}, {2560, 1600, 120},
    {2560, 1440, 60}, {2560, 1440, 120}, {2400, 1600, 60}, {2400, 1500, 60},
    {2388, 1668, 60}, {2224, 1668, 60},  {2160, 1620, 60}, {2000, 1200, 60},
    {2000, 1200, 120}, {1920, 1200, 60}, {1920, 1200, 120}, {1920, 1080, 60},
    {1920, 1080, 120}, {1600, 1200, 60}, {1280, 800, 60},  {3200, 1440, 60},
    {3120, 1440, 60},  {2400, 1080, 60}, {2340, 1080, 60},
};
constexpr DWORD kPreferredMode = 0;

// EDID 1.4 : identifiant et numéro de série stables, pour que Windows mémorise
// la position et l'échelle de cet écran d'une session à l'autre.
std::array<BYTE, 128> BuildEdid() {
    std::array<BYTE, 128> e{};
    static const BYTE header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::memcpy(e.data(), header, 8);

    // Fabricant "SXT" sur trois lettres de 5 bits.
    const WORD pnp = (WORD)((('S' - '@') << 10) | (('X' - '@') << 5) | ('T' - '@'));
    e[8] = (BYTE)(pnp >> 8);
    e[9] = (BYTE)pnp;
    e[10] = 0x36; // produit
    e[11] = 0x01;
    e[12] = 0x36; // numéro de série
    e[13] = 0x53;
    e[14] = 0x58;
    e[15] = 0x01;
    e[16] = 1;    // semaine
    e[17] = 36;   // 2026 - 1990
    e[18] = 1;    // EDID 1.4
    e[19] = 4;
    e[20] = 0xA5; // numérique, 8 bits, DisplayPort
    e[21] = 27;   // 27 x 17 cm : 12,7 pouces
    e[22] = 17;
    e[23] = 120;  // gamma 2.2
    e[24] = 0x06; // RGB, sRGB par défaut, mode préféré en DTD

    // Chromaticité sRGB.
    static const BYTE chroma[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};
    std::memcpy(e.data() + 25, chroma, 10);
    for (int i = 38; i < 54; i += 2) {
        e[i] = 0x01;
        e[i + 1] = 0x01;
    }

    // Timing détaillé : 2944x1840 @ 60 Hz, CVT reduced blanking, 353 MHz.
    BYTE* d = e.data() + 54;
    constexpr WORD clock = 35300, hActive = 2944, hBlank = 160, vActive = 1840, vBlank = 56;
    constexpr WORD hFront = 48, hSync = 32, vFront = 3, vSync = 6, hMm = 272, vMm = 170;
    d[0] = (BYTE)clock;
    d[1] = (BYTE)(clock >> 8);
    d[2] = (BYTE)hActive;
    d[3] = (BYTE)hBlank;
    d[4] = (BYTE)(((hActive >> 8) << 4) | (hBlank >> 8));
    d[5] = (BYTE)vActive;
    d[6] = (BYTE)vBlank;
    d[7] = (BYTE)(((vActive >> 8) << 4) | (vBlank >> 8));
    d[8] = (BYTE)hFront;
    d[9] = (BYTE)hSync;
    d[10] = (BYTE)((vFront << 4) | vSync);
    d[11] = 0;
    d[12] = (BYTE)hMm;
    d[13] = (BYTE)vMm;
    d[14] = (BYTE)(((hMm >> 8) << 4) | (vMm >> 8));
    d[17] = 0x1A; // synchro numérique séparée

    // Nom du moniteur.
    BYTE* n = e.data() + 72;
    n[3] = 0xFC;
    static const char name[] = "36Screenlink\n";
    std::memcpy(n + 5, name, 13);

    // Plage de fréquences.
    BYTE* r = e.data() + 90;
    r[3] = 0xFD;
    r[5] = 30;
    r[6] = 144;
    r[7] = 30;
    r[8] = 255;
    r[9] = 90; // 900 MHz
    r[10] = 0x01;
    std::memset(r + 11, 0x20, 7);
    r[11] = 0x0A;

    // Descripteur factice.
    e[111] = 0x10;

    BYTE sum = 0;
    for (int i = 0; i < 127; ++i) sum = (BYTE)(sum + e[i]);
    e[127] = (BYTE)(256 - sum);
    return e;
}

const std::array<BYTE, 128>& Edid() {
    static const auto edid = BuildEdid();
    return edid;
}

void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode, const Mode& m, bool monitorMode) {
    mode.totalSize.cx = mode.activeSize.cx = m.Width;
    mode.totalSize.cy = mode.activeSize.cy = m.Height;
    mode.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
    mode.AdditionalSignalInfo.videoStandard = 255;
    mode.vSyncFreq.Numerator = m.VSync;
    mode.vSyncFreq.Denominator = 1;
    mode.hSyncFreq.Numerator = m.VSync * m.Height;
    mode.hSyncFreq.Denominator = 1;
    mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    mode.pixelRate = (UINT64)m.VSync * m.Width * m.Height;
}

IDDCX_MONITOR_MODE MonitorMode(const Mode& m, IDDCX_MONITOR_MODE_ORIGIN origin) {
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    FillSignalInfo(mode.MonitorVideoSignalInfo, m, true);
    return mode;
}

IDDCX_TARGET_MODE TargetMode(const Mode& m) {
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo, m, false);
    return mode;
}

} // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY DeviceD0Entry;
EVT_IDD_CX_ADAPTER_INIT_FINISHED AdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES AdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION ParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES MonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES MonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN MonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN MonitorUnassignSwapChain;

struct DeviceContextWrapper {
    IndirectDeviceContext* Context;
    void Cleanup() {
        delete Context;
        Context = nullptr;
    }
};

struct MonitorContextWrapper {
    IndirectMonitorContext* Context;
    void Cleanup() {
        delete Context;
        Context = nullptr;
    }
};

WDF_DECLARE_CONTEXT_TYPE(DeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE, _In_ UINT, _In_opt_ LPVOID) {
    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, DeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, &attributes, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS DeviceAdd(WDFDRIVER, PWDFDEVICE_INIT deviceInit) {
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = DeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnp);

    IDD_CX_CLIENT_CONFIG idd;
    IDD_CX_CLIENT_CONFIG_INIT(&idd);
    idd.EvtIddCxAdapterInitFinished = AdapterInitFinished;
    idd.EvtIddCxParseMonitorDescription = ParseMonitorDescription;
    idd.EvtIddCxMonitorGetDefaultDescriptionModes = MonitorGetDefaultModes;
    idd.EvtIddCxMonitorQueryTargetModes = MonitorQueryModes;
    idd.EvtIddCxAdapterCommitModes = AdapterCommitModes;
    idd.EvtIddCxMonitorAssignSwapChain = MonitorAssignSwapChain;
    idd.EvtIddCxMonitorUnassignSwapChain = MonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &idd);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);
    attr.EvtCleanupCallback = [](WDFOBJECT object) {
        if (auto* wrapper = WdfObjectGet_DeviceContextWrapper(object)) wrapper->Cleanup();
    };

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) return status;

    status = IddCxDeviceInitialize(device);
    WdfObjectGet_DeviceContextWrapper(device)->Context = new IndirectDeviceContext(device);
    return status;
}

_Use_decl_annotations_
NTSTATUS DeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    WdfObjectGet_DeviceContextWrapper(device)->Context->InitAdapter();
    return STATUS_SUCCESS;
}

HRESULT Direct3DDevice::Init() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr)) return hr;
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr)) return hr;
    return D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                             D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
}

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent)
    : m_SwapChain(swapChain), m_Device(std::move(device)), m_AvailableBufferEvent(newFrameEvent) {
    m_TerminateEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_Thread = CreateThread(nullptr, 0, RunThread, this, 0, nullptr);
}

SwapChainProcessor::~SwapChainProcessor() {
    SetEvent(m_TerminateEvent);
    if (m_Thread) {
        WaitForSingleObject(m_Thread, INFINITE);
        CloseHandle(m_Thread);
    }
    CloseHandle(m_TerminateEvent);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID argument) {
    static_cast<SwapChainProcessor*>(argument)->Run();
    return 0;
}

void SwapChainProcessor::Run() {
    DWORD task = 0;
    HANDLE taskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &task);
    RunCore();
    // Supprimer la swapchain à la sortie pousse le système à en fournir une nouvelle si besoin.
    WdfObjectDelete((WDFOBJECT)m_SwapChain);
    m_SwapChain = nullptr;
    if (taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
}

void SwapChainProcessor::RunCore() {
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_Device->Device.As(&dxgiDevice))) return;

    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = dxgiDevice.Get();
    if (FAILED(IddCxSwapChainSetDevice(m_SwapChain, &setDevice))) return;

    for (;;) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(m_SwapChain, &buffer);
        if (hr == E_PENDING) {
            HANDLE handles[] = {m_AvailableBufferEvent, m_TerminateEvent};
            const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, 16);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) continue;
            break;
        }
        if (FAILED(hr)) break;

        ComPtr<IDXGIResource> acquired;
        acquired.Attach(buffer.MetaData.pSurface);
        acquired.Reset();
        if (FAILED(IddCxSwapChainFinishedProcessingFrame(m_SwapChain))) break;
    }
}

void IndirectDeviceContext::InitAdapter() {
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"36Screenlink";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"36";
    caps.EndPointDiagnostics.pEndPointModelName = L"Android Display";

    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT init = {};
    init.WdfDevice = m_WdfDevice;
    init.pCaps = &caps;
    init.ObjectAttributes = &attr;

    IDARG_OUT_ADAPTER_INIT out;
    if (NT_SUCCESS(IddCxAdapterInitAsync(&init, &out))) {
        m_Adapter = out.AdapterObject;
        WdfObjectGet_DeviceContextWrapper(out.AdapterObject)->Context = this;
    }
}

void IndirectDeviceContext::CreateMonitor() {
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorContextWrapper);

    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED;
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = (UINT)Edid().size();
    info.MonitorDescription.pData = const_cast<BYTE*>(Edid().data());
    // {3653C4A2-6E0F-4D3B-9A1C-36E5CAEE0001} : conteneur stable, propre à cet écran.
    info.MonitorContainerId = {0x3653C4A2, 0x6E0F, 0x4D3B, {0x9A, 0x1C, 0x36, 0xE5, 0xCA, 0xEE, 0x00, 0x01}};

    IDARG_IN_MONITORCREATE create = {};
    create.ObjectAttributes = &attr;
    create.pMonitorInfo = &info;

    IDARG_OUT_MONITORCREATE out;
    if (NT_SUCCESS(IddCxMonitorCreate(m_Adapter, &create, &out))) {
        WdfObjectGet_MonitorContextWrapper(out.MonitorObject)->Context = new IndirectMonitorContext();
        IDARG_OUT_MONITORARRIVAL arrival;
        IddCxMonitorArrival(out.MonitorObject, &arrival);
    }
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent) {
    m_Processor.reset();
    auto device = std::make_shared<Direct3DDevice>(renderAdapter);
    if (FAILED(device->Init())) {
        // Supprimer la swapchain demande au système d'en proposer une autre.
        WdfObjectDelete(swapChain);
        return;
    }
    m_Processor.reset(new SwapChainProcessor(swapChain, device, newFrameEvent));
}

void IndirectMonitorContext::UnassignSwapChain() {
    m_Processor.reset();
}

_Use_decl_annotations_
NTSTATUS AdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
    if (NT_SUCCESS(args->AdapterInitStatus)) WdfObjectGet_DeviceContextWrapper(adapter)->Context->CreateMonitor();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* in, IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
    constexpr UINT count = ARRAYSIZE(kModes);
    out->MonitorModeBufferOutputCount = count;
    if (in->MonitorModeBufferInputCount < count) return in->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    for (UINT i = 0; i < count; ++i) in->pMonitorModes[i] = MonitorMode(kModes[i], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    out->PreferredMonitorModeIdx = kPreferredMode;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorGetDefaultModes(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
    constexpr UINT count = ARRAYSIZE(kModes);
    out->DefaultMonitorModeBufferOutputCount = count;
    if (in->DefaultMonitorModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (in->DefaultMonitorModeBufferInputCount < count) return STATUS_BUFFER_TOO_SMALL;
    for (UINT i = 0; i < count; ++i) in->pDefaultMonitorModes[i] = MonitorMode(kModes[i], IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    out->PreferredMonitorModeIdx = kPreferredMode;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorQueryModes(IDDCX_MONITOR, const IDARG_IN_QUERYTARGETMODES* in, IDARG_OUT_QUERYTARGETMODES* out) {
    constexpr UINT count = ARRAYSIZE(kModes);
    out->TargetModeBufferOutputCount = count;
    if (in->TargetModeBufferInputCount >= count)
        for (UINT i = 0; i < count; ++i) in->pTargetModes[i] = TargetMode(kModes[i]);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* args) {
    WdfObjectGet_MonitorContextWrapper(monitor)->Context->AssignSwapChain(args->hSwapChain, args->RenderAdapterLuid, args->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
    WdfObjectGet_MonitorContextWrapper(monitor)->Context->UnassignSwapChain();
    return STATUS_SUCCESS;
}
