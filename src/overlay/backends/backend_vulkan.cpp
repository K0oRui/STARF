#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_vulkan.h"
#include <MinHook.h>
#include <vulkan/vulkan.h>
#include <memory>

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
    VK_FUNC(vkCreateSemaphore) \
    VK_FUNC(vkDestroySemaphore) \
    VK_FUNC(vkResetFences) \
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
    uint32_t next_frame = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkFence> fences;
    std::vector<VkSemaphore> present_ready;
    bool failed = false;
    VkSampler icon_sampler = VK_NULL_HANDLE;
    std::vector<VkImage>        icon_images;
    std::vector<VkDeviceMemory> icon_memories;
    std::vector<VkImageView>    icon_views;

    void cleanup() {
        if (device) {
            if (vkDeviceWaitIdle) vkDeviceWaitIdle(device);
            for (auto fence : fences) if (fence) vkDestroyFence(device, fence, nullptr);
            fences.clear();
            for (auto semaphore : present_ready) if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
            present_ready.clear();
            for (auto v : icon_views)    if (v && vkDestroyImageView) vkDestroyImageView(device, v, nullptr);
            icon_views.clear();
            for (auto i : icon_images) if (i) vkDestroyImage(device, i, nullptr);
            icon_images.clear();
            for (auto m : icon_memories) if (m) vkFreeMemory(device, m, nullptr);
            icon_memories.clear();
            if (icon_sampler && vkDestroySampler) vkDestroySampler(device, icon_sampler, nullptr);
            icon_sampler = VK_NULL_HANDLE;
            for (auto fb : framebuffers) if (fb && vkDestroyFramebuffer) vkDestroyFramebuffer(device, fb, nullptr);
            framebuffers.clear();
            for (auto iv : image_views) if (iv && vkDestroyImageView) vkDestroyImageView(device, iv, nullptr);
            image_views.clear();
            if (vkFreeCommandBuffers && command_pool && !command_buffers.empty()) {
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

int WINAPI StarOverlay::hooked_vkCreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateInstance)(const void*, const void*, void**);
    auto orig = (PFN_vkCreateInstance)g_overlay->orig_vkCreateInstance_;
    VkResult res = orig(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && g_overlay) {
        std::lock_guard<std::mutex> lock(g_overlay->render_mutex_);
        g_overlay->vk_instance_ = *pInstance;
        g_vk_instance = (VkInstance)*pInstance;
    }
    return res;
}

int WINAPI StarOverlay::hooked_vkCreateDevice(void* physicalDevice, const void* pCreateInfo, const void* pAllocator, void** pDevice)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateDevice)(void*, const void*, const void*, void**);
    auto orig = (PFN_vkCreateDevice)g_overlay->orig_vkCreateDevice_;
    VkResult res = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res == VK_SUCCESS && pDevice && g_overlay) {
        std::lock_guard<std::mutex> lock(g_overlay->render_mutex_);
        g_overlay->vk_physical_device_ = physicalDevice;
        g_overlay->vk_device_ = *pDevice;

        auto* ci = static_cast<const VkDeviceCreateInfo*>(pCreateInfo);
        g_overlay->vk_queue_family_ = UINT32_MAX;
        // Only a captured graphics queue is eligible for native rendering.
        auto module = GetModuleHandleA("vulkan-1.dll");
        auto get_properties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetProcAddress(module, "vkGetPhysicalDeviceQueueFamilyProperties");
        auto get_queue = (PFN_vkGetDeviceQueue)GetProcAddress(module, "vkGetDeviceQueue");
        if (ci && get_properties && get_queue && ci->queueCreateInfoCount == 1) {
            uint32_t count = 0;
            get_properties((VkPhysicalDevice)physicalDevice, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            get_properties((VkPhysicalDevice)physicalDevice, &count, families.data());
            const auto& qci = ci->pQueueCreateInfos[0];
            if (!qci.flags && qci.queueFamilyIndex < count &&
                (families[qci.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                g_overlay->vk_queue_family_ = qci.queueFamilyIndex;
                VkQueue graphics_queue{};
                get_queue((VkDevice)*pDevice, qci.queueFamilyIndex, 0, &graphics_queue);
                g_overlay->vk_queue_ = graphics_queue;
            }
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

int WINAPI StarOverlay::hooked_vkCreateSwapchainKHR(void* device, const void* pCreateInfo, const void* pAllocator, uint64_t* pSwapchain)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateSwapchainKHR)(void*, const void*, const void*, uint64_t*);
    auto orig = (PFN_vkCreateSwapchainKHR)g_overlay->orig_vkCreateSwapchainKHR_;
    VkResult res = orig ? orig(device, pCreateInfo, pAllocator, pSwapchain)
                        : ((PFN_vkCreateSwapchainKHR)GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkCreateSwapchainKHR"))(device, pCreateInfo, pAllocator, pSwapchain);
    if (res == VK_SUCCESS && pSwapchain && g_overlay) {
        std::lock_guard<std::mutex> lock(g_overlay->render_mutex_);
        // Recovery: if vkCreateDevice was missed (game init before SteamAPI_Init),
        // this still gives us the VkDevice for on_present_vulkan.
        if (device) g_overlay->vk_device_ = device;
        auto* ci = static_cast<const VkSwapchainCreateInfoKHR*>(pCreateInfo);
        if (ci) {
            g_overlay->vk_swapchain_format_ = ci->imageFormat;
            g_overlay->vk_min_image_count_ = ci->minImageCount;
            g_overlay->vk_width_ = ci->imageExtent.width;
            g_overlay->vk_height_ = ci->imageExtent.height;
            g_overlay->vk_image_usage_ = ci->imageUsage;
            g_overlay->vk_swapchain_ = (void*)(uintptr_t)*pSwapchain;
        }
        g_overlay->vk_swapchain_recreated_ = true;
        STAR_LOG("Vulkan swapchain created fmt=%d count=%u device=%p", g_overlay->vk_swapchain_format_, (unsigned)g_overlay->vk_min_image_count_, device);
    }
    return res;
}

void WINAPI StarOverlay::hooked_vkDestroySwapchainKHR(void* device, uint64_t swapchain, const void* allocator)
{
    if (!g_overlay) return;
    {
        std::lock_guard<std::mutex> lock(g_overlay->render_mutex_);
        auto* data = (VulkanOverlayData*)g_overlay->vk_data_;
        if (data && (uintptr_t)data->swapchain == (uintptr_t)swapchain)
            g_overlay->cleanup_vulkan();
        if ((uintptr_t)g_overlay->vk_swapchain_ == (uintptr_t)swapchain) {
            g_overlay->vk_swapchain_ = nullptr;
            g_overlay->vk_width_ = g_overlay->vk_height_ = 0;
        }
    }
    auto original = (PFN_vkDestroySwapchainKHR)g_overlay->orig_vkDestroySwapchainKHR_;
    original((VkDevice)device, (VkSwapchainKHR)swapchain, (const VkAllocationCallbacks*)allocator);
}

void WINAPI StarOverlay::hooked_vkDestroyDevice(void* device, const void* allocator)
{
    if (!g_overlay) return;
    {
        std::lock_guard<std::mutex> lock(g_overlay->render_mutex_);
        if (device == g_overlay->vk_device_) {
            g_overlay->cleanup_vulkan();
            g_overlay->vk_device_ = g_overlay->vk_physical_device_ = g_overlay->vk_queue_ = nullptr;
        }
    }
    auto original = (PFN_vkDestroyDevice)g_overlay->orig_vkDestroyDevice_;
    original((VkDevice)device, (const VkAllocationCallbacks*)allocator);
}

int WINAPI StarOverlay::hooked_vkQueuePresentKHR(void* queue, const void* pPresentInfo)
{
    auto* overlay = g_overlay;
    typedef VkResult(VKAPI_PTR* PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR*);
    auto orig = overlay ? (PFN_vkQueuePresentKHR)overlay->orig_vkQueuePresentKHR_ : nullptr;
    if (!orig)
        orig = (PFN_vkQueuePresentKHR)GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkQueuePresentKHR");
    if (!orig) return VK_ERROR_INITIALIZATION_FAILED;
    // Never alter behavior for malformed calls: forward them untouched.
    if (!overlay || !pPresentInfo) return orig((VkQueue)queue, (const VkPresentInfoKHR*)pPresentInfo);
    // Forward a private copy: render_frame_vulkan replaces only the wait list.
    VkPresentInfoKHR present = *static_cast<const VkPresentInfoKHR*>(pPresentInfo);
    overlay->poll_hotkey();
    overlay->note_present();
    overlay->on_present_vulkan(queue, &present);
    return orig((VkQueue)queue, &present);
}

int WINAPI StarOverlay::hooked_vkAcquireNextImageKHR(void* device, uint64_t swapchain, uint64_t timeout, uint64_t semaphore, uint64_t fence, uint32_t* pImageIndex)
{
    typedef VkResult(VKAPI_PTR* PFN_vkAcquireNextImageKHR)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t*);
    auto orig = (PFN_vkAcquireNextImageKHR)g_overlay->orig_vkAcquireNextImageKHR_;
    return orig(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

void StarOverlay::hook_vulkan()
{
    static std::mutex install_mutex;
    std::lock_guard<std::mutex> install_lock(install_mutex);
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
    auto install = [&](const char* name, void* detour, void** original) {
        if (*original) return;
        void* target = (void*)GetProcAddress(vulkan, name);
        if (target && MH_CreateHook(target, detour, original) == MH_OK &&
            MH_EnableHook(target) != MH_OK) {
            MH_RemoveHook(target);
            *original = nullptr;
        }
    };
    install("vkDestroySwapchainKHR", (void*)&hooked_vkDestroySwapchainKHR, &orig_vkDestroySwapchainKHR_);
    install("vkDestroyDevice", (void*)&hooked_vkDestroyDevice, &orig_vkDestroyDevice_);
    if (orig_vkQueuePresentKHR_) { vulkan_hooked_ = true; STAR_LOG("Vulkan hooked"); }
}

void StarOverlay::on_present_vulkan(void* queue, const void* pPresentInfo)
{
    if (!enabled_) return;
    if (game_api_ == GraphicsAPI::None) {
        game_api_ = GraphicsAPI::Vulkan;
        STAR_LOG("Game graphics API: Vulkan");
    }
    if (mode_ == OverlayMode::External) return;
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto* pi = static_cast<const VkPresentInfoKHR*>(pPresentInfo);
    if (!pi || !pi->swapchainCount || !pi->pSwapchains || !pi->pImageIndices) return;
    if (!orig_vkDestroySwapchainKHR_ || !orig_vkDestroyDevice_ ||
        !vk_device_ || !vk_instance_ || !vk_physical_device_ ||
        !vk_width_ || !vk_height_ || vk_queue_family_ == UINT32_MAX ||
        queue != vk_queue_ || pi->swapchainCount != 1 ||
        (uintptr_t)pi->pSwapchains[0] != (uintptr_t)vk_swapchain_ ||
        !(vk_image_usage_ & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            STAR_LOG("Vulkan native unavailable: creation metadata missing or unsupported queue/swapchain; instance=%p physical=%p device=%p queue=%p captured=%p extent=%ux%u",
                vk_instance_, vk_physical_device_, vk_device_, queue, vk_queue_, vk_width_, vk_height_);
        }
        // There is no Vulkan API to reconstruct a device's physical GPU and
        // enabled queues. A dummy instance cannot supply that information.
        if (Settings::get().overlay_mode != "hook") {
            cleanup_vulkan();
            mode_ = OverlayMode::External;
            STAR_LOG("Vulkan: using external overlay for this session (creation metadata unavailable)");
            start_external_thread();
        }
        return;
    }
    if (imgui_initialized_ && active_api_ != GraphicsAPI::Vulkan) return;
    if (vk_swapchain_recreated_ || !imgui_initialized_) {
        cleanup_vulkan();
        init_imgui_vulkan(queue, pPresentInfo);
        vk_swapchain_recreated_ = false;
    }
    if (imgui_initialized_ && active_api_ == GraphicsAPI::Vulkan)
        render_frame_vulkan(queue, pPresentInfo);
}

void StarOverlay::init_imgui_vulkan(void* queue, const void* pPresentInfo)
{
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan) return;

    if (!resolve_vulkan_funcs(vulkan)) return;

    auto destroy = [](VulkanOverlayData* p) { p->cleanup(); delete p; };
    std::unique_ptr<VulkanOverlayData, decltype(destroy)> owner(new VulkanOverlayData, destroy);
    auto* data = owner.get();
    data->device = (VkDevice)vk_device_;
    data->physical_device = (VkPhysicalDevice)vk_physical_device_;
    data->instance = (VkInstance)vk_instance_;
    data->queue = (VkQueue)queue;
    data->queue_family = vk_queue_family_;
    data->extent = {vk_width_, vk_height_};
    VkAttachmentDescription attachment = {};
    attachment.format = unorm_view_format((VkFormat)vk_swapchain_format_);
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    auto& rp = data->render_pass;
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

    auto& dp = data->descriptor_pool;
    if (vkCreateDescriptorPool((VkDevice)vk_device_, &pool_info, nullptr, &dp) != VK_SUCCESS) {
        return;
    }

    auto* pi = static_cast<const VkPresentInfoKHR*>(pPresentInfo);
    VkSwapchainKHR swapchain = pi->pSwapchains[0];

    uint32_t count = 0;
    if (vkGetSwapchainImagesKHR((VkDevice)vk_device_, swapchain, &count, nullptr) != VK_SUCCESS || count < 2) return;
    std::vector<VkImage> images(count);
    if (vkGetSwapchainImagesKHR((VkDevice)vk_device_, swapchain, &count, images.data()) != VK_SUCCESS) return;
    data->swapchain = swapchain;
    data->image_count = count;

    data->image_views.resize(count);
    data->framebuffers.resize(count);
    auto& views = data->image_views;
    auto& fbs = data->framebuffers;

    hook_window_for(find_game_window());
    uint32_t w = vk_width_, h = vk_height_;

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

    auto& cp = data->command_pool;
    if (vkCreateCommandPool((VkDevice)vk_device_, &cp_info, nullptr, &cp) != VK_SUCCESS) return;

    data->command_buffers.resize(count);
    auto& cbs = data->command_buffers;
    VkCommandBufferAllocateInfo cb_info = {};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.commandPool = cp;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = count;
    if (vkAllocateCommandBuffers((VkDevice)vk_device_, &cb_info, cbs.data()) != VK_SUCCESS) {
        cbs.clear(); return;
    }
    data->fences.resize(count);
    data->present_ready.resize(count);
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; ++i) {
        if (vkCreateFence(data->device, &fence_info, nullptr, &data->fences[i]) != VK_SUCCESS ||
            vkCreateSemaphore(data->device, &sem_info, nullptr, &data->present_ready[i]) != VK_SUCCESS) return;
    }

    ImGui::CreateContext();
    if (!ImGui_ImplWin32_Init(hwnd_)) { ImGui::DestroyContext(); return; }
    hook_window();

    if (!ImGui_ImplVulkan_LoadFunctions(&ImGuiVulkanLoader, vulkan)) {
        ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); return;
    }

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = (VkInstance)vk_instance_;
    init_info.PhysicalDevice = (VkPhysicalDevice)vk_physical_device_;
    init_info.Device = (VkDevice)vk_device_;
    init_info.QueueFamily = vk_queue_family_;
    init_info.Queue = (VkQueue)queue;
    init_info.DescriptorPool = dp;
    init_info.RenderPass = rp;
    init_info.MinImageCount = std::max(2u, std::min(vk_min_image_count_, count));
    init_info.ImageCount = count;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (ImGui_ImplVulkan_Init(&init_info)) {
        style_.setup();
        imgui_initialized_ = true;
        active_api_ = GraphicsAPI::Vulkan;

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

        vk_data_ = owner.release();
        STAR_LOG("ImGui ready (Vulkan native) images=%u extent=%ux%u", count, w, h);
    } else {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

void StarOverlay::maybe_capture_vulkan(void* queue, const void* pPresentInfo)
{
    if (!screenshots_.consume()) return;
    if (!enabled_ || !imgui_initialized_ || active_api_ != GraphicsAPI::Vulkan) return;
    if (!vk_device_ || !vk_physical_device_) return;
    auto* pi = static_cast<const VkPresentInfoKHR*>(pPresentInfo);
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

    if (!(vk_image_usage_ & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        capture_desktop_duplication();
        return;
    }
    uint32_t w = vk_width_, h = vk_height_;
    VkFormat fmt = (VkFormat)vk_swapchain_format_;
    bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    if (fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_SRGB && !bgra) {
        STAR_LOG("Screenshot: unsupported Vulkan format %d", (int)fmt);
        return;
    }
    auto* data = (VulkanOverlayData*)vk_data_;
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
            VkResult completed = vkQueueSubmit((VkQueue)queue, 1, &submit, fence);
            if (completed == VK_SUCCESS)
                completed = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
            void* mapped = nullptr;
            if (completed == VK_SUCCESS && vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped) {
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
    vkDestroyBuffer(dev, staging, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    if (shot_ok) notify_screenshot(shot_path);
}

void StarOverlay::render_frame_vulkan(void* queue, const void* pPresentInfo)
{
    auto* data = (VulkanOverlayData*)vk_data_;
    auto* pi = const_cast<VkPresentInfoKHR*>(static_cast<const VkPresentInfoKHR*>(pPresentInfo));
    if (!data || data->failed || pi->pSwapchains[0] != data->swapchain) return;
    uint32_t image_index = pi->pImageIndices[0];
    if (image_index >= data->image_count) return;
    uint32_t frame = data->next_frame;
    VkFence fence = data->fences[frame];
    if (vkWaitForFences(data->device, 1, &fence, VK_TRUE, 1000000000ULL) != VK_SUCCESS) return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2((float)data->extent.width, (float)data->extent.height);
    ImGui::NewFrame();
    apply_cursor_mode();
    build_frame_ui();

    VkCommandBuffer cb = data->command_buffers[frame];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(cb, 0) != VK_SUCCESS || vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) return;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = data->render_pass;
    rp.framebuffer = data->framebuffers[image_index];
    rp.renderArea.extent = data->extent;
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { data->failed = true; return; }

    std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = pi->waitSemaphoreCount;
    submit.pWaitSemaphores = pi->pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &data->present_ready[image_index];
    if (vkResetFences(data->device, 1, &fence) != VK_SUCCESS ||
        vkQueueSubmit((VkQueue)queue, 1, &submit, fence) != VK_SUCCESS) {
        data->failed = true;
        STAR_LOG("Vulkan overlay submission failed");
        return;
    }
    // Consume the game's waits exactly once. Present waits on our completion.
    // The semaphore is indexed by acquired image, not CPU frame number.
    pi->waitSemaphoreCount = 1;
    pi->pWaitSemaphores = &data->present_ready[image_index];
    data->next_frame = (frame + 1) % data->image_count;
    maybe_capture_vulkan(queue, pPresentInfo);
}

ImTextureID StarOverlay::upload_icon_vulkan(const std::vector<uint8_t>& rgba, int w, int h)
{
    auto* data = (VulkanOverlayData*)vk_data_;
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
    if (vkMapMemory(dev, staging_mem, 0, buf_req.size, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(dev, staging, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        return nullptr;
    }
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
        vkDestroyBuffer(dev, staging, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr); return nullptr;
    }

    VkMemoryRequirements img_req{};
    vkGetImageMemoryRequirements(dev, image, &img_req);
    uint32_t img_mtype = vk_find_memory_type((VkPhysicalDevice)vk_physical_device_, img_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_mtype == UINT32_MAX) {
        vkDestroyImage(dev, image, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr); return nullptr;
    }
    VkMemoryAllocateInfo img_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    img_alloc.allocationSize  = img_req.size;
    img_alloc.memoryTypeIndex = img_mtype;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &img_alloc, nullptr, &img_mem) != VK_SUCCESS) {
        vkDestroyImage(dev, image, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        return nullptr;
    }
    vkBindImageMemory(dev, image, img_mem, 0);

    VkCommandBufferAllocateInfo cb_alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cb_alloc.commandPool        = pool;
    cb_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &cb_alloc, &cb) != VK_SUCCESS) {
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
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
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        return nullptr;
    }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    if (vkQueueSubmit(que, 1, &submit, fence) != VK_SUCCESS) {
        vkDestroyFence(dev, fence, nullptr);
        vkFreeCommandBuffers(dev, pool, 1, &cb);
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        return nullptr;
    }
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    vkDestroyBuffer(dev, staging, nullptr);
    vkFreeMemory(dev, staging_mem, nullptr);

    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view_info.image            = image;
    view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format           = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &view_info, nullptr, &view) != VK_SUCCESS) {
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, img_mem, nullptr);
        return nullptr;
    }

    data->icon_images.push_back(image);
    data->icon_memories.push_back(img_mem);
    data->icon_views.push_back(view);

    return reinterpret_cast<ImTextureID>(ImGui_ImplVulkan_AddTexture(data->icon_sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

void StarOverlay::cleanup_vulkan()
{
    auto* data = (VulkanOverlayData*)vk_data_;
    if (!data) return;
    vkDeviceWaitIdle(data->device);
    if (imgui_initialized_ && active_api_ == GraphicsAPI::Vulkan) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_initialized_ = false;
        active_api_ = GraphicsAPI::None;
        icons_.clear();
    }
    data->cleanup();
    delete data;
    vk_data_ = nullptr;
}
