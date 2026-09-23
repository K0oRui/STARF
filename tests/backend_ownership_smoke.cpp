// Exercise the real DLL hooks with two different APIs in the same process.
#ifdef NDEBUG
#undef NDEBUG
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <d3d11.h>
#include <d3d10.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <d3d9.h>
#include <GL/gl.h>
#include <wrl/client.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include "../third_party/unity/IUnityGraphics.h"
#include "../third_party/unity/IUnityGraphicsD3D12.h"
#include "../third_party/unity/IUnityGraphicsVulkan.h"
using Microsoft::WRL::ComPtr;

// DX7/DX8 setup lives in backend_ownership_dx7.cpp / backend_ownership_dx8.cpp:
// d3d8.h clashes with d3d9.h (main TU) and with d3dtypes.h from d3d.h (dx7 TU),
// so each D3D generation gets its own translation unit, like the backends.
bool legacy_create_dx8(HWND window, void** device, void** params);
void legacy_present_dx8(void* device);
bool legacy_reset_dx8(void* device, void* params);
void legacy_release_dx8(void* device, void* params);
bool legacy_create_dx7(HWND window, void** device);
void legacy_end_scene_dx7(void* device);
void legacy_release_dx7(void* device);

// Model a Unity plugin loaded after the real native graphics objects exist.
static IDXGISwapChain* unity_chain;
static ID3D12CommandQueue* unity_queue;
static UnityVulkanInstance unity_vulkan{};
static IDXGISwapChain* UNITY_INTERFACE_API get_unity_chain() { return unity_chain; }
static ID3D12CommandQueue* UNITY_INTERFACE_API get_unity_queue() { return unity_queue; }
static UnityVulkanInstance UNITY_INTERFACE_API get_unity_vulkan() { return unity_vulkan; }
static IUnityInterface* UNITY_INTERFACE_API get_unity_interface(UnityInterfaceGUID guid)
{
    static IUnityGraphicsD3D12v7 dx12{};
    static IUnityGraphicsVulkan vulkan{};
    dx12.GetSwapChain = get_unity_chain;
    dx12.GetCommandQueue = get_unity_queue;
    vulkan.Instance = get_unity_vulkan;
    if (guid == GetUnityInterfaceGUID<IUnityGraphicsD3D12v7>()) return &dx12;
    if (guid == GetUnityInterfaceGUID<IUnityGraphicsVulkan>()) return &vulkan;
    return nullptr;
}
// The SDK declares these exports; the test invokes the DLL's versions below.
extern "C" void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces*) {}
extern "C" void UNITY_INTERFACE_API UnityPluginUnload() {}

static LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

