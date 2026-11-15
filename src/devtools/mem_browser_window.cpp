#include "imgui.h"
#include "mem_browser_window.h"
#include "variable_map.h"
#include "../tinyfd/tinyfiledialogs.h"

#include "./imgui-combo-filter.h"
#include <algorithm>
#include <cstdio>

static const char* hex_headers[17] = {
    " ",
    " 0", " 1", " 2", " 3", " 4", " 5", " 6", " 7",
    " 8", " 9", " A", " B", " C", " D", " E", " F"
};

static const char* var_headers[3] = {
    "Address",
    "Name",
    "Value"
};

static char const * mapFilterPatterns[1] = {"*.map"};

// Each table is the last item in its tab: with ScrollY, a (0, 0) size fills the rest of the
// window and scrolls inside it (which also keeps the frozen header row in view).
static const ImGuiTableFlags table_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
static const ImVec2 table_fill_size = ImVec2(0, 0);

#define EXPLORER_ROW_BYTES 16
#define EXPLORER_ROWS 4096
// Where the highlighted row lands when scrolled to: a little above the middle.
#define HIGHLIGHT_SCROLL_RATIO 0.3f
static const ImU32 highlight_color = IM_COL32(200, 150, 40, 110);
// The hovered byte's struct, and its own part of it, in the grid; the part's lines in the tooltip.
static const ImU32 hover_container_color = IM_COL32(70, 120, 200, 70);
static const ImU32 hover_part_color = IM_COL32(70, 120, 200, 170);
static const ImVec4 hover_part_text_color = ImVec4(1, 0.85f, 0.3f, 1);

#define TOP_ROW_SAVE_INTERVAL_MS 1000

MemBrowserWindow::~MemBrowserWindow() {
    SaveTopRow(true);
}

void MemBrowserWindow::SaveTopRow(bool now) {
    if((top_row < 0) || (top_row == saved_top_row)) {
        return;
    }
    if(!now && (SDL_GetTicks() - top_row_saved_ticks < TOP_ROW_SAVE_INTERVAL_MS)) {
        return;
    }
    WindowSizes::SaveValue(SIZE_NAME, TOP_ROW_KEY, top_row);
    saved_top_row = top_row;
    top_row_saved_ticks = SDL_GetTicks();
}

void MemBrowserWindow::ShowMemory(uint16_t address, uint32_t size) {
    has_highlight = true;
    highlight_start = address;
    highlight_size = (size == 0) ? 1 : size;
    scroll_to_highlight = true;
}

void MemBrowserWindow::Goto() {
    std::string text = goto_text;
    size_t first = text.find_first_not_of(" \t");
    size_t last = text.find_last_not_of(" \t");
    if(first == std::string::npos) {
        return;
    }
    text = text.substr(first, last - first + 1);
    uint16_t address;
    if((memorymap != nullptr) && memorymap->FindName(address, text)) {
        goto_failed = false;
        ShowMemory(address, 1);
        return;
    }
    std::string digits = text;
    if(digits[0] == '$') {
        digits = digits.substr(1);
    } else if((digits.size() > 2) && (digits[0] == '0') && ((digits[1] == 'x') || (digits[1] == 'X'))) {
        digits = digits.substr(2);
    }
    bool all_hex = !digits.empty() && (digits.size() <= 4)
        && (digits.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos);
    goto_failed = !all_hex;
    if(all_hex) {
        ShowMemory((uint16_t) std::stoul(digits, nullptr, 16), 1);
    }
}

std::string MemBrowserWindow::SelectionText() {
    uint16_t low = std::min(selection_anchor, selection_end);
    uint16_t high = std::max(selection_anchor, selection_end);
    std::string text;
    for(uint32_t address = low; address <= high; ++address) {
        char byte[4];
        std::snprintf(byte, sizeof(byte), "%02x", mem_read((uint16_t) address, false));
        if(!text.empty()) {
            text += ' ';
        }
        text += byte;
    }
    return text;
}

