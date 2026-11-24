#include "render/dx11_hook.h"
#include "game/vr_body_rig.h"
#include "game/engine_culling.h"
#include "game/engine_lighting.h"
#include "game/vr_weapons.h"
#include "core/config.h"
#include "core/logging.h"
#include "game/camera_capture.h"
#include "game/head_look.h"
#include "render/swapchain.h"
#include "openxr/xr_session.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <sstream>

namespace jc3vr {
namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateDxgiFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateD3D11DeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

PresentFn g_originalPresent = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
CreateDxgiFactoryFn g_originalCreateDxgiFactory = nullptr;
CreateDxgiFactoryFn g_originalCreateDxgiFactory1 = nullptr;
CreateD3D11DeviceFn g_originalCreateD3D11Device = nullptr;
CreateSwapChainFn g_originalCreateSwapChain = nullptr;
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
std::atomic<std::uint64_t> g_presentCount = 0;
std::atomic<std::uint64_t> g_resizeCount = 0;
std::atomic<bool> g_installed = false;
std::atomic<bool> g_swapchainHooksAttached = false;
std::atomic<bool> g_factoryHookAttached = false;
std::atomic<bool> g_factory2HookAttached = false;
std::once_flag g_deviceLogged;
std::mutex g_hookMutex;
std::mutex g_installMutex;
StereoMode g_stereoMode = StereoMode::MonoDebug;
// Optional render-resolution override (0 = off). JC3's swapchain is created
// at the desktop size (1920x1080, 16:9) while each headset eye is roughly
// square (tangent aspect ~0.93), so the eye image was vertically starved.
UINT g_renderWidth = 0;
UINT g_renderHeight = 0;

bool AttachFactoryHook(IDXGIFactory* factory);
bool AttachFactory2Hook(IDXGIFactory* factory);

void ObserveLiveSwapchain(IDXGISwapChain* swapchain) {
    std::call_once(g_deviceLogged, [swapchain] {
        ID3D11Device* device = nullptr;
        if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device) {
            Log("DX11 Present observed, but IDXGISwapChain::GetDevice(ID3D11Device) failed");
            return;
        }
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        const auto details = DescribeSwapchain(swapchain);
        std::ostringstream stream;
        stream << "DX11 live swapchain acquired: device=" << device << " context=" << context
               << " size=" << details.width << "x" << details.height << " buffers=" << details.bufferCount;
        Log(stream.str());
        TraceRuntime("real JC3 swapchain acquired " + std::to_string(details.width) + "x" + std::to_string(details.height));
#if JC3VR_HAVE_OPENXR_HEADERS
        if (g_stereoMode != StereoMode::ExternalSbs) {
            const auto xr = EnsureOpenXrSession(device);
            Log("OpenXR live-device session: " + xr.detail);
            TraceRuntime("OpenXR live-device session: " + xr.detail);
        } else {
            Log("External SBS diagnostic mode: JC3VR leaves OpenXR composition to the reference viewer");
        }
#endif
        Log(std::string("DX11 camera capture: ") + (InstallCameraCapture(context) ? "UpdateSubresource/Map observer installed" : "observer installation failed"));
        TraceRuntime(std::string("camera capture ") + (InstallCameraCapture(context) ? "installed" : "installation failed"));
        if (context) context->Release();
        device->Release();
    });
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags) {
    thread_local bool reentrant = false;
    if (!reentrant) {
        reentrant = true;
        const auto count = ++g_presentCount;
        ObserveLiveSwapchain(swapchain);
        VrBodyPresentTick();
        EngineCullingPresentTick();
        EngineLightingPresentTick();
        VrWeaponsPresentTick();
#if JC3VR_HAVE_OPENXR_HEADERS
        if (g_stereoMode != StereoMode::ExternalSbs) {
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
                device->GetImmediateContext(&context);
                const auto xrSession = EnsureOpenXrSession(device);
                static std::string lastXrSessionStatus;
                if (xrSession.detail != lastXrSessionStatus) {
                    Log("OpenXR live-device session: " + xrSession.detail);
                    TraceRuntime("OpenXR live-device session: " + xrSession.detail);
                    lastXrSessionStatus = xrSession.detail;
                }
                if (context && xrSession.success) {
                    const auto xrFrame = SubmitMonoOpenXrFrame(swapchain, context);
                    static std::string lastXrStatus;
                    if (xrFrame.detail != lastXrStatus && (xrFrame.detail != "OpenXR frame submitted" || !xrFrame.success)) {
                        Log("OpenXR frame loop: " + xrFrame.detail);
                        lastXrStatus = xrFrame.detail;
                    }
                    const auto headLook = ApplyHeadLook(GetLatestOpenXrHeadPose());
                    static std::string lastHeadLookStatus;
                    if (headLook != lastHeadLookStatus && headLook != "HMD head-look dispatched" && headLook != "HMD head-look awaiting a measurable rotation" && headLook != "HMD head-look waiting for a newer pose") {
                        Log(headLook);
                        lastHeadLookStatus = headLook;
                    }
                }
                if (context) context->Release();
                device->Release();
            }
        }
#endif
        if (count == 1 || count % 600 == 0) {
            Log("DX11 Present observed: frame=" + std::to_string(count));
            Log(DescribeCameraCapture());
        }
        reentrant = false;
    }
    return g_originalPresent(swapchain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* swapchain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    const auto calls = ++g_resizeCount;
    Log("DX11 ResizeBuffers observed: call=" + std::to_string(calls) + " requested=" + std::to_string(width) + "x" + std::to_string(height) +
        (g_renderWidth && g_renderHeight ? " overridden=" + std::to_string(g_renderWidth) + "x" + std::to_string(g_renderHeight) : std::string()));
    if (g_renderWidth && g_renderHeight) {
        width = g_renderWidth;
        height = g_renderHeight;
    }
    return g_originalResizeBuffers(swapchain, count, width, height, format, flags);
}

