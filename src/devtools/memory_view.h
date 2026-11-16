#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "imgui.h"
#include "memory_map.h"
#include "variable_map.h"

// A view of the 64K address space as a hex grid: the memory browser's Explorer tab, and the
// debugger's memory pane. It has a goto box (an address or a symbol; Ctrl+G), selection and
// copying ("aa bb cc"; Ctrl+C), a tooltip naming the hovered byte's variable, a status bar, a
// highlight for memory something asked to show, and keeps its scroll position between runs.
// Each view has its own of all of these.
class MemoryView {
public:
    // saveName and topRowKey: where its scroll position is kept (Settings).
    MemoryView(MemoryMap*& memorymap, std::function<uint8_t(uint16_t, bool)> mem_read, const char* saveName,
        const char* topRowKey);
    ~MemoryView();
    MemoryView(const MemoryView&) = delete;
    MemoryView& operator=(const MemoryView&) = delete;

    // Draws it in the space left in the current window or child.
    void Render();
    // Scrolls to size bytes at address, and highlights them.
    void ShowMemory(uint16_t address, uint32_t size);
    // Whether a ShowMemory is waiting to be scrolled to (so the view should be made visible).
    bool IsShowPending() const { return scroll_to_highlight; }
    // The width the grid needs, in the current ImGui context's style.
    static float PreferredWidth();

private:
    MemoryMap*& memorymap;
    const std::function<uint8_t(uint16_t, bool)> mem_read;
    const std::string save_name;
    const std::string top_row_key;

    // ShowMemory's range, highlighted until the next one.
    bool has_highlight = false;
    uint16_t highlight_start = 0;
    uint32_t highlight_size = 0;
    bool scroll_to_highlight = false;
    // The goto box: an address in hex, or a symbol from the memory map.
    char goto_text[64] = { 0 };
    bool goto_failed = false;
    // The selection, for copying: the byte clicked first and the one it reaches to (either way
    // round), and whether a drag is extending it.
    bool has_selection = false;
    uint16_t selection_anchor = 0;
    uint16_t selection_end = 0;
    bool selecting = false;
    // The row scrolled to the top, kept between runs: the one to scroll back to when it opens
    // (-1 once done), and the last one saved.
    int row_to_restore = -1;
    int saved_top_row = -1;
    int top_row = -1;
    uint32_t top_row_saved_ticks = 0;
    // The variable the hovered byte is in, found while drawing its tooltip; the grid tints that
    // struct and the byte's part of it on the next frame.
    bool hover_owner_shown = false;
    VariableMap::MemoryOwner hover_owner;

    // Saves the top row, at most every so often while scrolling (it writes a file).
    void SaveTopRow(bool now);
    void Goto();
    // The selected bytes, as "aa bb cc".
    std::string SelectionText();
    void RenderGrid(int& hovered_address);
    // The tooltip about the hovered byte's variable (-1: none).
    void RenderOwnerTooltip(int hovered_address);
    // The status bar under the grid, about the hovered byte (-1: none).
    void RenderStatusBar(int hovered_address);
};
