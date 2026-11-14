
#include "debug_window.h"
#include "window_sizes.h"
#include "memory_map.h"
#include "../game_config.h"
#include <functional>
#include <string>

class MemBrowserWindow : public DebugWindow {
private:
    static constexpr const char* SIZE_NAME = "mem_browser";
    static constexpr const char* TOP_ROW_KEY = "top_row";

    MemoryMap*& memorymap;
    const std::function<uint8_t(uint16_t, bool)> mem_read;
    const std::function<uint8_t*(uint16_t)> ram_read;
    bool decimal = false;
    GameConfig &gameconfig;
    // ShowMemory's range, highlighted in the Explorer tab until the next one.
    bool has_highlight = false;
    uint16_t highlight_start = 0;
    uint32_t highlight_size = 0;
    bool scroll_to_highlight = false; // on the next frame, which also switches to the Explorer tab
    // The goto box: an address in hex, or a symbol from the memory map.
    char goto_text[64] = { 0 };
    bool goto_failed = false;
    // The Explorer tab's selection, for copying: the byte clicked first and the one it reaches
    // to (either way round), and whether a drag is extending it.
    bool has_selection = false;
    uint16_t selection_anchor = 0;
    uint16_t selection_end = 0;
    bool selecting = false;
    // The Explorer row scrolled to the top, kept between runs (WindowSizes): the one to scroll
    // back to when the window opens (-1 once done), and the last one saved.
    int row_to_restore = -1;
    int saved_top_row = -1;
    int top_row = -1;
    uint32_t top_row_saved_ticks = 0;
    // Saves the top row, at most every so often while scrolling (it writes a file).
    void SaveTopRow(bool now);

    void Goto();
    // The selected bytes, as "aa bb cc".
    std::string SelectionText();
    void RenderExplorer();
    // The status bar under the Explorer grid, about the byte under the mouse (-1: none).
    void RenderStatusBar(int hovered_address);
protected:
    ImVec2 Render();
public:
    MemBrowserWindow(MemoryMap*& map, std::function<uint8_t(uint16_t, bool)> reader,
    std::function<uint8_t*(uint16_t)> ram_read, GameConfig &gameconfig):
        DebugWindow("Memory Browser", SIZE_NAME),
        memorymap(map),
        mem_read(reader),
        ram_read(ram_read),
        gameconfig(gameconfig) {
        if(WindowSizes::LoadValue(SIZE_NAME, TOP_ROW_KEY, saved_top_row)) {
            row_to_restore = saved_top_row;
        }
    };
    ~MemBrowserWindow();
    // Scrolls the Explorer tab to size bytes at address, and highlights them.
    void ShowMemory(uint16_t address, uint32_t size);
};