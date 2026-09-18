#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_vulkan.h"
#include <MinHook.h>
#include <vulkan/vulkan.h>

#define VULKAN_FUNCS \
    VK_FUNC(vkGetInstanceProcAddr) \
    VK_FUNC(vkGetDeviceProcAddr) \
    VK_FUNC(vkCreateInstance) \
    VK_FUNC(vkDestroyInstance) \
    VK_FUNC(vkCreateDevice) \
    VK_FUNC(vkDestroyDevice) \
    VK_FUNC(vkEnumeratePhysicalDevices) \
    VK_FUNC(vkGetPhysicalDeviceProperties) \
    VK_FUNC(vkGetPhysicalDeviceQueueFamilyProperties) \
    VK_FUNC(vkGetPhysicalDeviceMemoryProperties) \
    VK_FUNC(vkGetDeviceQueue) \
    VK_FUNC(vkCreateSwapchainKHR) \
    VK_FUNC(vkDestroySwapchainKHR) \
    VK_FUNC(vkGetSwapchainImagesKHR) \
    VK_FUNC(vkCreateRenderPass) \
    VK_FUNC(vkDestroyRenderPass) \
    VK_FUNC(vkCreateDescriptorPool) \
    VK_FUNC(vkDestroyDescriptorPool) \
    VK_FUNC(vkCreateCommandPool) \
    VK_FUNC(vkDestroyCommandPool) \
    VK_FUNC(vkAllocateCommandBuffers) \
    VK_FUNC(vkFreeCommandBuffers) \
    VK_FUNC(vkBeginCommandBuffer) \
    VK_FUNC(vkEndCommandBuffer) \
    VK_FUNC(vkCmdBeginRenderPass) \
    VK_FUNC(vkCmdEndRenderPass) \
    VK_FUNC(vkCmdPipelineBarrier) \
    VK_FUNC(vkCmdCopyBufferToImage) \
    VK_FUNC(vkCmdCopyImageToBuffer) \
    VK_FUNC(vkCreateFramebuffer) \
    VK_FUNC(vkDestroyFramebuffer) \
    VK_FUNC(vkCreateImage) \
    VK_FUNC(vkDestroyImage) \
    VK_FUNC(vkCreateImageView) \
    VK_FUNC(vkDestroyImageView) \
    VK_FUNC(vkCreateBuffer) \
    VK_FUNC(vkDestroyBuffer) \
    VK_FUNC(vkAllocateMemory) \
    VK_FUNC(vkFreeMemory) \
    VK_FUNC(vkBindBufferMemory) \
    VK_FUNC(vkBindImageMemory) \
    VK_FUNC(vkMapMemory) \
    VK_FUNC(vkUnmapMemory) \
    VK_FUNC(vkGetBufferMemoryRequirements) \
    VK_FUNC(vkGetImageMemoryRequirements) \
    VK_FUNC(vkCreateSampler) \
    VK_FUNC(vkDestroySampler) \
    VK_FUNC(vkCreateFence) \
    VK_FUNC(vkWaitForFences) \
    VK_FUNC(vkDestroyFence) \
    VK_FUNC(vkQueuePresentKHR) \
    VK_FUNC(vkDeviceWaitIdle) \
    VK_FUNC(vkResetCommandBuffer) \
    VK_FUNC(vkQueueSubmit)

#define VK_FUNC(name) static PFN_##name name = nullptr;
VULKAN_FUNCS
#undef VK_FUNC