bool AttachSwapchainHooks(IDXGISwapChain* swapchain) {
    if (!swapchain || g_swapchainHooksAttached) return g_swapchainHooksAttached;
    std::lock_guard lock(g_hookMutex);
    if (g_swapchainHooksAttached) return true;
    void** vtable = *reinterpret_cast<void***>(swapchain);
    const MH_STATUS presentStatus = MH_CreateHook(vtable[8], &PresentDetour, reinterpret_cast<void**>(&g_originalPresent));
    const MH_STATUS resizeStatus = MH_CreateHook(vtable[13], &ResizeBuffersDetour, reinterpret_cast<void**>(&g_originalResizeBuffers));
    if ((presentStatus != MH_OK && presentStatus != MH_ERROR_ALREADY_CREATED) ||
        (resizeStatus != MH_OK && resizeStatus != MH_ERROR_ALREADY_CREATED) ||
        MH_EnableHook(vtable[8]) != MH_OK || MH_EnableHook(vtable[13]) != MH_OK) return false;
    g_swapchainHooksAttached = true;
    return true;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainDetour(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapchain) {
    DXGI_SWAP_CHAIN_DESC overridden{};
    if (description && g_renderWidth && g_renderHeight) {
        overridden = *description;
        Log("DX11 render resolution override: CreateSwapChain " + std::to_string(overridden.BufferDesc.Width) + "x" +
            std::to_string(overridden.BufferDesc.Height) + " -> " + std::to_string(g_renderWidth) + "x" + std::to_string(g_renderHeight));
        overridden.BufferDesc.Width = g_renderWidth;
        overridden.BufferDesc.Height = g_renderHeight;
        description = &overridden;
    }
    const HRESULT result = g_originalCreateSwapChain(factory, device, description, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain) {
        Log(std::string("DX11 Geo-11 bridge: game IDXGIFactory::CreateSwapChain observed; Present attachment ") + (AttachSwapchainHooks(*swapchain) ? "installed" : "failed"));
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndDetour(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* description, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDescription, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapchain) {
    DXGI_SWAP_CHAIN_DESC1 overridden{};
    if (description && g_renderWidth && g_renderHeight) {
        overridden = *description;
        Log("DX11 render resolution override: CreateSwapChainForHwnd " + std::to_string(overridden.Width) + "x" +
            std::to_string(overridden.Height) + " -> " + std::to_string(g_renderWidth) + "x" + std::to_string(g_renderHeight));
        overridden.Width = g_renderWidth;
        overridden.Height = g_renderHeight;
        description = &overridden;
    }
    const HRESULT result = g_originalCreateSwapChainForHwnd(factory, device, window, description, fullscreenDescription, restrictToOutput, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain) {
        Log(std::string("DX11 Geo-11 bridge: game IDXGIFactory2::CreateSwapChainForHwnd observed; Present attachment ") + (AttachSwapchainHooks(*swapchain) ? "installed" : "failed"));
    }
    return result;
}

bool AttachFactoryHook(IDXGIFactory* factory) {
    if (!factory || g_factoryHookAttached) return g_factoryHookAttached;
    std::lock_guard lock(g_hookMutex);
    if (g_factoryHookAttached) return true;
    void* target = (*reinterpret_cast<void***>(factory))[10]; // IDXGIFactory::CreateSwapChain.
    const MH_STATUS status = MH_CreateHook(target, &CreateSwapChainDetour, reinterpret_cast<void**>(&g_originalCreateSwapChain));
    if ((status != MH_OK && status != MH_ERROR_ALREADY_CREATED) || MH_EnableHook(target) != MH_OK) return false;
    g_factoryHookAttached = true;
    return true;
}

bool AttachFactory2Hook(IDXGIFactory* factory) {
    if (!factory || g_factory2HookAttached) return g_factory2HookAttached;
    IDXGIFactory2* factory2 = nullptr;
    if (FAILED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))) || !factory2) return false;
    std::lock_guard lock(g_hookMutex);
    if (g_factory2HookAttached) { factory2->Release(); return true; }
    void* target = (*reinterpret_cast<void***>(factory2))[15]; // IDXGIFactory2::CreateSwapChainForHwnd.
    const MH_STATUS status = MH_CreateHook(target, &CreateSwapChainForHwndDetour, reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd));
    const bool attached = (status == MH_OK || status == MH_ERROR_ALREADY_CREATED) && MH_EnableHook(target) == MH_OK;
    if (attached) g_factory2HookAttached = true;
    factory2->Release();
    return attached;
}

