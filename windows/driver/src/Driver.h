// Dérivé du sample Microsoft IndirectDisplay (MS-PL), voir windows/driver/NOTICE.md.
#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>

namespace Screenlink {

struct Direct3DDevice {
    explicit Direct3DDevice(LUID adapterLuid) : AdapterLuid(adapterLuid) {}
    HRESULT Init();

    LUID AdapterLuid;
    Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
};

// Vide la swapchain : sans consommateur, Windows cesserait de composer ce moniteur.
// L'image est lue par l'agent via Desktop Duplication.
class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent);
    ~SwapChainProcessor();

private:
    static DWORD CALLBACK RunThread(LPVOID argument);
    void Run();
    void RunCore();

    IDDCX_SWAPCHAIN m_SwapChain;
    std::shared_ptr<Direct3DDevice> m_Device;
    HANDLE m_AvailableBufferEvent;
    HANDLE m_Thread = nullptr;
    HANDLE m_TerminateEvent = nullptr;
};

class IndirectDeviceContext {
public:
    explicit IndirectDeviceContext(WDFDEVICE device) : m_WdfDevice(device) {}

    void InitAdapter();
    void CreateMonitor();

private:
    WDFDEVICE m_WdfDevice;
    IDDCX_ADAPTER m_Adapter = nullptr;
};

class IndirectMonitorContext {
public:
    void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
    void UnassignSwapChain();

private:
    std::unique_ptr<SwapChainProcessor> m_Processor;
};

} // namespace Screenlink