static bool resolve_vulkan_funcs(HMODULE vulkan) {
    #define VK_FUNC(name) \
        name = (PFN_##name)GetProcAddress(vulkan, #name); \
        if (!name) return false;
    VULKAN_FUNCS
    #undef VK_FUNC
    return true;
}

static VkInstance g_vk_instance = VK_NULL_HANDLE;

static PFN_vkVoidFunction ImGuiVulkanLoader(const char* function_name, void* user_data) {
    HMODULE vulkan = (HMODULE)user_data;
    if (vkGetInstanceProcAddr && g_vk_instance) {
        auto addr = vkGetInstanceProcAddr(g_vk_instance, function_name);
        if (addr) return addr;
    }
    return (PFN_vkVoidFunction)GetProcAddress(vulkan, function_name);
}

static uint32_t vk_find_memory_type(VkPhysicalDevice pdev, uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(pdev, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((type_filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

// The game's swapchain is often sRGB (e.g. R8G8B8A8_SRGB). ImGui's shader
// outputs colors as-is, so an sRGB render pass would re-encode them and wash
// the overlay out. Render through a UNORM view of the same image instead: the
// formats are view-compatible, the game's sRGB content is preserved by
// LOAD_OP_LOAD, and ImGui's colors hit the screen unmodified.
static VkFormat unorm_view_format(VkFormat fmt)
{
    switch (fmt) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8_SRGB:    return VK_FORMAT_R8G8B8_UNORM;
        case VK_FORMAT_B8G8R8_SRGB:    return VK_FORMAT_B8G8R8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return fmt;
    }
}

struct VulkanOverlayData {
    HMODULE vulkan_dll = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> image_views;
    uint32_t image_count = 0;
    VkSampler icon_sampler = VK_NULL_HANDLE;
    std::vector<VkImage>        icon_images;
    std::vector<VkDeviceMemory> icon_memories;
    std::vector<VkImageView>    icon_views;

    void cleanup() {
        if (device) {
            if (vkDeviceWaitIdle) vkDeviceWaitIdle(device);
            for (auto v : icon_views)    if (v && vkDestroyImageView) vkDestroyImageView(device, v, nullptr);
            icon_views.clear();
            for (auto m : icon_memories) if (m && vkFreeMemory)       vkFreeMemory(device, m, nullptr);
            icon_memories.clear();
            for (auto i : icon_images)   if (i && vkDestroyImage)     vkDestroyImage(device, i, nullptr);
            icon_images.clear();
            if (icon_sampler && vkDestroySampler) vkDestroySampler(device, icon_sampler, nullptr);
            icon_sampler = VK_NULL_HANDLE;
            for (auto fb : framebuffers) if (fb && vkDestroyFramebuffer) vkDestroyFramebuffer(device, fb, nullptr);
            framebuffers.clear();
            for (auto iv : image_views) if (iv && vkDestroyImageView) vkDestroyImageView(device, iv, nullptr);
            image_views.clear();
            if (vkFreeCommandBuffers && command_pool) {
                vkFreeCommandBuffers(device, command_pool, (uint32_t)command_buffers.size(), command_buffers.data());
            }
            command_buffers.clear();
            if (vkDestroyCommandPool && command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            command_pool = VK_NULL_HANDLE;
            if (vkDestroyDescriptorPool && descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            descriptor_pool = VK_NULL_HANDLE;
            if (vkDestroyRenderPass && render_pass) vkDestroyRenderPass(device, render_pass, nullptr);
            render_pass = VK_NULL_HANDLE;
        }
        device = VK_NULL_HANDLE;
        physical_device = VK_NULL_HANDLE;
        instance = VK_NULL_HANDLE;
    }
};

int StarOverlay::hooked_vkCreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateInstance)(const void*, const void*, void**);
    auto orig = (PFN_vkCreateInstance)g_overlay->orig_vkCreateInstance_;
    VkResult res = orig(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && g_overlay) {
        g_overlay->vk_instance_ = *pInstance;
        g_vk_instance = (VkInstance)*pInstance;
    }
    return res;
}

int StarOverlay::hooked_vkCreateDevice(void* physicalDevice, const void* pCreateInfo, const void* pAllocator, void** pDevice)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateDevice)(void*, const void*, const void*, void**);
    auto orig = (PFN_vkCreateDevice)g_overlay->orig_vkCreateDevice_;
    VkResult res = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res == VK_SUCCESS && pDevice && g_overlay) {
        g_overlay->vk_physical_device_ = physicalDevice;
        g_overlay->vk_device_ = *pDevice;
        g_overlay->vk_queue_family_ = 0;

        struct FakeQueueCreateInfo {
            int sType;
            const void* pNext;
            int flags;
            uint32_t queueFamilyIndex;
        };
        struct FakeCreateInfo {
            int sType;
            const void* pNext;
            int flags;
            uint32_t queueCreateInfoCount;
            const FakeQueueCreateInfo* pQueueCreateInfos;
        };
        auto* ci = (const FakeCreateInfo*)pCreateInfo;
        if (ci && ci->queueCreateInfoCount > 0 && ci->pQueueCreateInfos) {
            g_overlay->vk_queue_family_ = ci->pQueueCreateInfos[0].queueFamilyIndex;
        }

        auto gdpa = (PFN_vkGetDeviceProcAddr)GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkGetDeviceProcAddr");
        if (gdpa) {
            void* pCreateSwapchain = (void*)gdpa((VkDevice)*pDevice, "vkCreateSwapchainKHR");
            if (pCreateSwapchain && !g_overlay->orig_vkCreateSwapchainKHR_) {
                MH_CreateHook(pCreateSwapchain, &hooked_vkCreateSwapchainKHR, (void**)&g_overlay->orig_vkCreateSwapchainKHR_);
                MH_EnableHook(pCreateSwapchain);
            }
        }
    }
    return res;
}

int StarOverlay::hooked_vkCreateSwapchainKHR(void* device, const void* pCreateInfo, const void* pAllocator, uint64_t* pSwapchain)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateSwapchainKHR)(void*, const void*, const void*, uint64_t*);
    auto orig = (PFN_vkCreateSwapchainKHR)g_overlay->orig_vkCreateSwapchainKHR_;
    VkResult res = orig ? orig(device, pCreateInfo, pAllocator, pSwapchain)
                        : ((PFN_vkCreateSwapchainKHR)GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkCreateSwapchainKHR"))(device, pCreateInfo, pAllocator, pSwapchain);
    if (res == VK_SUCCESS && pSwapchain && g_overlay) {
        // Recovery: if vkCreateDevice was missed (game init before SteamAPI_Init),
        // this still gives us the VkDevice for on_present_vulkan.
        if (device) g_overlay->vk_device_ = device;
        struct FakeSwapchainCreateInfo {
            int sType;
            const void* pNext;
            int flags;
            void* surface;
            uint32_t minImageCount;
            int imageFormat;
        };
        auto* ci = (const FakeSwapchainCreateInfo*)pCreateInfo;
        if (ci) {
            g_overlay->vk_swapchain_format_ = ci->imageFormat;
            g_overlay->vk_min_image_count_ = ci->minImageCount;
        }
        g_overlay->vk_swapchain_recreated_ = true;
        STAR_LOG("Vulkan swapchain created fmt=%d count=%u device=%p", g_overlay->vk_swapchain_format_, (unsigned)g_overlay->vk_min_image_count_, device);
    }
    return res;
}

