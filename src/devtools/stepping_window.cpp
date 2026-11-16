#include "../imgui/imgui.h"
#include "stepping_window.h"
#include "breakpoints.h"
#include "disassembler.h"
#include "imgui-combo-filter.h"
#include "source_map.h"
#include "source_stepper.h"
#include "variable_map.h"
#include "settings.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#define SOURCE_TAB_WIDTH 4
#define RAM_END 0x2000

const char* source_file_getter(const std::vector<std::string> &items, int index) {
    if (index >= 0 && index < (int)items.size()) {
        return items[index].c_str();
    }
    return "N/A";
}

// Draws the clickable breakpoint margin in front of a source line. Returns true if a breakpoint was added or removed.
static bool source_breakpoint_margin(const std::string& file_name, int line) {
    float line_height = ImGui::GetTextLineHeight();
    bool clicked = ImGui::InvisibleButton("##breakpoint margin", ImVec2(line_height, line_height));
    bool hovered = ImGui::IsItemHovered();
    bool has_breakpoint = Breakpoints::hasBreakpointNamed(Breakpoints::sourceLineName(file_name, line));

    bool has_code = has_breakpoint;
    if(hovered && !has_breakpoint) {
        has_code = SourceMap::singleton->ReverseSearch(file_name, line).found;
    }

    ImVec2 rect_min = ImGui::GetItemRectMin();
    ImVec2 rect_max = ImGui::GetItemRectMax();
    ImVec2 center((rect_min.x + rect_max.x) * 0.5f, (rect_min.y + rect_max.y) * 0.5f);
    float radius = line_height * 0.3f;
    if(has_breakpoint) {
        ImGui::GetWindowDrawList()->AddCircleFilled(center, radius, IM_COL32(230, 40, 40, 255));
    } else if(hovered && has_code) {
        ImGui::GetWindowDrawList()->AddCircleFilled(center, radius, IM_COL32(230, 40, 40, 80));
    }

    if(hovered) {
        if(has_breakpoint) {
            ImGui::SetTooltip("Click to remove the breakpoint");
        } else if(!has_code) {
            ImGui::SetTooltip("No code on this line");
        } else if(Breakpoints::enabled) {
            ImGui::SetTooltip("Click to set a breakpoint");
        } else {
            ImGui::SetTooltip("Click to set a breakpoint (also turns on \"Enable breakpoints\")");
        }
    }

    if(!clicked) return false;
    return Breakpoints::toggleSourceBreakpoint(*SourceMap::singleton, file_name, line) != SourceBreakpointToggle::NoCode;
}

// An identifier in a line of source, and the variable expression it ends: for the X of
// "g_player.X", g_player then .X. The chain is empty when it can't be a variable: a function
// call, or a member of something that isn't a plain name ("g_well[y][x].Color").
struct SourceIdentifier {
    size_t start;
    size_t length;
    VariableMap::Chain chain;
};

static bool is_identifier_start(char character) {
    return std::isalpha((unsigned char) character) || (character == '_');
}

static bool is_identifier_char(char character) {
    return std::isalnum((unsigned char) character) || (character == '_');
}