void MemBrowserWindow::RenderExplorer() {
    const ImGuiIO& io = ImGui::GetIO();
    // Ctrl+G: to the goto box, its text selected to type over.
    if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        ImGui::SetKeyboardFocusHere();
    }
    const char* goto_hint = "address or symbol (Ctrl+G)";
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(goto_hint).x + (ImGui::GetStyle().FramePadding.x * 2));
    bool entered = ImGui::InputTextWithHint("##goto", goto_hint, goto_text, sizeof(goto_text),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    if(ImGui::Button("Go") || entered) {
        Goto();
    }
    if(goto_failed) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "not an address or symbol");
    }

    int hovered_address = -1;
    bool open_context_menu = false;
    uint16_t selection_low = std::min(selection_anchor, selection_end);
    uint16_t selection_high = std::max(selection_anchor, selection_end);
    // Leaves a row under the grid for the status bar.
    ImVec2 grid_size = ImVec2(0, -ImGui::GetFrameHeightWithSpacing());
    if(ImGui::BeginTable("memtable", 17, table_flags | ImGuiTableFlags_RowBg, grid_size)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(hex_headers[0], ImGuiTableColumnFlags_None, 48);
        for(int i = 1; i < 17; ++i) {
            ImGui::TableSetupColumn(hex_headers[i], ImGuiTableColumnFlags_None);
        }
        ImGui::TableHeadersRow();

        // A row is a line of text and its cell padding.
        float row_height = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2);
        // Back to where it was last time, once the grid has a height to scroll in (not on the
        // first frame), unless something asked to show memory.
        if((row_to_restore >= 0) && (scroll_to_highlight || (ImGui::GetScrollMaxY() > 0))) {
            if(!scroll_to_highlight) {
                ImGui::SetScrollY(row_to_restore * row_height);
            }
            row_to_restore = -1;
        } else if(row_to_restore < 0) {
            top_row = (int) (ImGui::GetScrollY() / row_height);
            SaveTopRow(false);
        }

        int highlight_row = highlight_start / EXPLORER_ROW_BYTES;
        ImGuiListClipper clipper;
        clipper.Begin(EXPLORER_ROWS);
        if(scroll_to_highlight) {
            // Rows out of view aren't drawn, and there'd be nothing to scroll to.
            clipper.IncludeItemByIndex(highlight_row);
        }
        while(clipper.Step()) {
            for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%04x", row * EXPLORER_ROW_BYTES);
                for(int column = 0; column < EXPLORER_ROW_BYTES; column++) {
                    ImGui::TableSetColumnIndex(column + 1);
                    uint16_t address = (uint16_t) ((row * EXPLORER_ROW_BYTES) + column);
                    if(has_highlight && (address >= highlight_start) && (address < highlight_start + highlight_size)) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, highlight_color);
                    }
                    // The hovered byte's struct, and its part of it (from the last frame's hover).
                    if(hover_owner_shown && (VariableMap::singleton != nullptr)) {
                        VariableMap::ByteRole role = VariableMap::singleton->RoleOf(hover_owner, address);
                        if(role == VariableMap::ByteRole::Part) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, hover_part_color);
                        } else if(role == VariableMap::ByteRole::Container) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, hover_container_color);
                        }
                    }
                    bool selected = has_selection && (address >= selection_low) && (address <= selection_high);
                    char label[16];
                    std::snprintf(label, sizeof(label), "%02x##%04x", mem_read(address, false), address);
                    ImGui::Selectable(label, selected);
                    // Hovered even while a drag holds the mouse on the byte it started on.
                    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                        hovered_address = address;
                        if(selecting) {
                            selection_end = address;
                        }
                    }
                    if(ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        if(io.KeyShift && has_selection) {
                            selection_end = address;
                        } else {
                            selection_anchor = address;
                            selection_end = address;
                            has_selection = true;
                        }
                        selecting = true;
                    }
                    if(ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        if(!selected) {
                            selection_anchor = address;
                            selection_end = address;
                            has_selection = true;
                        }
                        open_context_menu = true;
                    }
                }
                if(scroll_to_highlight && (row == highlight_row)) {
                    ImGui::SetScrollHereY(HIGHLIGHT_SCROLL_RATIO);
                    scroll_to_highlight = false;
                }
            }
        }
        ImGui::EndTable();
    }
    if(!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        selecting = false;
    }

    if(open_context_menu) {
        ImGui::OpenPopup("byte menu");
    }
    if(ImGui::BeginPopup("byte menu")) {
        if(ImGui::MenuItem("Copy", "Ctrl+C")) {
            ImGui::SetClipboardText(SelectionText().c_str());
        }
        ImGui::EndPopup();
    }
    // Not while the goto box is taking the keys.
    bool copy_pressed = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !io.WantTextInput;
    if(has_selection && copy_pressed && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        ImGui::SetClipboardText(SelectionText().c_str());
    }

    RenderOwnerTooltip(hovered_address);
    RenderStatusBar(hovered_address);
}