int StarOverlay::hooked_vkQueuePresentKHR(void* queue, const void* pPresentInfo)
{
    if (g_overlay) {
        g_overlay->poll_hotkey();
        g_overlay->note_present();
        g_overlay->on_present_vulkan(queue, pPresentInfo);
    }
    typedef VkResult(VKAPI_PTR* PFN_vkQueuePresentKHR)(void*, const void*);
    auto orig = (PFN_vkQueuePresentKHR)g_overlay->orig_vkQueuePresentKHR_;
    return orig(queue, pPresentInfo);
}

int StarOverlay::hooked_vkAcquireNextImageKHR(void* device, uint64_t swapchain, uint64_t timeout, void* semaphore, void* fence, uint32_t* pImageIndex)
{
    if (g_overlay) {
        // Called every frame with the game's device + swapchain. This is the
        // recovery path for games that init Vulkan before SteamAPI_Init: the
        // create hooks never fire, but this still gives us the VkDevice.
        if (!g_overlay->vk_device_) g_overlay->vk_device_ = device;
        g_overlay->vk_swapchain_ = (void*)swapchain;
    }
    typedef VkResult(VKAPI_PTR* PFN_vkAcquireNextImageKHR)(void*, uint64_t, uint64_t, void*, void*, uint32_t*);
    auto orig = (PFN_vkAcquireNextImageKHR)g_overlay->orig_vkAcquireNextImageKHR_;
    return orig(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

void StarOverlay::hook_vulkan()
{
    if (orig_vkQueuePresentKHR_ && orig_vkCreateDevice_ && orig_vkCreateInstance_) { vulkan_hooked_ = true; return; }
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan) vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) return;

    void* pCreateInstance = (void*)GetProcAddress(vulkan, "vkCreateInstance");
    void* pCreateDevice = (void*)GetProcAddress(vulkan, "vkCreateDevice");
    void* pQueuePresent = (void*)GetProcAddress(vulkan, "vkQueuePresentKHR");
    void* pCreateSwapchain = (void*)GetProcAddress(vulkan, "vkCreateSwapchainKHR");
    void* pAcquireNextImage = (void*)GetProcAddress(vulkan, "vkAcquireNextImageKHR");

    if (pCreateInstance && !orig_vkCreateInstance_) {
        if (MH_CreateHook(pCreateInstance, &hooked_vkCreateInstance, (void**)&orig_vkCreateInstance_) == MH_OK)
            MH_EnableHook(pCreateInstance);
    }
    if (pCreateDevice && !orig_vkCreateDevice_) {
        if (MH_CreateHook(pCreateDevice, &hooked_vkCreateDevice, (void**)&orig_vkCreateDevice_) == MH_OK)
            MH_EnableHook(pCreateDevice);
    }
    if (pQueuePresent && !orig_vkQueuePresentKHR_) {
        if (MH_CreateHook(pQueuePresent, &hooked_vkQueuePresentKHR, (void**)&orig_vkQueuePresentKHR_) == MH_OK) {
            MH_EnableHook(pQueuePresent);
            STAR_LOG("Vulkan QueuePresent hooked");
        }
    }
    // Loader export: catches swapchain recreates even when vkCreateDevice was missed.
    if (pCreateSwapchain && !orig_vkCreateSwapchainKHR_) {
        if (MH_CreateHook(pCreateSwapchain, &hooked_vkCreateSwapchainKHR, (void**)&orig_vkCreateSwapchainKHR_) == MH_OK) {
            MH_EnableHook(pCreateSwapchain);
            STAR_LOG("Vulkan CreateSwapchain hooked (loader)");
        }
    }
    // Loader export: fires every frame with the game's device + swapchain.
    // Recovery for games that init Vulkan before SteamAPI_Init (the create
    // hooks are missed); on_present_vulkan uses the captured device to
    // rebuild the instance/physical device at present time.
    if (pAcquireNextImage && !orig_vkAcquireNextImageKHR_) {
        if (MH_CreateHook(pAcquireNextImage, &hooked_vkAcquireNextImageKHR, (void**)&orig_vkAcquireNextImageKHR_) == MH_OK) {
            MH_EnableHook(pAcquireNextImage);
            STAR_LOG("Vulkan AcquireNextImage hooked (loader)");
        }
    }
    if (orig_vkQueuePresentKHR_) { vulkan_hooked_ = true; STAR_LOG("Vulkan hooked"); }
}