// The identifiers in a line, outside comments, strings and character constants.
static std::vector<SourceIdentifier> source_identifiers(const std::string& text) {
    std::vector<SourceIdentifier> identifiers;
    char quote = 0;
    for(size_t i = 0; i < text.size(); ++i) {
        char character = text[i];
        if(quote) {
            if(character == '\\') {
                ++i;
            } else if(character == quote) {
                quote = 0;
            }
            continue;
        }
        if((character == '"') || (character == '\'')) {
            quote = character;
            continue;
        }
        if((character == '/') && (i + 1 < text.size()) && ((text[i + 1] == '/') || (text[i + 1] == '*'))) {
            break;
        }
        // Not the middle of a name or a number (0x10, 1.5f).
        if(!is_identifier_start(character) || ((i > 0) && is_identifier_char(text[i - 1]))) {
            continue;
        }
        size_t end = i;
        while((end < text.size()) && is_identifier_char(text[end])) {
            ++end;
        }
        SourceIdentifier identifier = { i, end - i, {} };
        std::string name = text.substr(i, end - i);

        size_t before = i;
        while((before > 0) && (text[before - 1] == ' ')) --before;
        char separator = 0;
        if((before >= 1) && (text[before - 1] == '.')) {
            separator = '.';
            before -= 1;
        } else if((before >= 2) && (text[before - 2] == '-') && (text[before - 1] == '>')) {
            separator = '>';
            before -= 2;
        }
        if(separator) {
            while((before > 0) && (text[before - 1] == ' ')) --before;
            // What the member is of: the name or index ending just before the separator (not
            // necessarily the last one found: an index's own names come after it).
            const SourceIdentifier* previous = nullptr;
            for(auto candidate = identifiers.rbegin(); candidate != identifiers.rend(); ++candidate) {
                if(candidate->start + candidate->length == before) {
                    previous = &*candidate;
                    break;
                }
            }
            if((previous != nullptr) && !previous->chain.empty()) {
                identifier.chain = previous->chain;
                identifier.chain.emplace_back(separator, name);
            }
        } else {
            identifier.chain.emplace_back('\0', name);
        }
        size_t after = end;
        while((after < text.size()) && (text[after] == ' ')) ++after;
        if((after < text.size()) && (text[after] == '(')) {
            identifier.chain.clear();
        }
        identifiers.push_back(identifier);

        // Indexes after it, foo[5][i]: each "[...]" is a span of its own, the element. The
        // names inside are scanned after, as their own spans.
        VariableMap::Chain indexed = identifier.chain;
        size_t open = after;
        while(!indexed.empty() && (open < text.size()) && (text[open] == '[')) {
            int depth = 0;
            size_t close = open;
            for(; close < text.size(); ++close) {
                if(text[close] == '[') {
                    ++depth;
                } else if((text[close] == ']') && (--depth == 0)) {
                    break;
                }
            }
            if(close >= text.size()) {
                break;
            }
            indexed.emplace_back('[', text.substr(open + 1, close - open - 1));
            identifiers.push_back({ open, close - open + 1, indexed });
            open = close + 1;
            while((open < text.size()) && (text[open] == ' ')) ++open;
        }
        i = end - 1;
    }
    return identifiers;
}

static std::string expand_tabs(const std::string& text) {
    std::string expanded;
    for(char character : text) {
        if(character == '\t') {
            expanded.append(SOURCE_TAB_WIDTH - (expanded.size() % SOURCE_TAB_WIDTH), ' ');
        } else {
            expanded += character;
        }
    }
    return expanded;
}

bool SteppingWindow::FindRamSymbol(const std::string& name, uint16_t& address) {
    if(memorymap == nullptr) {
        return false;
    }
    // cc65 prefixes C names with an underscore.
    bool found = memorymap->FindName(address, name) || memorymap->FindName(address, "_" + name);
    return found && (address < RAM_END);
}

bool SteppingWindow::CanDescribe(const VariableMap::Chain& chain) {
    if(chain.empty()) {
        return false;
    }
    if((VariableMap::singleton != nullptr) && VariableMap::singleton->IsVisible(chain[0].second, cpu->pc)) {
        // An index has to be something the debugger can work out (not a macro or an expression).
        VariableMap::Registers registers = { cpu->A, cpu->X, cpu->Y };
        for(auto& step : chain) {
            int64_t index;
            if((step.first == '[') && !VariableMap::singleton->ResolveIndex(step.second, cpu->pc, registers,
                [this](uint16_t address) { return mem_read(address, false); }, index)) {
                return false;
            }
        }
        return true;
    }
    uint16_t address;
    return (chain.size() == 1) && FindRamSymbol(chain[0].second, address);
}

std::string SteppingWindow::Describe(const VariableMap::Chain& chain) {
    if((VariableMap::singleton != nullptr) && VariableMap::singleton->IsVisible(chain[0].second, cpu->pc)) {
        VariableMap::Registers registers = { cpu->A, cpu->X, cpu->Y };
        return VariableMap::singleton->Describe(chain, cpu->pc, registers,
            [this](uint16_t address) { return mem_read(address, false); });
    }
    uint16_t address;
    if((chain.size() == 1) && FindRamSymbol(chain[0].second, address)) {
        char text[160];
        uint8_t value = mem_read(address, false);
        std::snprintf(text, sizeof(text), "%s = %u ($%02X)\nthe byte at $%04X (a symbol, no type information)",
            chain[0].second.c_str(), value, value, address);
        return text;
    }
    return "";
}

bool SteppingWindow::Locate(const VariableMap::Chain& chain, uint16_t& address, uint32_t& size) {
    if((VariableMap::singleton != nullptr) && VariableMap::singleton->IsVisible(chain[0].second, cpu->pc)) {
        VariableMap::Registers registers = { cpu->A, cpu->X, cpu->Y };
        return VariableMap::singleton->Locate(chain, cpu->pc, registers,
            [this](uint16_t readAddress) { return mem_read(readAddress, false); }, address, size);
    }
    if((chain.size() == 1) && FindRamSymbol(chain[0].second, address)) {
        size = 1;
        return true;
    }
    return false;
}