void MemBrowserWindow::RenderOwnerTooltip(int hovered_address) {
    hover_owner_shown = (hovered_address >= 0) && (VariableMap::singleton != nullptr)
        && VariableMap::singleton->FindOwner((uint16_t) hovered_address,
            [this](uint16_t address) { return mem_read(address, false); }, hover_owner);
    if(!hover_owner_shown) {
        return;
    }
    ImGui::BeginTooltip();
    // Titled by the struct, not the field, so the tooltip keeps its size moving across a struct
    // (the field is its highlighted line).
    ImGui::TextUnformatted(hover_owner.container.c_str());
    ImGui::TextDisabled("%s, %s", hover_owner.containerType.c_str(), hover_owner.containerWhere.c_str());
    ImGui::Separator();
    for(auto& line : hover_owner.lines) {
        if(line.second) {
            ImGui::TextColored(hover_part_text_color, "%s", line.first.c_str());
        } else {
            ImGui::TextUnformatted(line.first.c_str());
        }
    }
    ImGui::EndTooltip();
}

// Fixed-width panes, so one pane's text changing length doesn't move the ones after it.
void MemBrowserWindow::RenderStatusBar(int hovered_address) {
    ImGui::Separator();
    if(!ImGui::BeginTable("statusbar", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    ImGui::TableSetupColumn("address", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Address $0000").x);
    ImGui::TableSetupColumn("binary", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000_0000").x);
    ImGui::TableSetupColumn("selection", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    if(hovered_address >= 0) {
        ImGui::Text("Address $%04x", hovered_address);
    }
    ImGui::TableNextColumn();
    if(hovered_address >= 0) {
        uint8_t value = mem_read((uint16_t) hovered_address, false);
        char binary[10];
        for(int bit = 0; bit < 8; ++bit) {
            int position = (bit < 4) ? bit : bit + 1;
            binary[position] = (value & (0x80 >> bit)) ? '1' : '0';
        }
        binary[4] = '_';
        binary[9] = '\0';
        ImGui::TextUnformatted(binary);
    }
    ImGui::TableNextColumn();
    if(has_selection) {
        uint16_t low = std::min(selection_anchor, selection_end);
        uint16_t high = std::max(selection_anchor, selection_end);
        ImGui::Text("Selected $%04x-$%04x (%u bytes)", low, high, (unsigned int) (high - low + 1));
    }
    ImGui::EndTable();
}

ImVec2 MemBrowserWindow::Render() {
    ImVec2 sizeOut = {0, 0};
    // Fill the SDL window, whatever size it has been resized to.
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Mem Browser", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::BeginTabBar("memtabs", 0);
    if(ImGui::BeginTabItem("Explorer", nullptr, scroll_to_highlight ? ImGuiTabItemFlags_SetSelected : 0)) {
        RenderExplorer();
        ImGui::EndTabItem();
    }
    
    if(ImGui::BeginTabItem("Variables")) {
        if(memorymap == NULL) {
            ImGui::Text("Variables will go here if a cc65 linker map file is loaded.");
            if(ImGui::Button("Load")) {
                char* mapFileName = tinyfd_openFileDialog(
                    "Select a map file from a cc65 project",
                    "",
                    1,
                    mapFilterPatterns,
                    "LD65 map file",
                    0
                );
                if(mapFileName) {
                    memorymap = new MemoryMap(std::string(mapFileName));
                }
            }
        } else {
            static int selected_search_item = -1;
            bool searched = false;
            if(ImGui::ComboFilter("##variable names", selected_search_item, *memorymap, memory_map_getter, ImGuiComboFlags_HeightRegular )) {
                searched = true;
            }
            ImGui::Checkbox("Decimal", &decimal);
            if(ImGui::BeginTable("vartable",3, table_flags, table_fill_size)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(var_headers[0], ImGuiTableColumnFlags_None);
            ImGui::TableSetupColumn(var_headers[1], ImGuiTableColumnFlags_None);
            ImGui::TableSetupColumn(var_headers[2], ImGuiTableColumnFlags_None, 100);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(memorymap->GetCount());
            if(selected_search_item != -1) {
                clipper.IncludeItemByIndex(selected_search_item);
            }
            while(clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                    {
                        ImGui::TableNextRow();
                        const Symbol& sym = memorymap->GetAt(row);
                        size_t size = (row < (memorymap->GetCount()-2)) ? memorymap->GetAt(row+1).address - sym.address : 1;
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%04x", sym.address);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s",sym.name.c_str());
                        ImGui::TableSetColumnIndex(2);                        
                        if(sym.address < 0x2000) {
                            ImGui::PushID(row);
                            ImGui::InputScalar("", (size == 1) ? ImGuiDataType_U8 : ImGuiDataType_U16, ram_read(sym.address), NULL, NULL, decimal ? "%d" : "%x", ImGuiInputTextFlags_CharsHexadecimal);
                            ImGui::PopID();
                        } else {
                            ImGui::Text(decimal ? "%d" : "%02x", mem_read(sym.address, false));
                        }
                        if(searched && (row == selected_search_item)) {
                            ImGui::ScrollToItem();
                        }
                    }
            }
            clipper.End();
            ImGui::EndTable();
        }
        }
        ImGui::EndTabItem();
    }

    if(ImGui::BeginTabItem("Watch")) {
        static int selected_item = -1;
        if(memorymap != NULL) {
            if(ImGui::ComboFilter("##variable names", selected_item, *memorymap, memory_map_getter, ImGuiComboFlags_HeightRegular )) {
                if(selected_item != -1) {
                    MemoryWatch watch;
                    watch.address = memorymap->GetAt(selected_item).address;
                    watch.name = memorymap->GetAt(selected_item).name;
                    watch.by_address = false;
                    watch.linked = true;
                    watch.linkFailed = false;
                    gameconfig.watch_locations.push_back(watch);
                    gameconfig.Save();
                }
            }

            if(ImGui::Button("Clear watches")) {
                ImGui::OpenPopup("clearwatches");
            }

            if(ImGui::BeginPopup("clearwatches")) {
                ImGui::Text("Are you sure?");
                if(ImGui::Button("Yes##clear")) {
                    gameconfig.watch_locations.clear();
                    gameconfig.Save();
                    ImGui::CloseCurrentPopup();
                }
                if(ImGui::Button("No##clear")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            
            ImGui::Checkbox("Decimal", &decimal);
            if(ImGui::BeginTable("watchtable",5, table_flags, table_fill_size)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(var_headers[0], ImGuiTableColumnFlags_None);
                ImGui::TableSetupColumn(var_headers[1], ImGuiTableColumnFlags_None);
                ImGui::TableSetupColumn(var_headers[2], ImGuiTableColumnFlags_None, 100);
                ImGui::TableSetupColumn("word", ImGuiTableColumnFlags_None, 32);
                ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_None, 32);
                ImGui::TableHeadersRow();

                if(gameconfig.watch_locations.size() != 0) {
                    int delete_item = -1;
                    int should_save = 0;
                    for (int row = 0; row < gameconfig.watch_locations.size(); row++)
                    {
                        ImGui::TableNextRow();
                        MemoryWatch& sym = gameconfig.watch_locations.at(row);

                        if((!sym.linked) && !(sym.linkFailed)) {
                            if(memorymap->FindName(sym.address, sym.name)) {
                                sym.linked = true;
                            } else {
                                sym.linkFailed = true;
                            }
                        }

                        if(sym.linkFailed) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(1, 0, 0, 1)));
                        }
                        ImGui::PushID(row);

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%04x", sym.address);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s",sym.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        if(sym.address < 0x2000) {
                            ImGui::InputScalar("##edit", (!sym.word) ? ImGuiDataType_U8 : ImGuiDataType_U16, ram_read(sym.address), NULL, NULL, decimal ? "%d" : "%x", ImGuiInputTextFlags_CharsHexadecimal);
                        } else {
                            ImGui::Text(decimal ? "%d" : "%02x", mem_read(sym.address, false));
                        }
                        ImGui::TableSetColumnIndex(3);
                        if(ImGui::Checkbox("##word", &(sym.word))) {
                            should_save = 1;
                        }
                        ImGui::TableSetColumnIndex(4);
                        if(ImGui::SmallButton("x##deleterow")) {
                            delete_item = row;
                            should_save = 1;
                        }
                        ImGui::PopID();
                    }
                    if(delete_item != -1) {
                        gameconfig.watch_locations.erase(std::next(gameconfig.watch_locations.begin(), delete_item));
                    }
                    if(should_save) {
                        gameconfig.Save();
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTabItem();
    }
    
    ImGui::EndTabBar();
    // The initial size; the window can be resized from there.
    sizeOut.x = 432;
    sizeOut.y = 800;
    ImGui::End();
    return sizeOut;
}