void StarOverlay::recover_vulkan_late()
{
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan) return;
    if (!resolve_vulkan_funcs(vulkan)) return;

    // The game's instance was created before we hooked, so we can't capture
    // it. Create a throwaway instance to enumerate the physical device and
    // satisfy ImGui's backend (which asserts Instance + PhysicalDevice).
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "STAR";
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo inst_info = {};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;

    VkInstance dummy = VK_NULL_HANDLE;
    if (vkCreateInstance(&inst_info, nullptr, &dummy) != VK_SUCCESS) return;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(dummy, &count, nullptr);
    if (count == 0) { vkDestroyInstance(dummy, nullptr); return; }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(dummy, &count, devices.data());

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    for (auto pd : devices) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { chosen = pd; break; }
    }
    if (!chosen && !devices.empty()) chosen = devices[0];

    vk_instance_ = dummy;
    vk_physical_device_ = chosen;
    g_vk_instance = dummy;
    STAR_LOG("Vulkan late recovery: device=%p instance=%p physical=%p fmt=%d count=%u",
        vk_device_, vk_instance_, vk_physical_device_, vk_swapchain_format_, (unsigned)vk_min_image_count_);
}

void StarOverlay::on_present_vulkan(void* queue, const void* pPresentInfo)
{
    if (!enabled_) return;
    if (game_api_ == GraphicsAPI::None) {
        game_api_ = GraphicsAPI::Vulkan;
        STAR_LOG("Game graphics API: Vulkan");
        // In-backbuffer Vulkan drawing submits without the game's present
        // semaphores and a lying initialLayout - a GPU-hang vector this
        // session cannot validate. Auto mode takes the external window
        // (proven path); explicit "hook" keeps the old behavior.
        if (Settings::get().overlay_mode == "auto") {
            switch_to_external("Vulkan in-backbuffer drawing disabled (hang risk)");
            return;
        }
    }
    if (mode_ == OverlayMode::External) return;
    if (!vk_device_ || !vk_instance_) {
        // Late-init recovery: the game created its Vulkan instance/device
        // before SteamAPI_Init, so the create hooks were missed. The
        // vkCreateSwapchainKHR / vkAcquireNextImageKHR hooks captured the
        // device; rebuild the instance + physical device here so the overlay
        // can init.
        if (!vk_instance_ && vk_device_) {
            recover_vulkan_late();
            // The recovery's format/count are unknown until the swapchain
            // hook fires; init on a later present so the real values are used.
            vk_swapchain_recreated_ = true;
            return;
        }
        static bool logged = false;
        if (!logged) { logged = true; STAR_LOG("Vulkan present: waiting for device/instance (device=%p instance=%p) - game may have init before SteamAPI_Init, will recover on swapchain recreate", vk_device_, vk_instance_); }
        return;
    }

    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    struct FakePresentInfo {
        int sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const uint64_t* pWaitSemaphores;
        uint32_t swapchainCount;
        const uint64_t* pSwapchains;
        const uint32_t* pImageIndices;
    };
    auto* pi = (const FakePresentInfo*)pPresentInfo;
    if (!pi || pi->swapchainCount == 0 || !pi->pSwapchains || !pi->pImageIndices) return;

    uint32_t image_index = pi->pImageIndices[0];
    vk_queue_ = queue;

    if (vk_swapchain_recreated_ || !imgui_initialized_) {
        cleanup_vulkan();
        init_imgui_vulkan(queue, pPresentInfo);
        vk_swapchain_recreated_ = false;
    }

    if (imgui_initialized_ && active_api_ == GraphicsAPI::Vulkan) {
        render_frame_vulkan(queue, pPresentInfo);
    }
}