int main(int argc, char** argv)
{
    assert(argc == 4);
    const bool vulkan_helper = std::string(argv[2]) == "dx11_vulkan";
    const bool vulkan_proc = std::string(argv[2]) == "vulkan_proc";
    const bool late = std::string(argv[2]) == "dx12_late" || std::string(argv[2]) == "vulkan_late";
    const std::string api = vulkan_helper ? "dx11" : vulkan_proc ? "vulkan" :
        late ? std::string(argv[2]).substr(0, std::string(argv[2]).find('_')) : argv[2];
    auto dir = std::filesystem::absolute(argv[3]) / (api + "-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(dir / "STAR");
    std::ofstream(dir / "STAR" / "overlay.star") << "enabled=true\nmode=hook\nshow_fps=true\n";
    std::ofstream(dir / "STAR" / "steam_appid.txt") << "480\n";
    SetEnvironmentVariableW(L"APPDATA", dir.c_str());
    auto dll = dir / std::filesystem::path(argv[1]).filename();
    std::filesystem::copy_file(argv[1], dll);

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = test_window_proc;
    wc.lpszClassName = L"STAR_Backend_Test";
    wc.style = CS_OWNDC;
    assert(RegisterClassW(&wc));
    // Visible to API detection (IsWindowVisible) but never activated, so the
    // tests can't steal foreground focus. Offscreen position alone does not
    // prevent activation: WS_VISIBLE activates on creation.
    HWND main = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Game", WS_POPUP,
        -30000, -30000, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    HWND helper = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Helper", WS_POPUP,
        -29000, -29000, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    assert(main && helper);
    ShowWindow(main, SW_SHOWNOACTIVATE);
    ShowWindow(helper, SW_SHOWNOACTIVATE);

    void (*shutdown)() = nullptr;
    // Legacy backends latch at init: hook_dx7 only while ddraw is loaded.
    if (api == "dx7") LoadLibraryW(L"ddraw.dll");
    if (api == "dx8") LoadLibraryW(L"d3d8.dll");
    auto load_overlay = [&] {
        HMODULE module = LoadLibraryW(dll.c_str());
        assert(module);
        auto init = (bool(*)())GetProcAddress(module, "SteamAPI_Init");
        shutdown = (void(*)())GetProcAddress(module, "SteamAPI_Shutdown");
        assert(init && shutdown);
        if (late) {
            static IUnityInterfaces interfaces{};
            interfaces.GetInterface = get_unity_interface;
            auto plugin_load = (void(UNITY_INTERFACE_API*)(IUnityInterfaces*))GetProcAddress(module, "UnityPluginLoad");
            assert(plugin_load);
            plugin_load(&interfaces);
        }
        std::thread competing_init([&] { assert(init()); });
        assert(init());
        competing_init.join();
    };
    if (!late) load_overlay();

    // Skips must still tear down: exiting with overlay threads, a current GL
    // context, or GPU objects live crashes in driver teardown (atio6axx fastfail).
    HWND gl_window = api == "opengl" ? main : helper;
    HDC dc = nullptr;
    HGLRC gl = nullptr;
    auto skip = [&]() -> int {
        if (gl) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(gl); gl = nullptr; }
        if (dc) { ReleaseDC(gl_window, dc); dc = nullptr; }
        if (shutdown) shutdown();
        return 77;
    };
    dc = GetDC(gl_window);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    int format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd)) return skip();
    gl = wglCreateContext(dc);
    if (!gl || !wglMakeCurrent(dc, gl)) return skip();
    auto swap = (BOOL(WINAPI*)(HDC))GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglSwapBuffers");
    assert(swap);

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1; desc.BufferDesc.Width = 100; desc.BufferDesc.Height = 100;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SampleDesc.Count = 1; desc.Windowed = TRUE;
    desc.OutputWindow = api == "dx11" ? main : helper;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &desc, &chain, &device, nullptr, nullptr))) return skip();
    ComPtr<IDirect3D9> d3d9;
    ComPtr<IDirect3DDevice9> device9;
    D3DPRESENT_PARAMETERS pp{};
    if (api == "dx9") {
        d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = main; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
        if (!d3d9 || FAILED(d3d9->CreateDevice(0, D3DDEVTYPE_HAL, main,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device9))) return skip();
    }
    void* device8 = nullptr;
    void* params8 = nullptr;
    if (api == "dx8" && !legacy_create_dx8(main, &device8, &params8)) return skip();
    void* device7 = nullptr;
    if (api == "dx7" && !legacy_create_dx7(main, &device7)) return skip();
    ComPtr<IDXGISwapChain> main_chain;
    ComPtr<ID3D10Device> device10;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12, replacement_queue;
    if (api == "dx10") {
        desc.OutputWindow = main;
        if (FAILED(D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr,
            0, D3D10_SDK_VERSION, &desc, &main_chain, &device10))) return skip();
    }
#ifdef _WIN64
    if (api == "dx12") {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)))) return skip();
        D3D12_COMMAND_QUEUE_DESC qd{};
        assert(SUCCEEDED(device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&replacement_queue))));
        assert(SUCCEEDED(device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue12))));
        ComPtr<IDXGIFactory2> factory;
        assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        desc.OutputWindow = main;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        assert(SUCCEEDED(factory->CreateSwapChain(queue12.Get(), &desc, &main_chain)));
    }
#endif
    HDC main_dc = GetDC(main);
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = 640; bitmap.bmiHeader.biHeight = -480;
    bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 32;
    std::vector<unsigned> pixels(640 * 480);
    HMODULE vulkan = (api == "vulkan" || vulkan_helper) ? LoadLibraryW(L"vulkan-1.dll") : nullptr;