void SteppingWindow::RenderSourceLine(const std::string& text, ImVec4 color) {
    std::string shown = expand_tabs(text);
    ImGui::TextColored(color, "%s", shown.c_str());
    if(!ImGui::IsItemVisible()) {
        return;
    }
    ImVec2 text_min = ImGui::GetItemRectMin();
    ImVec2 text_max = ImGui::GetItemRectMax();
    bool hovered = ImGui::IsItemHovered();
    float mouse_x = ImGui::GetIO().MousePos.x;
    // The source view's font is fixed width.
    float character_width = ImGui::CalcTextSize("M").x;
    // Spans can nest (foo[i]: the index, and i inside it): the narrowest one under the mouse is
    // the one meant.
    const SourceIdentifier* under_mouse = nullptr;
    std::vector<SourceIdentifier> identifiers = source_identifiers(shown);
    for(auto& identifier : identifiers) {
        if(!CanDescribe(identifier.chain)) {
            continue;
        }
        float left = text_min.x + identifier.start * character_width;
        float right = left + identifier.length * character_width;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(left, text_max.y - 1), ImVec2(right, text_max.y - 1),
            IM_COL32(110, 160, 255, 200));
        bool mouse_in = hovered && (mouse_x >= left) && (mouse_x < right);
        if(mouse_in && ((under_mouse == nullptr) || (identifier.length < under_mouse->length))) {
            under_mouse = &identifier;
        }
    }
    if(under_mouse == nullptr) {
        return;
    }
    uint16_t address;
    uint32_t size;
    if(Locate(under_mouse->chain, address, size)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip(memory_pane_shown ? "%s\nClick to show it in the memory pane" : "%s\nClick to show it in the memory browser",
            Describe(under_mouse->chain).c_str());
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if(memory_pane_shown) {
                memory_pane.ShowMemory(address, size);
            } else {
                show_memory(address, size);
            }
        }
    } else {
        ImGui::SetTooltip("%s", Describe(under_mouse->chain).c_str());
    }
}

// The source line at an address. RAM can't be mapped to source (SourceMap::Search warns about it
// on every call), so a RAM address has none.
static SourceMapSearchResult search_source(uint16_t address, uint8_t bank) {
    if(address < 0x8000) {
        return SourceMapSearchResult{ false, nullptr, nullptr, 0 };
    }
    return SourceMap::singleton->Search(address, bank);
}

void SteppingWindow::ViewCode(uint16_t address, uint8_t bank) {
    viewing_code = true;
    view_address = address;
    view_bank = bank;
    last_scrolled_line = -1;
    if((SourceMap::singleton != nullptr) && search_source(address, bank).found) {
        tab_to_select = CodeTab::Source;
    } else {
        Disassembler::Decode(mem_read, memorymap, address, 32);
        tab_to_select = CodeTab::Disassembly;
    }
}

void SteppingWindow::ShowPcCode() {
    viewing_code = false;
    last_scrolled_line = -1;
    if(timekeeper.clock_mode == CLOCKMODE_STOPPED) {
        // Also after a stop that didn't decode it (a pause).
        Disassembler::Decode(mem_read, memorymap, cpu->pc, 32);
    }
}

void SteppingWindow::ToggleMemoryPane() {
    memory_pane_shown = !memory_pane_shown;
    Settings::SaveValue(SIZE_NAME, MEMORY_PANE_KEY, memory_pane_shown ? 1 : 0);
    // The window grows by the pane, so the debugger beside it keeps its size, and shrinks back.
    int pane_width = (int) (MemoryView::PreferredWidth() + ImGui::GetStyle().ItemSpacing.x);
    int width;
    int height;
    SDL_GetWindowSize(window, &width, &height);
    SDL_SetWindowSize(window, memory_pane_shown ? (width + pane_width) : std::max(width - pane_width, pane_width), height);
}