void StarOverlay::init_imgui_vulkan(void* queue, const void* pPresentInfo)
{
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan) return;

    if (!resolve_vulkan_funcs(vulkan)) return;

    VkAttachmentDescription attachment = {};
    attachment.format = unorm_view_format((VkFormat)vk_swapchain_format_);
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_attachment = {};
    color_attachment.attachment = 0;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass((VkDevice)vk_device_, &rp_info, nullptr, &rp) != VK_SUCCESS) return;

    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 257 }
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 257;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = pool_sizes;

    VkDescriptorPool dp = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool((VkDevice)vk_device_, &pool_info, nullptr, &dp) != VK_SUCCESS) {
        vkDestroyRenderPass((VkDevice)vk_device_, rp, nullptr);
        return;
    }

    struct FakePresentInfo {
        int sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const uint64_t* pWaitSemaphores;
        uint32_t swapchainCount;
        const uint64_t* pSwapchains;
        const uint32_t* pImageIndices;
    };
    auto* pi = (const FakePresentInfo*)pPresentInfo;
    uint64_t swapchain = pi->pSwapchains[0];

    uint32_t count = 0;
    vkGetSwapchainImagesKHR((VkDevice)vk_device_, (VkSwapchainKHR)swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR((VkDevice)vk_device_, (VkSwapchainKHR)swapchain, &count, images.data());

    std::vector<VkImageView> views(count);
    std::vector<VkFramebuffer> fbs(count);

    if (!hwnd_) hwnd_ = GetActiveWindow();
    RECT rect{};
    if (hwnd_) GetClientRect(hwnd_, &rect);
    uint32_t w = rect.right - rect.left;
    uint32_t h = rect.bottom - rect.top;
    if (w == 0) w = 1280;
    if (h == 0) h = 720;

    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = unorm_view_format((VkFormat)vk_swapchain_format_);
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        if (vkCreateImageView((VkDevice)vk_device_, &view_info, nullptr, &views[i]) != VK_SUCCESS) return;

        VkFramebufferCreateInfo fb_info = {};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = rp;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &views[i];
        fb_info.width = w;
        fb_info.height = h;
        fb_info.layers = 1;

        if (vkCreateFramebuffer((VkDevice)vk_device_, &fb_info, nullptr, &fbs[i]) != VK_SUCCESS) return;
    }

    VkCommandPoolCreateInfo cp_info = {};
    cp_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_info.queueFamilyIndex = vk_queue_family_;

    VkCommandPool cp = VK_NULL_HANDLE;
    if (vkCreateCommandPool((VkDevice)vk_device_, &cp_info, nullptr, &cp) != VK_SUCCESS) return;

    std::vector<VkCommandBuffer> cbs(count);
    VkCommandBufferAllocateInfo cb_info = {};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.commandPool = cp;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = count;
    if (vkAllocateCommandBuffers((VkDevice)vk_device_, &cb_info, cbs.data()) != VK_SUCCESS) return;

    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd_);
    hook_window();

    ImGui_ImplVulkan_LoadFunctions(&ImGuiVulkanLoader, vulkan);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = (VkInstance)vk_instance_;
    init_info.PhysicalDevice = (VkPhysicalDevice)vk_physical_device_;
    init_info.Device = (VkDevice)vk_device_;
    init_info.QueueFamily = vk_queue_family_;
    init_info.Queue = (VkQueue)queue;
    init_info.DescriptorPool = dp;
    init_info.RenderPass = rp;
    init_info.MinImageCount = vk_min_image_count_;
    init_info.ImageCount = count;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (ImGui_ImplVulkan_Init(&init_info)) {
        style_.setup();
        imgui_initialized_ = true;
        active_api_ = GraphicsAPI::Vulkan;

        auto* data = new VulkanOverlayData();
        data->vulkan_dll = vulkan;
        data->instance = (VkInstance)vk_instance_;
        data->physical_device = (VkPhysicalDevice)vk_physical_device_;
        data->device = (VkDevice)vk_device_;
        data->queue = (VkQueue)queue;
        data->queue_family = vk_queue_family_;
        data->render_pass = rp;
        data->descriptor_pool = dp;
        data->command_pool = cp;
        data->command_buffers = cbs;
        data->framebuffers = fbs;
        data->image_views = views;
        data->image_count = count;

        VkSamplerCreateInfo samp_info = {};
        samp_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samp_info.magFilter    = VK_FILTER_LINEAR;
        samp_info.minFilter    = VK_FILTER_LINEAR;
        samp_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samp_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_info.maxLod       = 1.f;
        if (vkCreateSampler((VkDevice)vk_device_, &samp_info, nullptr, &data->icon_sampler) != VK_SUCCESS)
            data->icon_sampler = VK_NULL_HANDLE;

        context_ = (ID3D11DeviceContext*)data;
    }
}

