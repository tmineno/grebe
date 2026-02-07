#include "hud.h"
#include "vulkan_context.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

Hud::~Hud() {
    if (initialized_) {
        destroy();
    }
}

void Hud::init(GLFWwindow* window, VulkanContext& ctx, VkRenderPass render_pass,
               uint32_t image_count) {
    device_cache_ = ctx.device();

    // ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(8.0f, 4.0f);

    // GLFW backend
    ImGui_ImplGlfw_InitForVulkan(window, true);

    // Vulkan backend
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = ctx.instance();
    init_info.PhysicalDevice = ctx.physical_device();
    init_info.Device = ctx.device();
    init_info.QueueFamily = ctx.graphics_queue_family();
    init_info.Queue = ctx.graphics_queue();
    init_info.RenderPass = render_pass;
    init_info.MinImageCount = 2;
    init_info.ImageCount = image_count;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.DescriptorPoolSize = 10;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        throw std::runtime_error("Failed to init ImGui Vulkan backend");
    }

    // Upload font atlas
    ImGui_ImplVulkan_CreateFontsTexture();

    initialized_ = true;
    spdlog::info("HUD (ImGui) initialized");
}

void Hud::destroy() {
    if (!initialized_) return;

    vkDeviceWaitIdle(device_cache_);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    initialized_ = false;
    spdlog::info("HUD destroyed");
}

void Hud::on_swapchain_recreated(uint32_t min_image_count) {
    if (!initialized_) return;
    ImGui_ImplVulkan_SetMinImageCount(min_image_count);
}

void Hud::new_frame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Hud::build_status_bar(double fps, double frame_time_ms) {
    ImGuiIO& io = ImGui::GetIO();
    float bar_height = 28.0f;
    float screen_width = io.DisplaySize.x;
    float screen_height = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0.0f, screen_height - bar_height));
    ImGui::SetNextWindowSize(ImVec2(screen_width, bar_height));
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;

    ImGui::Begin("##statusbar", nullptr, flags);
    ImGui::Text("FPS: %.1f | Frame: %.2f ms", fps, frame_time_ms);
    ImGui::End();
}

void Hud::render(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}
