#pragma once
#include <string>
#include "../SDL_inc.h"
#include "imgui.h"
#include "implot.h"
#include "base_window.h"

class DebugWindow : public BaseWindow {
private:
    ImGuiContext* ctx;
    ImPlotContext* plot_ctx;
    bool imgui_already_cleaned = false;
    bool size_dirty = true;
    std::string size_name; // empty: a fixed size, not remembered
    void Cleanup();
protected:
    SDL_Renderer* renderer;
    virtual ImVec2 Render() = 0;
public:
    // Render() returns the window's size, which is applied once, on the first frame.
    // sizeName: makes the window resizable, and remembers the size and position it's left at
    // under that name (Settings): the next time, it opens there at that size instead of
    // Render()'s.
    DebugWindow(const char* title, const char* sizeName = nullptr);
    virtual ~DebugWindow();
    void Draw();
    void HandleEvent(SDL_Event& e);
    bool IsTypingText();
};