void StarOverlay::maybe_capture_vulkan(void* queue, const void* pPresentInfo)
{
    if (!screenshots_.consume()) return;
    if (!enabled_ || !imgui_initialized_ || active_api_ != GraphicsAPI::Vulkan) return;
    if (!vk_device_ || !vk_physical_device_) return;
    struct FakePresentInfo {
        int sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const uint64_t* pWaitSemaphores;
        uint32_t swapchainCount;
        const uint64_t* pSwapchains;
        const uint32_t* pImageIndices;
    };
    auto* pi = (const FakePresentInfo*)pPresentInfo;
    if (!pi || pi->swapchainCount == 0 || !pi->pSwapchains || !pi->pImageIndices) return;
    VkDevice dev = (VkDevice)vk_device_;
    VkSwapchainKHR swap = (VkSwapchainKHR)pi->pSwapchains[0];
    uint32_t idx = pi->pImageIndices[0];
    uint32_t count = 0;
    if (!vkGetSwapchainImagesKHR || vkGetSwapchainImagesKHR(dev, swap, &count, nullptr) != VK_SUCCESS || idx >= count)
        return;
    std::vector<VkImage> images(count);
    if (vkGetSwapchainImagesKHR(dev, swap, &count, images.data()) != VK_SUCCESS) return;
    VkImage image = images[idx];

    RECT rect{};
    uint32_t w = 1280, h = 720;
    if (hwnd_ && GetClientRect(hwnd_, &rect)) {
        if (rect.right - rect.left > 0) w = (uint32_t)(rect.right - rect.left);
        if (rect.bottom - rect.top > 0) h = (uint32_t)(rect.bottom - rect.top);
    }
    VkFormat fmt = (VkFormat)vk_swapchain_format_;
    bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    if (fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_SRGB && !bgra) {
        STAR_LOG("Screenshot: unsupported Vulkan format %d", (int)fmt);
        return;
    }
    auto* data = (VulkanOverlayData*)context_;
    if (!data || !data->command_pool) return;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = (VkDeviceSize)w * h * 4;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &buf_info, nullptr, &staging) != VK_SUCCESS) return;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, staging, &req);
    uint32_t mtype = vk_find_memory_type((VkPhysicalDevice)vk_physical_device_, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (mtype == UINT32_MAX) { vkDestroyBuffer(dev, staging, nullptr); return; }
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = mtype;
    if (vkAllocateMemory(dev, &alloc_info, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, staging, nullptr);
        return;
    }
    vkBindBufferMemory(dev, staging, mem, 0);

    VkCommandBufferAllocateInfo cb_alloc{};
    cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_alloc.commandPool = data->command_pool;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool shot_ok = false;
    std::string shot_path;
    if (vkAllocateCommandBuffers(dev, &cb_alloc, &cb) == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &begin);
        auto barrier = [&](VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                           VkAccessFlags sa, VkAccessFlags da) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = 0; b.subresourceRange.levelCount = 1;
            b.subresourceRange.baseArrayLayer = 0; b.subresourceRange.layerCount = 1;
            b.srcAccessMask = sa; b.dstAccessMask = da;
            vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        barrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = w; copy.imageExtent.height = h; copy.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, 0);
        vkEndCommandBuffer(cb);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(dev, &fence_info, nullptr, &fence) == VK_SUCCESS) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cb;
            if (vkQueueSubmit((VkQueue)queue, 1, &submit, fence) == VK_SUCCESS)
                vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ULL);
            void* mapped = nullptr;
            if (vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped) {
                std::vector<uint8_t> rgba((size_t)w * h * 4);
                if (bgra) {
                    const uint8_t* s = (const uint8_t*)mapped;
                    uint8_t* d = rgba.data();
                    for (size_t p = 0; p < (size_t)w * h; p++) {
                        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                        s += 4; d += 4;
                    }
                } else {
                    memcpy(rgba.data(), mapped, rgba.size());
                }
                vkUnmapMemory(dev, mem);
                shot_path = ScreenshotService::next_path();
                if (!shot_path.empty() && ScreenshotService::save_rgba_png(shot_path, rgba.data(), (int)w, (int)h))
                    shot_ok = true;
            }
            vkDestroyFence(dev, fence, nullptr);
        }
        vkFreeCommandBuffers(dev, data->command_pool, 1, &cb);
    }
    vkFreeMemory(dev, mem, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);
    if (shot_ok) notify_screenshot(shot_path);
}