#define VK_LOAD(name) auto name = vulkan ? (PFN_##name)GetProcAddress(vulkan, #name) : nullptr
    VK_LOAD(vkCreateInstance); VK_LOAD(vkEnumeratePhysicalDevices); VK_LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_LOAD(vkCreateWin32SurfaceKHR); VK_LOAD(vkGetPhysicalDeviceSurfaceSupportKHR);
    VK_LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); VK_LOAD(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VK_LOAD(vkCreateDevice); VK_LOAD(vkGetDeviceQueue); VK_LOAD(vkCreateSwapchainKHR);
    VK_LOAD(vkGetSwapchainImagesKHR); VK_LOAD(vkCreateCommandPool); VK_LOAD(vkAllocateCommandBuffers);
    VK_LOAD(vkCreateSemaphore); VK_LOAD(vkAcquireNextImageKHR); VK_LOAD(vkResetCommandBuffer);
    VK_LOAD(vkBeginCommandBuffer); VK_LOAD(vkCmdPipelineBarrier); VK_LOAD(vkEndCommandBuffer);
    VK_LOAD(vkQueueSubmit); VK_LOAD(vkQueuePresentKHR); VK_LOAD(vkDeviceWaitIdle);
    VK_LOAD(vkDestroySwapchainKHR); VK_LOAD(vkDestroyDevice);
    VK_LOAD(vkDestroyCommandPool); VK_LOAD(vkDestroySemaphore); VK_LOAD(vkDestroySurfaceKHR); VK_LOAD(vkDestroyInstance);
#undef VK_LOAD
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkSurfaceKHR surface{};
    VkDevice vk_device{};
    VkQueue vk_queue{};
    VkSwapchainKHR vk_chain{};
    VkCommandPool pool{};
    VkCommandBuffer commands{};
    VkSemaphore acquired{}, ready{};
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    std::vector<VkImage> images;
    auto read_images = [&] {
        uint32_t count = 0;
        assert(vkGetSwapchainImagesKHR(vk_device, vk_chain, &count, nullptr) == VK_SUCCESS);
        images.resize(count);
        assert(vkGetSwapchainImagesKHR(vk_device, vk_chain, &count, images.data()) == VK_SUCCESS);
    };
    if (api == "vulkan" || vulkan_helper) {
        if (!vulkan || !vkCreateInstance) return skip();
        const char* extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = extensions;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return skip();
        auto get_instance_proc = (PFN_vkGetInstanceProcAddr)GetProcAddress(vulkan, "vkGetInstanceProcAddr");
        if (vulkan_proc) {
#define VK_INSTANCE(name) name = (PFN_##name)get_instance_proc(instance, #name); assert(name);
            VK_INSTANCE(vkEnumeratePhysicalDevices)
            VK_INSTANCE(vkCreateWin32SurfaceKHR)
            VK_INSTANCE(vkCreateDevice)
            VK_INSTANCE(vkDestroySurfaceKHR)
#undef VK_INSTANCE
        }
        uint32_t count = 0;
        assert(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS);
        if (!count) return skip();
        std::vector<VkPhysicalDevice> physicals(count);
        assert(vkEnumeratePhysicalDevices(instance, &count, physicals.data()) == VK_SUCCESS);
        physical = physicals[0];
        VkWin32SurfaceCreateInfoKHR wci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        wci.hinstance = wc.hInstance; wci.hwnd = vulkan_helper ? helper : main;
        assert(vkCreateWin32SurfaceKHR(instance, &wci, nullptr, &surface) == VK_SUCCESS);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i) {
            VkBool32 supports = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supports);
            if (supports && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        }
        if (family == UINT32_MAX) return skip();
        float priority = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        const char* swap_extension = "VK_KHR_swapchain";
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &swap_extension;
        assert(vkCreateDevice(physical, &dci, nullptr, &vk_device) == VK_SUCCESS);
        if (vulkan_proc) {
            auto get_device_proc = (PFN_vkGetDeviceProcAddr)get_instance_proc(instance, "vkGetDeviceProcAddr");
            assert(get_device_proc);
#define VK_DEVICE(name) name = (PFN_##name)get_device_proc(vk_device, #name); assert(name);
            VK_DEVICE(vkGetDeviceQueue)
            VK_DEVICE(vkCreateSwapchainKHR)
            VK_DEVICE(vkQueuePresentKHR)
            VK_DEVICE(vkDestroySwapchainKHR)
            VK_DEVICE(vkDestroyDevice)
#undef VK_DEVICE
        }
        vkGetDeviceQueue(vk_device, family, 0, &vk_queue);
        unity_vulkan.instance = instance;
        unity_vulkan.physicalDevice = physical;
        unity_vulkan.device = vk_device;
        unity_vulkan.graphicsQueue = vk_queue;
        unity_vulkan.queueFamilyIndex = family;
        VkSurfaceCapabilitiesKHR caps{};
        assert(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) == VK_SUCCESS);
        assert(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr) == VK_SUCCESS);
        std::vector<VkSurfaceFormatKHR> formats(count);
        assert(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()) == VK_SUCCESS);
        auto chosen = formats[0];
        for (auto f : formats) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) chosen = f;
        sci.surface = surface; sci.minImageCount = caps.minImageCount;
        sci.imageFormat = chosen.format; sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = caps.currentExtent; sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform; sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE;
        assert(vkCreateSwapchainKHR(vk_device, &sci, nullptr, &vk_chain) == VK_SUCCESS);
        read_images();
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = family; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        assert(vkCreateCommandPool(vk_device, &pci, nullptr, &pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.commandBufferCount = 1;
        assert(vkAllocateCommandBuffers(vk_device, &cai, &commands) == VK_SUCCESS);
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        assert(vkCreateSemaphore(vk_device, &sem, nullptr, &acquired) == VK_SUCCESS);
        assert(vkCreateSemaphore(vk_device, &sem, nullptr, &ready) == VK_SUCCESS);
    }
    auto present_vulkan = [&] {
        uint32_t index = 0;
        assert(vkAcquireNextImageKHR(vk_device, vk_chain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index) == VK_SUCCESS);
        assert(vkResetCommandBuffer(commands, 0) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        assert(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        assert(vkEndCommandBuffer(commands) == VK_SUCCESS);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &commands;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready;
        assert(vkQueueSubmit(vk_queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &ready;
        present.swapchainCount = 1; present.pSwapchains = &vk_chain; present.pImageIndices = &index;
        assert(vkQueuePresentKHR(vk_queue, &present) == VK_SUCCESS);
        assert(vkDeviceWaitIdle(vk_device) == VK_SUCCESS);
    };
    if (late) {
        unity_chain = main_chain.Get();
        unity_queue = queue12.Get();
        load_overlay();
        if (api == "vulkan") {
            vkDestroySwapchainKHR(vk_device, vk_chain, nullptr);
            vkDestroySurfaceKHR(instance, surface, nullptr);
            VkWin32SurfaceCreateInfoKHR wci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            wci.hinstance = wc.hInstance; wci.hwnd = main;
            assert(vkCreateWin32SurfaceKHR(instance, &wci, nullptr, &surface) == VK_SUCCESS);
            sci.surface = surface;
            sci.oldSwapchain = VK_NULL_HANDLE;
            assert(vkCreateSwapchainKHR(vk_device, &sci, nullptr, &vk_chain) == VK_SUCCESS);
            read_images();
        }
    }
    const LONG_PTR main_proc = GetWindowLongPtrW(main, GWLP_WNDPROC);
    const LONG_PTR helper_proc = GetWindowLongPtrW(helper, GWLP_WNDPROC);
    auto present_main = [&] {
        if (api == "vulkan") present_vulkan();
        else if (api == "opengl") swap(dc);
        else if (api == "dx9") device9->Present(nullptr, nullptr, nullptr, nullptr);
        else if (api == "dx8") legacy_present_dx8(device8);
        else if (api == "dx7") legacy_end_scene_dx7(device7);
        else if (api == "gdi") StretchDIBits(main_dc, 0, 0, 640, 480, 0, 0, 640, 480,
            pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
        else if (main_chain) main_chain->Present(0, 0);
        else chain->Present(0, 0);
    };
    if (api == "gdi") {
        for (int i = 0; i < 23; ++i) { present_main(); Sleep(100); }
    }
    present_main();
    assert(GetWindowLongPtrW(main, GWLP_WNDPROC) != main_proc);
    for (int i = 0; i < 4; ++i) {
        if (vulkan_helper) present_vulkan();
        if (api != "opengl") swap(dc);
        if (api != "dx11") chain->Present(0, 0);
        assert(GetWindowLongPtrW(helper, GWLP_WNDPROC) == helper_proc);
        assert(GetWindowLongPtrW(main, GWLP_WNDPROC) != main_proc);
        present_main();
    }
    // Resize/reset the selected renderer, then let the unrelated API present
    // before the owner resumes. Selection and input ownership must survive.
    if (api == "vulkan") {
        sci.oldSwapchain = vk_chain;
        assert(vkCreateSwapchainKHR(vk_device, &sci, nullptr, &vk_chain) == VK_SUCCESS);
        vkDestroySwapchainKHR(vk_device, sci.oldSwapchain, nullptr);
        read_images();
    }
    if (api == "dx11") assert(SUCCEEDED(chain->ResizeBuffers(0, 640, 480, DXGI_FORMAT_UNKNOWN, 0)));
    if (api == "dx9") assert(SUCCEEDED(device9->Reset(&pp)));
    if (api == "dx8") assert(legacy_reset_dx8(device8, params8));
    if (api == "dx10") assert(SUCCEEDED(main_chain->ResizeBuffers(0, 640, 480, DXGI_FORMAT_UNKNOWN, 0)));
#ifdef _WIN64
    if (api == "dx12") {
        ComPtr<IDXGISwapChain3> chain3;
        assert(SUCCEEDED(main_chain.As(&chain3)));
        UINT masks[] = {0, 0};
        IUnknown* queues[] = {replacement_queue.Get(), replacement_queue.Get()};
        assert(SUCCEEDED(chain3->ResizeBuffers1(2, 640, 480, DXGI_FORMAT_UNKNOWN, 0, masks, queues)));
    }
#endif
    if (api == "opengl") {
        wglMakeCurrent(nullptr, nullptr);
        assert(wglDeleteContext(gl));
        gl = wglCreateContext(dc);
        assert(gl && wglMakeCurrent(dc, gl));
    }
    if (api != "opengl") swap(dc);
    else chain->Present(0, 0);
    present_main();
    assert(GetWindowLongPtrW(helper, GWLP_WNDPROC) == helper_proc);
    if (device12) assert(SUCCEEDED(device12->GetDeviceRemovedReason()));
    shutdown();
    assert(GetWindowLongPtrW(main, GWLP_WNDPROC) == main_proc);
    std::ifstream log_file(dir / "STAR" / "star.log");
    std::string log((std::istreambuf_iterator<char>(log_file)), {});
    const std::string marker = "Game graphics API locked:";
    auto first = log.find(marker);
    assert(first != std::string::npos && log.find(marker, first + marker.size()) == std::string::npos);
    const std::string initialized = "STAR initialized:";
    auto init_line = log.find(initialized);
    assert(init_line != std::string::npos && log.find(initialized, init_line + initialized.size()) == std::string::npos);
    const char* ready_message = api == "vulkan" ? "ImGui ready (Vulkan native)" : api == "opengl" ? "ImGui ready (OpenGL)" :
        api == "dx7" ? "ImGui ready (DX7)" : api == "dx8" ? "ImGui ready (DX8)" :
        api == "dx9" ? "ImGui ready (DX9)" : api == "dx10" ? "ImGui ready (DX10)" :
        api == "dx12" ? "ImGui ready (DX12)" : api == "gdi" ? "ImGui ready (GDI renderer)" : "ImGui ready (DX11)";
    assert(log.find(ready_message) != std::string::npos);
    for (const char* other : {"ImGui ready (DX7)", "ImGui ready (DX8)", "ImGui ready (DX9)", "ImGui ready (DX10)", "ImGui ready (DX11)",
            "ImGui ready (DX12)", "ImGui ready (OpenGL)", "ImGui ready (Vulkan native)", "ImGui ready (GDI renderer)"})
        if (std::string(other) != ready_message) assert(log.find(other) == std::string::npos);
    if (vk_device) {
        vkDestroySwapchainKHR(vk_device, vk_chain, nullptr);
        vkDestroyCommandPool(vk_device, pool, nullptr);
        vkDestroySemaphore(vk_device, acquired, nullptr);
        vkDestroySemaphore(vk_device, ready, nullptr);
        vkDestroyDevice(vk_device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(gl);
    ReleaseDC(gl_window, dc);
    if (device8) legacy_release_dx8(device8, params8);
    if (device7) legacy_release_dx7(device7);
    std::cout << "Backend ownership passed: " << api << '\n';
    // Hooks retain DLL function pointers until process exit; do not FreeLibrary.
}