HRESULT WINAPI CreateDxgiFactoryDetour(REFIID riid, void** factory) {
    const HRESULT result = g_originalCreateDxgiFactory(riid, factory);
    if (SUCCEEDED(result) && factory && *factory) {
        auto* resultFactory = static_cast<IDXGIFactory*>(*factory);
        Log(std::string("DX11 Geo-11 bridge: CreateDXGIFactory observed; swapchain attachments ") + (AttachFactoryHook(resultFactory) && AttachFactory2Hook(resultFactory) ? "installed" : "partial"));
    }
    return result;
}

HRESULT WINAPI CreateDxgiFactory1Detour(REFIID riid, void** factory) {
    const HRESULT result = g_originalCreateDxgiFactory1(riid, factory);
    if (SUCCEEDED(result) && factory && *factory) {
        auto* resultFactory = static_cast<IDXGIFactory*>(*factory);
        Log(std::string("DX11 Geo-11 bridge: CreateDXGIFactory1 observed; swapchain attachments ") + (AttachFactoryHook(resultFactory) && AttachFactory2Hook(resultFactory) ? "installed" : "partial"));
    }
    return result;
}

HRESULT WINAPI CreateD3D11DeviceDetour(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context) {
    const HRESULT result = g_originalCreateD3D11Device(adapter, driverType, software, flags, featureLevels, featureLevelCount, sdkVersion, device, featureLevel, context);
    if (FAILED(result) || !device || !*device) return result;
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* dxgiAdapter = nullptr;
    IDXGIFactory* factory = nullptr;
    const bool attached = SUCCEEDED((*device)->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice &&
                          SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) && dxgiAdapter &&
                          SUCCEEDED(dxgiAdapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) && factory &&
                          AttachFactoryHook(factory) && AttachFactory2Hook(factory);
    if (factory) factory->Release();
    if (dxgiAdapter) dxgiAdapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    Log(std::string("DX11 Geo-11 bridge: game D3D11CreateDevice observed; device-owned factory attachments ") + (attached ? "installed" : "partial"));
    return result;
}

bool MakeDummySwapchain(IDXGISwapChain** swapchain) {
    const wchar_t* className = L"JC3VR_DummySwapchainWindow";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, className, L"JC3VR", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) return false;
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 16;
    description.BufferDesc.Height = 16;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &description, swapchain, &device, nullptr, &context);
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(window);
    return SUCCEEDED(result) && *swapchain;
}
}