void StarOverlay::render_frame_vulkan(void* queue, const void* pPresentInfo)
{
    auto* data = (VulkanOverlayData*)context_;
    if (!data) return;

    struct FakePresentInfo {
        int sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const uint64_t* pWaitSemaphores;
        uint32_t swapchainCount;
        const uint64_t* pSwapchains;
        const uint32_t* pImageIndices;
    };
    auto* pi = (const FakePresentInfo*)pPresentInfo;
    uint32_t image_index = pi->pImageIndices[0];
    if (image_index >= data->image_count) return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    apply_cursor_mode();

    build_frame_ui();

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBuffer cb = data->command_buffers[image_index];
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &begin_info);

    RECT rect{};
    if (hwnd_) GetClientRect(hwnd_, &rect);
    uint32_t w = rect.right - rect.left;
    uint32_t h = rect.bottom - rect.top;
    if (w == 0) w = 1280;
    if (h == 0) h = 720;

    VkRenderPassBeginInfo rp_begin = {};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = data->render_pass;
    rp_begin.framebuffer = data->framebuffers[image_index];
    rp_begin.renderArea.extent.width = w;
    rp_begin.renderArea.extent.height = h;

    vkCmdBeginRenderPass(cb, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;

    if (vkQueueSubmit) {
        vkQueueSubmit((VkQueue)queue, 1, &submit, VK_NULL_HANDLE);
    }

    maybe_capture_vulkan(queue, pPresentInfo);
}

