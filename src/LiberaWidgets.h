#pragma once

#include "imgui.h"

namespace libera { namespace widgets {

// Status indicator square — colored square showing connection/health state.
// Colors: grey=disconnected, green=good, orange=issues, red=error.
// Returns true if clicked.
inline bool statusSquare(const char* id, ImU32 color, float size = 0.0f) {
    ImGui::PushID(id);
    float frameH = ImGui::GetFrameHeight();
    float sz = (size > 0.0f) ? size : frameH * 0.8f;
    float yPad = (frameH - sz) * 0.5f;

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 p(cursor.x, cursor.y + yPad);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), color, 2.0f);

    ImGui::InvisibleButton("##sq", ImVec2(sz, frameH));
    bool clicked = ImGui::IsItemClicked();
    ImGui::PopID();
    return clicked;
}

// Toggle button — blue square with inner white square when active.
// Blinks when showConnecting is true. Optional text label drawn beside the square.
// Returns true when toggled.
inline bool toggleButton(const char* label, bool* v, bool showConnecting = false,
                          float sizeOverride = 0.0f, const char* text = nullptr) {
    ImGui::PushID(label);
    bool clicked = false;
    bool on = *v;

    float frameH = ImGui::GetFrameHeight();
    float sz = (sizeOverride > 0) ? sizeOverride : frameH;
    float yPad = (frameH - sz) * 0.5f;

    float totalW = sz;
    if (text) {
        float textW = ImGui::CalcTextSize(text).x;
        totalW += ImGui::GetStyle().ItemInnerSpacing.x + textW;
    }

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 p(cursor.x, cursor.y + yPad);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    if (ImGui::InvisibleButton("##toggle", ImVec2(totalW, frameH))) {
        *v = !*v;
        clicked = true;
        on = *v;
    }
    bool hovered = ImGui::IsItemHovered();

    ImU32 outerCol = hovered ? IM_COL32(66, 150, 250, 255) : IM_COL32(66, 150, 250, 102);
    drawList->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), outerCol, 2.0f);

    float m = sz * 0.3f;
    bool showInner = false;
    if (showConnecting) {
        double t = ImGui::GetTime();
        showInner = (static_cast<int>(t * 4.0) % 2 == 0);
    } else if (on) {
        showInner = true;
    }
    if (showInner) {
        drawList->AddRectFilled(
            ImVec2(p.x + m, p.y + m), ImVec2(p.x + sz - m, p.y + sz - m),
            IM_COL32(255, 255, 255, 255), 1.0f);
    }

    if (text) {
        float textX = p.x + sz + ImGui::GetStyle().ItemInnerSpacing.x;
        float textY = cursor.y + (frameH - ImGui::GetTextLineHeight()) * 0.5f;
        ImU32 textCol = hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 200, 255);
        drawList->AddText(ImVec2(textX, textY), textCol, text);
    }

    ImGui::PopID();
    return clicked;
}

}} // namespace libera::widgets