bool InstallDx11Hooks() {
    std::lock_guard installLock(g_installMutex);
    if (g_installed) return true;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { TraceRuntime("DX11 hook install failed: MinHook initialization error " + std::to_string(init)); return false; }
    if (g_stereoMode == StereoMode::Geo11 || g_stereoMode == StereoMode::NativeAlternate || g_stereoMode == StereoMode::NativeFull) {
        const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        const HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        const auto createFactory = dxgi ? GetProcAddress(dxgi, "CreateDXGIFactory") : nullptr;
        const auto createFactory1 = dxgi ? GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
        const auto createDevice = d3d11 ? GetProcAddress(d3d11, "D3D11CreateDevice") : nullptr;
        if (!createFactory || !createFactory1 || !createDevice) {
            TraceRuntime("DX11 creation bridge: exports unavailable dxgi="+std::to_string(dxgi!=nullptr)+" d3d11="+std::to_string(d3d11!=nullptr));
            return false;
        }
        void* targets[]{createFactory,createFactory1,createDevice};
        void* detours[]{reinterpret_cast<void*>(&CreateDxgiFactoryDetour),reinterpret_cast<void*>(&CreateDxgiFactory1Detour),reinterpret_cast<void*>(&CreateD3D11DeviceDetour)};
        void** originals[]{reinterpret_cast<void**>(&g_originalCreateDxgiFactory),reinterpret_cast<void**>(&g_originalCreateDxgiFactory1),reinterpret_cast<void**>(&g_originalCreateD3D11Device)};
        for(int i=0;i<3;++i) {
            const auto create=MH_CreateHook(targets[i],detours[i],originals[i]);
            // Accept our partially created hooks on retry, never an unknown
            // hook for which we have no callable original trampoline.
            if(create!=MH_OK && !(create==MH_ERROR_ALREADY_CREATED && *originals[i])) {
                TraceRuntime("DX11 creation bridge: create index="+std::to_string(i)+" status="+std::to_string(create));return false;
            }
            const auto enable=MH_EnableHook(targets[i]);
            if(enable!=MH_OK && enable!=MH_ERROR_ENABLED) {
                TraceRuntime("DX11 creation bridge: enable index="+std::to_string(i)+" status="+std::to_string(enable));return false;
            }
        }
        g_installed = true;
        TraceRuntime("DX11 creation bridge armed before gameplay hooks; waiting for real swapchain");
        Log("DX11 real-swapchain bridge armed: waiting for the game DXGI factory/swapchain (no dummy device created)");
        return true;
    }
    IDXGISwapChain* dummy = nullptr;
    if (!MakeDummySwapchain(&dummy)) { Log("DX11 hook install failed: unable to create dummy D3D11 swapchain"); return false; }
    const bool attached = AttachSwapchainHooks(dummy);
    dummy->Release();
    if (!attached) {
        Log("DX11 hook install failed: unable to attach Present and ResizeBuffers hooks");
        return false;
    }
    g_installed = true;
    Log("DX11 hooks installed: IDXGISwapChain::Present and ResizeBuffers");
    return true;
}

bool ArmNativePresentFallback() {
    if(g_stereoMode!=StereoMode::NativeAlternate && g_stereoMode!=StereoMode::NativeFull) return true;
    if(g_swapchainHooksAttached.load()) return true;
    std::lock_guard installLock(g_installMutex);
    if(g_swapchainHooksAttached.load()) return true;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    TraceRuntime("DX11 native Present fallback: creation not observed; discovering system DXGI vtable (no XR/dummy Present)");
    IDXGISwapChain* dummy=nullptr;
    if(!MakeDummySwapchain(&dummy)) {
        TraceRuntime("DX11 native Present fallback: temporary device creation failed last_error="+std::to_string(GetLastError()));return false;
    }
    const bool attached=AttachSwapchainHooks(dummy);
    dummy->Release();
    if(attached) g_installed=true;
    TraceRuntime(std::string("DX11 native Present fallback: ")+(attached?"Present/ResizeBuffers attached":"attachment failed"));
    return attached;
}

Dx11HookStatus GetDx11HookStatus() { return {g_installed, g_presentCount, g_resizeCount}; }
void ConfigureDx11StereoMode(StereoMode mode) { g_stereoMode = mode; }
void ConfigureRenderResolution(unsigned width, unsigned height) {
    g_renderWidth = width >= 640 && height >= 480 && width <= 8192 && height <= 8192 ? width : 0;
    g_renderHeight = g_renderWidth ? height : 0;
    Log("DX11 render resolution override: " + (g_renderWidth ? std::to_string(g_renderWidth) + "x" + std::to_string(g_renderHeight) : std::string("off")));
}
}