ImTextureID StarOverlay::upload_icon_vulkan(const std::vector<uint8_t>& rgba, int w, int h)
{
    auto* data = (VulkanOverlayData*)context_;
    if (!data || !data->device || !data->icon_sampler || !data->command_pool) return nullptr;

    VkDevice dev  = data->device;
    VkQueue  que  = data->queue;
    VkCommandPool pool = data->command_pool;

    VkBufferCreateInfo buf_info = {};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size  = (VkDeviceSize)w * h * 4;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &buf_info, nullptr, &staging) != VK_SUCCESS) return nullptr;

    VkMemoryRequirements buf_req{};
    vkGetBufferMemoryRequirements(dev, staging, &buf_req);
    uint32_t buf_mtype = vk_find_memory_type((VkPhysicalDevice)vk_physical_device_, buf_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_mtype == UINT32_MAX) { vkDestroyBuffer(dev, staging, nullptr); return nullptr; }

    VkMemoryAllocateInfo buf_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    buf_alloc.allocationSize  = buf_req.size;
    buf_alloc.memoryTypeIndex = buf_mtype;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &buf_alloc, nullptr, &staging_mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, staging, nullptr); return nullptr;
    }
    vkBindBufferMemory(dev, staging, staging_mem, 0);

    void* mapped = nullptr;
    vkMapMemory(dev, staging_mem, 0, buf_req.size, 0, &mapped);
    memcpy(mapped, rgba.data(), (size_t)w * h * 4);
    vkUnmapMemory(dev, staging_mem);

    VkImageCreateInfo img_info = {};
    img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType     = VK_IMAGE_TYPE_2D;
    img_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent        = { (uint32_t)w, (uint32_t)h, 1 };
    img_info.mipLevels     = 1;
    img_info.arrayLayers   = 1;
    img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &img_info, nullptr, &image) != VK_SUCCESS) {
        vkFreeMemory(dev, staging_mem, nullptr); vkDestroyBuffer(dev, staging, nullptr); return nullptr;
    }

    VkMemoryRequirements img_req{};
    vkGetImageMemoryRequirements(dev, image, &img_req);
    uint32_t img_mtype = vk_find_memory_type((VkPhysicalDevice)vk_physical_device_, img_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_mtype == UINT32_MAX) {
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr); vkDestroyBuffer(dev, staging, nullptr); return nullptr;
    }
    VkMemoryAllocateInfo img_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    img_alloc.allocationSize  = img_req.size;
    img_alloc.memoryTypeIndex = img_mtype;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &img_alloc, nullptr, &img_mem) != VK_SUCCESS) {
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return nullptr;
    }
    vkBindImageMemory(dev, image, img_mem, 0);

    VkCommandBufferAllocateInfo cb_alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cb_alloc.commandPool        = pool;
    cb_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &cb_alloc, &cb) != VK_SUCCESS) {
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return nullptr;
    }

    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    auto transition = [&](VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                          VkAccessFlags src_access, VkAccessFlags dst_access) {
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout           = from;
        barrier.newLayout           = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask       = src_access;
        barrier.dstAccessMask       = dst_access;
        vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };

    transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy copy = {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent      = { (uint32_t)w, (uint32_t)h, 1 };
    vkCmdCopyBufferToImage(cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    vkEndCommandBuffer(cb);

    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(dev, pool, 1, &cb);
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return nullptr;
    }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    if (vkQueueSubmit(que, 1, &submit, fence) != VK_SUCCESS) {
        vkDestroyFence(dev, fence, nullptr);
        vkFreeCommandBuffers(dev, pool, 1, &cb);
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return nullptr;
    }
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    vkFreeMemory(dev, staging_mem, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);

    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view_info.image            = image;
    view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format           = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &view_info, nullptr, &view) != VK_SUCCESS) {
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        return nullptr;
    }

    data->icon_images.push_back(image);
    data->icon_memories.push_back(img_mem);
    data->icon_views.push_back(view);

    return reinterpret_cast<ImTextureID>(ImGui_ImplVulkan_AddTexture(data->icon_sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

void StarOverlay::cleanup_vulkan()
{
    if (active_api_ == GraphicsAPI::Vulkan && context_) {
        auto* data = (VulkanOverlayData*)context_;
        data->cleanup();
        delete data;
        context_ = nullptr;
    }
}

