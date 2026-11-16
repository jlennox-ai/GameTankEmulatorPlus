#include "debug_window.h"

#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "../emulator_config.h"
#include "settings.h"

// How much of a window's top left corner must be on a monitor for its saved position to be used:
// enough to grab it. A position on a monitor that's since been unplugged isn't.
#define ON_SCREEN_MARGIN 32

static bool position_is_on_a_display(int x, int y) {
    SDL_Point corner = { x + ON_SCREEN_MARGIN, y + ON_SCREEN_MARGIN };
    for(int display = 0; display < SDL_GetNumVideoDisplays(); ++display) {
        SDL_Rect bounds;
        if((SDL_GetDisplayBounds(display, &bounds) == 0) && SDL_PointInRect(&corner, &bounds)) {
            return true;
        }
    }
    return false;
}

DebugWindow::DebugWindow(const char* title, const char* sizeName):
    BaseWindow(title, 10, 10, (sizeName == nullptr) ? 0 : SDL_WINDOW_RESIZABLE),
    size_name((sizeName == nullptr) ? "" : sizeName) {
    // Before any events, so a move event from opening can't save over the position first.
    int x;
    int y;
    if(!size_name.empty() && Settings::LoadPosition(size_name, x, y) && position_is_on_a_display(x, y)) {
        SDL_SetWindowPosition(window, x, y);
    }

    ImGuiContext* oldCtx = ImGui::GetCurrentContext();
    ImPlotContext* oldPlotCtx = ImPlot::GetCurrentContext();
    ctx = ImGui::CreateContext();
    plot_ctx = ImPlot::CreateContext();

    ImGui::SetCurrentContext(ctx);
    ImPlot::SetCurrentContext(plot_ctx);

    ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigViewportsNoDecoration = false;
	io.ConfigViewportsNoAutoMerge = true;
    io.IniFilename = NULL;
	ImGui::StyleColorsDark();

    renderer = SDL_CreateRenderer(window, -1, EmulatorConfig::defaultRendererFlags);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer2_Init(renderer);

    ImGui::SetCurrentContext(oldCtx);
    ImPlot::SetCurrentContext(oldPlotCtx);
}

void DebugWindow::Cleanup() {
    if(imgui_already_cleaned) {
        return;
    }
    ImGui::SetCurrentContext(ctx);
    ImPlot::DestroyContext(plot_ctx);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(ctx);
    SDL_DestroyRenderer(renderer);
    imgui_already_cleaned = true;
}

DebugWindow::~DebugWindow() {
    Cleanup();
}

void DebugWindow::Draw() {
    ImGui::SetCurrentContext(ctx);
    ImPlot::SetCurrentContext(plot_ctx);

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
    ImVec2 size = Render();

    if(size_dirty) {
        int width = (int) size.x;
        int height = (int) size.y;
        if(!size_name.empty()) {
            Settings::LoadSize(size_name, width, height);
        }
        SDL_SetWindowSize(window, width, height);
        size_dirty = false;
    }

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    ImGui::Render();
    ImGuiIO io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, (Uint8)(clear_color.x * 255), (Uint8)(clear_color.y * 255), (Uint8)(clear_color.z * 255), (Uint8)(clear_color.w * 255));
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
}

bool DebugWindow::IsTypingText() {
    if(imgui_already_cleaned) {
        return false;
    }
    ImGuiContext* oldCtx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    bool typing = ImGui::GetIO().WantTextInput;
    ImGui::SetCurrentContext(oldCtx);
    return typing;
}

void DebugWindow::HandleEvent(SDL_Event& e) {
    if(e.type == SDL_WINDOWEVENT) {
        if(e.window.event == SDL_WINDOWEVENT_CLOSE) {
            SDL_Window* closedWindow = SDL_GetWindowFromID(e.window.windowID);
            if(closedWindow == window) {
                open = false;
                Cleanup();
            }
        } else if((e.window.event == SDL_WINDOWEVENT_RESIZED) && !size_name.empty()
            && (e.window.windowID == SDL_GetWindowID(window))) {
            Settings::SaveSize(size_name, e.window.data1, e.window.data2);
        } else if((e.window.event == SDL_WINDOWEVENT_MOVED) && !size_name.empty()
            && (e.window.windowID == SDL_GetWindowID(window))) {
            Settings::SavePosition(size_name, e.window.data1, e.window.data2);
        }
    }

    if(SDL_GetMouseFocus() != window) return;
    ImGui::SetCurrentContext(ctx);
    ImPlot::SetCurrentContext(plot_ctx);
    ImGui_ImplSDL2_ProcessEvent(&e);
}