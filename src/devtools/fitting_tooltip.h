#pragma once
#include <algorithm>
#include "imgui.h"

// A tooltip kept inside the window it's shown in. ImGui puts a tooltip on whichever side of the
// mouse it fits, and when it fits on none, under the mouse even if that runs off the window. This
// one goes below and right of the mouse, or above or left of it where there isn't room, or against
// the window's edge (over the mouse) where there's room on neither side. Its lines wrap at the
// window's width, and it's no taller than the window (the rest is cut off).
class FittingTooltip {
public:
    void Begin() {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 position(Place(io.MousePos.x, last_size.x, io.DisplaySize.x, MOUSE_LEFT, MOUSE_RIGHT),
            Place(io.MousePos.y, last_size.y, io.DisplaySize.y, MOUSE_TOP, MOUSE_BOTTOM));
        ImGui::SetNextWindowPos(position);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), io.DisplaySize);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(io.DisplaySize.x - ImGui::GetStyle().WindowPadding.x);
    }

    void End() {
        last_size = ImGui::GetWindowSize();
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

private:
    // How far the tooltip keeps from the mouse on each side (the cursor's arrow is below and right
    // of its point), as ImGui's own do.
    static constexpr float MOUSE_LEFT = 16;
    static constexpr float MOUSE_RIGHT = 24;
    static constexpr float MOUSE_TOP = 8;
    static constexpr float MOUSE_BOTTOM = 24;

    // Its size when last shown: it fits itself to what it shows, so it's known only once shown.
    ImVec2 last_size = ImVec2(0, 0);

    // Where the tooltip starts on one axis: after the mouse, else before it, else at the window's end.
    static float Place(float mouse, float size, float display, float gapBefore, float gapAfter) {
        if(mouse + gapAfter + size <= display) {
            return mouse + gapAfter;
        }
        if(mouse - gapBefore - size >= 0) {
            return mouse - gapBefore - size;
        }
        return std::max(display - size, 0.0f);
    }
};
