#pragma once

#include "fonts/IconsForkAwesome.h"
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

// Toggle button — square power button icon with Libera Lab orange on-state styling.
// Pulses when showConnecting is true. Optional text label drawn beside the button.
// Returns true when toggled.
inline bool toggleButton(const char* label, bool* v, bool showConnecting = false,
                         float sizeOverride = 0.0f, const char* text = nullptr,
                         bool disabled = false) {
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

    if (disabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InvisibleButton("##toggle", ImVec2(totalW, frameH)) && !disabled) {
        *v = !*v;
        clicked = true;
        on = *v;
    }
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool active = ImGui::IsItemActive();
    if (disabled) {
        ImGui::EndDisabled();
    }

    const bool pulsingOn = showConnecting && ((static_cast<int>(ImGui::GetTime() * 4.0) % 2) == 0);
    const bool showPowered = on || pulsingOn;

    const ImU32 fillCol =
        disabled ? IM_COL32(42, 47, 54, 145)
                 : active ? IM_COL32(218, 145, 65, 255)
                          : showPowered ? IM_COL32(174, 84, 0, 255)
                                        : hovered ? IM_COL32(36, 42, 50, 255)
                                                  : IM_COL32(22, 28, 36, 235);
    const ImU32 borderCol =
        disabled ? IM_COL32(96, 104, 114, 120)
                 : showPowered ? IM_COL32(206, 115, 0, 255)
                               : hovered ? IM_COL32(130, 136, 144, 235)
                                         : IM_COL32(82, 98, 118, 220);
    const ImU32 iconCol =
        disabled ? IM_COL32(132, 136, 142, 180)
                 : showPowered ? IM_COL32(255, 255, 255, 255)
                               : hovered ? IM_COL32(220, 224, 228, 255)
                                         : IM_COL32(145, 161, 180, 235);

    const float rounding = 2.0f;
    const ImVec2 center(p.x + sz * 0.5f, p.y + sz * 0.5f);
    drawList->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), fillCol, rounding);
    drawList->AddRect(p, ImVec2(p.x + sz, p.y + sz), borderCol, rounding);

    ImFont* font = ImGui::GetFont();
    const float iconFontSize = sz * 0.7f;
    const char* icon = ICON_FK_POWER_OFF;
    const ImVec2 iconSize = font->CalcTextSizeA(iconFontSize, 1e9f, 0.0f, icon);
    const ImVec2 iconPos(
        center.x - iconSize.x * 0.5f + 0.45f,
        center.y - iconSize.y * 0.5f - sz * 0.02f + 1.2f);
    drawList->AddText(font, iconFontSize, iconPos, iconCol, icon);

    if (text) {
        float textX = p.x + sz + ImGui::GetStyle().ItemInnerSpacing.x;
        float textY = cursor.y + (frameH - ImGui::GetTextLineHeight()) * 0.5f;
        ImU32 textCol =
            disabled ? IM_COL32(132, 136, 142, 200)
                     : hovered ? IM_COL32(255, 255, 255, 255)
                               : IM_COL32(200, 200, 200, 255);
        drawList->AddText(ImVec2(textX, textY), textCol, text);
    }

    ImGui::PopID();
    return clicked;
}

}} // namespace libera::widgets
