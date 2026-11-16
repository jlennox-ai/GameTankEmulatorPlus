#include "memory_view.h"
#include <algorithm>
#include <cstdio>
#include "../SDL_inc.h"
#include "settings.h"

#define ROW_BYTES 16
#define ROWS 4096
// Where the highlighted row lands when scrolled to: a little above the middle.
#define HIGHLIGHT_SCROLL_RATIO 0.3f
#define TOP_ROW_SAVE_INTERVAL_MS 1000

static const char* column_headers[ROW_BYTES + 1] = {
    " ",
    " 0", " 1", " 2", " 3", " 4", " 5", " 6", " 7",
    " 8", " 9", " A", " B", " C", " D", " E", " F"
};
static const float address_column_width = 48;

static const ImU32 highlight_color = IM_COL32(200, 150, 40, 110);
// The hovered byte's struct, and its own part of it, in the grid; the part's lines in the tooltip.
static const ImU32 hover_container_color = IM_COL32(70, 120, 200, 70);
static const ImU32 hover_part_color = IM_COL32(70, 120, 200, 170);
static const ImVec4 hover_part_text_color = ImVec4(1, 0.85f, 0.3f, 1);
static const ImVec4 goto_failed_color = ImVec4(1, 0.4f, 0.4f, 1);

MemoryView::MemoryView(MemoryMap*& memorymap, std::function<uint8_t(uint16_t, bool)> mem_read, const char* saveName,
    const char* topRowKey) :
    memorymap(memorymap),
    mem_read(mem_read),
    save_name(saveName),
    top_row_key(topRowKey) {
    if(Settings::LoadValue(save_name, top_row_key.c_str(), saved_top_row)) {
        row_to_restore = saved_top_row;
    }
}

MemoryView::~MemoryView() {
    SaveTopRow(true);
}

float MemoryView::PreferredWidth() {
    const ImGuiStyle& style = ImGui::GetStyle();
    float byte_column = ImGui::CalcTextSize("00").x + (style.CellPadding.x * 2);
    return address_column_width + (ROW_BYTES * byte_column) + style.ScrollbarSize + (style.WindowPadding.x * 2)
        + ImGui::CalcTextSize("00").x;
}

void MemoryView::SaveTopRow(bool now) {
    if((top_row < 0) || (top_row == saved_top_row)) {
        return;
    }
    if(!now && (SDL_GetTicks() - top_row_saved_ticks < TOP_ROW_SAVE_INTERVAL_MS)) {
        return;
    }
    Settings::SaveValue(save_name, top_row_key.c_str(), top_row);
    saved_top_row = top_row;
    top_row_saved_ticks = SDL_GetTicks();
}

void MemoryView::ShowMemory(uint16_t address, uint32_t size) {
    has_highlight = true;
    highlight_start = address;
    highlight_size = (size == 0) ? 1 : size;
    scroll_to_highlight = true;
}

void MemoryView::Goto() {
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

std::string MemoryView::SelectionText() {
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

void MemoryView::Render() {
    const ImGuiIO& io = ImGui::GetIO();
    // The view's shortcuts work while it (not just the window it's in) has the focus.
    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    // Ctrl+G: to the goto box, its text selected to type over.
    if(focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
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
        ImGui::TextColored(goto_failed_color, "not an address or symbol");
    }

    int hovered_address = -1;
    RenderGrid(hovered_address);

    // Not while the goto box is taking the keys.
    bool copy_pressed = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !io.WantTextInput;
    if(focused && has_selection && copy_pressed) {
        ImGui::SetClipboardText(SelectionText().c_str());
    }
    RenderOwnerTooltip(hovered_address);
    RenderStatusBar(hovered_address);
}

void MemoryView::RenderGrid(int& hovered_address) {
    const ImGuiIO& io = ImGui::GetIO();
    bool open_context_menu = false;
    uint16_t selection_low = std::min(selection_anchor, selection_end);
    uint16_t selection_high = std::max(selection_anchor, selection_end);
    // Fills the space left, but a row for the status bar; scrolls inside it, which also keeps the
    // frozen header row in view.
    ImVec2 grid_size = ImVec2(0, -ImGui::GetFrameHeightWithSpacing());
    ImGuiTableFlags grid_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
    if(ImGui::BeginTable("memtable", ROW_BYTES + 1, grid_flags, grid_size)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(column_headers[0], ImGuiTableColumnFlags_None, address_column_width);
        for(int i = 1; i <= ROW_BYTES; ++i) {
            ImGui::TableSetupColumn(column_headers[i], ImGuiTableColumnFlags_None);
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

        int highlight_row = highlight_start / ROW_BYTES;
        ImGuiListClipper clipper;
        clipper.Begin(ROWS);
        if(scroll_to_highlight) {
            // Rows out of view aren't drawn, and there'd be nothing to scroll to.
            clipper.IncludeItemByIndex(highlight_row);
        }
        while(clipper.Step()) {
            for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%04x", row * ROW_BYTES);
                for(int column = 0; column < ROW_BYTES; column++) {
                    ImGui::TableSetColumnIndex(column + 1);
                    uint16_t address = (uint16_t) ((row * ROW_BYTES) + column);
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
}

void MemoryView::RenderOwnerTooltip(int hovered_address) {
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
void MemoryView::RenderStatusBar(int hovered_address) {
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
