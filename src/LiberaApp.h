#pragma once

struct GLFWwindow;
struct ImFont;

struct LiberaAppConfig {
    const char* title = "Libera Link";
    int width = 1100;
    int height = 760;
    int windowX = -1;
    int windowY = -1;
};

struct LiberaApp {
    GLFWwindow* window = nullptr;
    float dpiScale = 1.0f;
    ImFont* fontBase = nullptr;
    ImFont* fontMedium = nullptr;
    ImFont* fontLarge = nullptr;

    bool init(const LiberaAppConfig& config);
    bool beginFrame();
    void endFrame();
    void shutdown();
    void getWindowGeometry(int& x, int& y, int& w, int& h) const;
};
