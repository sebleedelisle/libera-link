#include "LiberaApp.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "fonts/IconsForkAwesome.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <GLFW/glfw3.h>

#include <cstdio>

extern const unsigned int RobotoMedium_compressed_size;
extern const unsigned int RobotoMedium_compressed_data[];
extern const unsigned int RobotoBold_compressed_size;
extern const unsigned int RobotoBold_compressed_data[];
extern const unsigned int ForkAwesome_compressed_size;
extern const unsigned int ForkAwesome_compressed_data[];

bool LiberaApp::init(const LiberaAppConfig& config) {
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return false;
    }

#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    const char* glslVersion = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const char* glslVersion = "#version 130";
#endif

    window = glfwCreateWindow(config.width, config.height, config.title, nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return false;
    }
    if (config.windowX >= 0 && config.windowY >= 0) {
        glfwSetWindowPos(window, config.windowX, config.windowY);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr;

    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    dpiScale = xscale;

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;

    ImFontConfig mergeConfig;
    mergeConfig.MergeMode = true;
    mergeConfig.OversampleH = 2;
    mergeConfig.OversampleV = 2;

    static const ImWchar iconRanges[] = { ICON_MIN_FK, ICON_MAX_FK, 0 };

    io.Fonts->Clear();

    const float baseFontSize = 16.0f * dpiScale;
    const float mediumFontSize = 20.0f * dpiScale;
    const float largeFontSize = 28.0f * dpiScale;

    fontBase = io.Fonts->AddFontFromMemoryCompressedTTF(
        RobotoMedium_compressed_data, RobotoMedium_compressed_size, baseFontSize, &fontConfig);
    mergeConfig.GlyphMinAdvanceX = baseFontSize;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        ForkAwesome_compressed_data, ForkAwesome_compressed_size, baseFontSize, &mergeConfig, iconRanges);

    fontMedium = io.Fonts->AddFontFromMemoryCompressedTTF(
        RobotoBold_compressed_data, RobotoBold_compressed_size, mediumFontSize, &fontConfig);
    mergeConfig.GlyphMinAdvanceX = mediumFontSize;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        ForkAwesome_compressed_data, ForkAwesome_compressed_size, mediumFontSize, &mergeConfig, iconRanges);

    fontLarge = io.Fonts->AddFontFromMemoryCompressedTTF(
        RobotoBold_compressed_data, RobotoBold_compressed_size, largeFontSize, &fontConfig);
    mergeConfig.GlyphMinAdvanceX = largeFontSize;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        ForkAwesome_compressed_data, ForkAwesome_compressed_size, largeFontSize, &mergeConfig, iconRanges);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleMain = 1.0f / dpiScale;
    style.WindowRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildRounding = 6.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 6.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.FrameRounding = 3.0f;
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.CellPadding = ImVec2(10.0f, 8.0f);
    style.GrabRounding = 2.0f;
    style.ScrollbarSize = 12.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.18f, 0.22f, 0.27f, 0.95f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.16f, 0.24f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.23f, 0.35f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.42f, 0.68f, 0.85f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.50f, 0.80f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.38f, 0.62f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.19f, 0.33f, 0.52f, 0.80f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.42f, 0.65f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.34f, 0.58f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.28f, 0.41f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.25f, 0.31f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.58f, 0.62f, 0.68f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.12f, 0.17f, 0.23f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.25f, 0.31f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.18f, 0.23f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);
    return true;
}

bool LiberaApp::beginFrame() {
    if (glfwWindowShouldClose(window)) {
        return false;
    }

    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    return true;
}

void LiberaApp::endFrame() {
    ImGui::Render();

    int displayW = 0;
    int displayH = 0;
    glfwGetFramebufferSize(window, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.06f, 0.07f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backupContext = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backupContext);
    }

    glfwSwapBuffers(window);
}

void LiberaApp::shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (window) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

void LiberaApp::getWindowGeometry(int& x, int& y, int& w, int& h) const {
    glfwGetWindowSize(window, &w, &h);
    glfwGetWindowPos(window, &x, &y);
}