ImVec2 SteppingWindow::Render() {
     ImVec2 sizeOut = {0, 0};

    // Fill the SDL window, whatever size it has been resized to.
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Code Stepper", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    
    if(ImGui::Button("Step")) {
        timekeeper.clock_mode = CLOCKMODE_SINGLE;
    }
    ImGui::SameLine();
    // Needs a source map to know where lines start and end.
    ImGui::BeginDisabled(SourceMap::singleton == nullptr);
    if(ImGui::Button("Step src")) {
        SourceStepper::StartStepSource(cpu->pc, cartridgestate.bank_mask, cpu->sp);
        timekeeper.clock_mode = CLOCKMODE_STEPSOURCE;
    }
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Run to the next source line (into calls; interrupts run through)");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(SourceMap::singleton == nullptr);
    if(ImGui::Button("Step over")) {
        SourceStepper::StartStepOver(cpu->pc, cartridgestate.bank_mask, cpu->sp);
        timekeeper.clock_mode = CLOCKMODE_STEPSOURCE;
    }
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Run to the next source line, running calls through (interrupts too)");
    }
    ImGui::SameLine();
    if(ImGui::Button("Step out")) {
        SourceStepper::StartStepOut(cpu->sp);
        timekeeper.clock_mode = CLOCKMODE_STEPOUT;
    }
    if(ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Run until this function returns to its caller (interrupts run through)");
    }
    ImGui::SameLine();
    if(ImGui::Button("Continue")) {
        timekeeper.clock_mode = CLOCKMODE_NORMAL;
    }
    if((timekeeper.clock_mode == CLOCKMODE_STEPSOURCE) || (timekeeper.clock_mode == CLOCKMODE_STEPOUT)) {
        ImGui::SameLine();
        ImGui::Text("Stepping... (Continue or Step cancels)");
    }
    // The memory pane's button, at the right end of the row (never over the others), drawn
    // pressed while the pane is shown.
    const char* pane_label = "Memory";
    float pane_button_width = ImGui::CalcTextSize(pane_label).x + (ImGui::GetStyle().FramePadding.x * 2);
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - pane_button_width));
    bool pane_was_shown = memory_pane_shown;
    if(pane_was_shown) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if(ImGui::Button(pane_label)) {
        ToggleMemoryPane();
    }
    if(pane_was_shown) {
        ImGui::PopStyleColor();
    }
    if(ImGui::IsItemHovered()) {
        ImGui::SetTooltip(memory_pane_shown ? "Hide the memory pane" : "Show memory beside the code");
    }

    // With the memory pane, the rest of the debugger is the left pane.
    float pane_width = MemoryView::PreferredWidth();
    if(memory_pane_shown) {
        ImGui::BeginChild("debugger", ImVec2(-(pane_width + ImGui::GetStyle().ItemSpacing.x), 0));
    }

    ImGui::Separator();

    ImGui::Checkbox("Enable breakpoints", &Breakpoints::enabled);

    int selected_item = -1;
    if(memorymap != NULL) {
        if(ImGui::ComboFilter("##label names", selected_item, *memorymap, memory_map_getter, ImGuiComboFlags_HeightRegular )) {
            Symbol sym = memorymap->GetAt(selected_item);
            Breakpoint bp;
            bp.address = memorymap->GetAt(selected_item).address;
            bp.name = memorymap->GetAt(selected_item).name;
            bp.by_address = false;
            bp.bank_set = false;
            bp.enabled = true;
            bp.linked = true;
            bp.linkFailed = false;
            Breakpoints::breakpoints.push_back(bp);
            gameconfig.Save();
        }
    }

    if(ImGui::Button("Add by address")) {
        ImGui::OpenPopup("Add Manual Breakpoint");
    }

    if(ImGui::BeginPopup("Add Manual Breakpoint")) {
        static uint16_t manual_addr = 0;
        static uint8_t manual_bank = 0;
        static bool set_bank = false;
        ImGui::InputScalar("Address", ImGuiDataType_U16, &manual_addr, NULL, NULL, "%x", ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::InputScalar("Bank", ImGuiDataType_U8, &manual_bank, NULL, NULL, "%x", ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::Checkbox("Set bank", &set_bank);
        if(ImGui::Button("Add##manualbreak")) {
            Breakpoint bp;
            bp.name = std::string("N/A");
            bp.address = manual_addr;
            bp.by_address = true;
            bp.bank_set = set_bank;
            bp.bank = manual_bank;
            bp.enabled = true;
            bp.linked = true;
            bp.linkFailed = false;
            Breakpoints::breakpoints.push_back(bp);
            gameconfig.Save();
        }
        ImGui::EndPopup();
    }

    if(SourceMap::singleton) {
        static int selected_file_idx;
        static int32_t bp_line_num;
        if(ImGui::Button("Add C Breakpoint")) {
            ImGui::OpenPopup("Add C Breakpoint");
        }

        if(ImGui::BeginPopup("Add C Breakpoint")) {
            if(ImGui::ComboFilter("##cfile names", selected_file_idx, SourceMap::singleton->GetFileNames(), source_file_getter, ImGuiComboFlags_HeightRegular )) {
            }
            ImGui::InputScalar("Line", ImGuiDataType_S32, &bp_line_num, NULL, NULL, "%x", 0);
            if(selected_file_idx != -1) {
                if(ImGui::Button("Add##cfilebp")) {
                    auto searchResult = SourceMap::singleton->ReverseSearch(SourceMap::singleton->GetFileNames()[selected_file_idx], bp_line_num);
                    if(searchResult.found) {
                        Breakpoints::addSourceBreakpoint(*SourceMap::singleton, SourceMap::singleton->GetFileNames()[selected_file_idx], bp_line_num, searchResult.address, searchResult.bank);
                        gameconfig.Save();
                    }
                }
            }
            ImGui::EndPopup();   
        }
    }

    if(ImGui::Button("Clear breakpoints")) {
        ImGui::OpenPopup("clearbreakpoints");
    }

    if(ImGui::BeginPopup("clearbreakpoints")) {
        ImGui::Text("Are you sure?");
        if(ImGui::Button("Yes##clear")) {
            Breakpoints::breakpoints.clear();
            gameconfig.Save();
            ImGui::CloseCurrentPopup();
        }
        if(ImGui::Button("No##clear")) {
             ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    int index = 0;
    int delete_index = -1;
    bool should_save = false;
    // A table, so every row's checkbox and delete button line up: the name column is as wide as
    // the longest name.
    if(!Breakpoints::breakpoints.empty() && ImGui::BeginTable("breakpointtable", 3, ImGuiTableFlags_SizingFixedFit)) {
        for(auto& man : Breakpoints::breakpoints) {
            ImGui::PushID(index);
            auto color = man.linkFailed ? ImVec4(1, 0, 0, 1) : ImVec4(1, 1, 1, 1);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // Centers the name on the row's checkbox and button.
            ImGui::AlignTextToFramePadding();
            char location[16];
            if(man.bank_set) {
                snprintf(location, sizeof(location), "%04x:%02x", man.address, man.bank);
            } else {
                snprintf(location, sizeof(location), "%04x", man.address);
            }
            // Source line breakpoints are found by address too, but their file and line say more.
            bool show_name = !man.by_address || man.by_source_line;
            std::string label = show_name ? (man.name + "@" + location) : location;
            // A breakpoint whose name wasn't found has no address to show.
            ImGui::BeginDisabled(man.linkFailed);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if(ImGui::Selectable(label.c_str())) {
                ViewCode(man.address, man.bank_set ? man.bank : cartridgestate.bank_mask);
            }
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if(ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show this breakpoint's code");
            }
            ImGui::TableNextColumn();
            if(ImGui::Checkbox("##enable breakpoint", &man.enabled)) {
                should_save = true;
            }
            ImGui::TableNextColumn();
            if(ImGui::Button("x##delete breakpoint")) {
                delete_index = index;
                should_save = true;
            }
            ++index;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if(delete_index != -1) {
        Breakpoints::breakpoints.erase(std::next(Breakpoints::breakpoints.begin(), delete_index));
    }

    if(should_save) {
        gameconfig.Save();
    }


    ImGui::Separator();

    ImGui::Text("A:      %02x X:     %02x  Y:   %02x", cpu->A, cpu->X, cpu->Y);
    ImGui::Text("Status: %02x Stack: %02x PC: %04x", cpu->status, cpu->sp, cpu->pc);
    ImGui::Text("Cycles Since Boot: %lu", timekeeper.totalCyclesCount);
    ImGui::NewLine();

    bool stopped = (timekeeper.clock_mode == CLOCKMODE_STOPPED);
    if(stopped && !was_stopped) {
        // A new stop: show where it is, scrolling to it even if it's the line it last stopped on.
        ShowPcCode();
        // Until the first stop, the code tabs are empty, so that one picks the tab: Source when
        // there's source for it. After that, the tab you chose stays.
        if(!shown_a_stop) {
            shown_a_stop = true;
            if((SourceMap::singleton != nullptr) && search_source(cpu->pc, cartridgestate.bank_mask).found) {
                tab_to_select = CodeTab::Source;
            }
        }
    }
    was_stopped = stopped;

    if(viewing_code) {
        if(ImGui::Button("Back to PC")) {
            ShowPcCode();
        }
        ImGui::SameLine();
        ImGui::Text("Showing the breakpoint at %04x", view_address);
    }

    const ImVec4 pc_line_color = ImVec4(1, 1, 1, 1);
    const ImVec4 viewed_line_color = ImVec4(1, 0.85f, 0.3f, 1);
    const ImVec4 other_line_color = ImVec4(0.75f, 0.75f, 0.75f, 1);

    ImGui::BeginTabBar("codetabs", 0);
    if(ImGui::BeginTabItem("Disassembly", nullptr, (tab_to_select == CodeTab::Disassembly) ? ImGuiTabItemFlags_SetSelected : 0)) {
        if(stopped || viewing_code) {
            for(auto& line : Disassembler::GetLastDecode()) {
                if(line.isLabel) {
                    ImGui::Text("%s", line.disassembledLine.c_str());
                } else {
                    bool viewed = viewing_code && (line.address == view_address);
                    ImGui::TextColored(viewed ? viewed_line_color : pc_line_color, "%04x %s", line.address,
                        line.disassembledLine.c_str());
                }
            }
        }
        ImGui::EndTabItem();
    }
    if(ImGui::BeginTabItem("Source", nullptr, (tab_to_select == CodeTab::Source) ? ImGuiTabItemFlags_SetSelected : 0)) {
        if(stopped || viewing_code) {
            uint16_t address = viewing_code ? view_address : cpu->pc;
            uint8_t bank = viewing_code ? view_bank : cartridgestate.bank_mask;
            if(SourceMap::singleton) {
                SourceMapSearchResult res = search_source(address, bank);
                if(res.found) {
                    SourceMap::singleton->GetFileContent(*res.file);
                    ImGui::Text("%s:%d", res.file->name.c_str(), res.line->line);
                    if(res.file->contents_cached) {
                        const int shown_line = res.line->line;
                        // The PC's line is marked too, when it's in the file shown.
                        int pc_line = viewing_code ? -1 : shown_line;
                        if(viewing_code && stopped) {
                            SourceMapSearchResult pc_res = search_source(cpu->pc, cartridgestate.bank_mask);
                            if(pc_res.found && (pc_res.file == res.file)) {
                                pc_line = pc_res.line->line;
                            }
                        }
                        // Scrolling to the line every frame would pin the view to it.
                        bool scroll_to_shown = (res.file->name != last_scrolled_file) || (shown_line != last_scrolled_line);
                        bool should_save_breakpoints = false;
                        ImGui::BeginChild("Source file");
                        int i = 1;

                        for(auto& line : res.file->contents) {
                            ImGui::PushID(i);
                            ImVec4 color = other_line_color;
                            if(viewing_code && (i == shown_line)) {
                                color = viewed_line_color;
                            } else if(i == pc_line) {
                                color = pc_line_color;
                            }
                            if(i == pc_line) {
                                ImGui::Separator();
                            }
                            if(source_breakpoint_margin(res.file->name, i)) {
                                should_save_breakpoints = true;
                            }
                            ImGui::SameLine();
                            ImGui::TextDisabled("%4d", i);
                            ImGui::SameLine();
                            RenderSourceLine(line, color);
                            if(i == pc_line) {
                                ImGui::Separator();
                            }
                            if((i == shown_line) && scroll_to_shown) {
                                ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleCenterY);
                            }
                            ImGui::PopID();
                            i++;
                        }
                        ImGui::EndChild();
                        if(scroll_to_shown) {
                            last_scrolled_file = res.file->name;
                            last_scrolled_line = shown_line;
                        }
                        if(should_save_breakpoints) {
                            gameconfig.Save();
                        }
                    } else {
                        ImGui::Text("Couldn't find file. %s/%s", SourceMap::singleton->project_root.c_str(), res.file->name.c_str());
                    }
                } else {
                    ImGui::Text("Couldn't find source for %04x/%02x, debug num is %d", address, bank, res.debug);
                }
            } else {
                ImGui::Text("No source map loaded");
            }
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    tab_to_select = CodeTab::None;
    if(memory_pane_shown) {
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("memory pane", ImVec2(0, 0), ImGuiChildFlags_Borders);
        memory_pane.Render();
        ImGui::EndChild();
    }
    sizeOut = ImVec2(480, 640); // the initial size; the window can be resized from there
    ImGui::End();
    return sizeOut;
}