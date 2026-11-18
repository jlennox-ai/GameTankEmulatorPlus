#include "debug_window.h"
#include "settings.h"
#include "memory_map.h"
#include "fitting_tooltip.h"
#include "memory_view.h"
#include "variable_map.h"
#include "../timekeeper.h"
#include <functional>
#include <string>
#include "../mos6502/mos6502.h"
#include "../game_config.h"
#include "../system_state.h"

class SteppingWindow : public DebugWindow {
private:
    static constexpr const char* SIZE_NAME = "code_stepper";
    static constexpr const char* MEMORY_PANE_KEY = "memory_pane";

    Timekeeper& timekeeper;
    MemoryMap*& memorymap;
    mos6502* cpu;
    GameConfig& gameconfig;
    CartridgeState& cartridgestate;
    // Reads emulated memory; the second argument is whether the read may have side effects.
    const std::function<uint8_t(uint16_t, bool)> mem_read;
    // Shows size bytes at an address in the memory browser (opening it if it's closed).
    const std::function<void(uint16_t, uint32_t)> show_memory;
    // The memory pane at the window's right (the Memory button), and whether it's shown (kept
    // between runs). While it is, a clicked variable shows there, not in the memory browser.
    MemoryView memory_pane;
    bool memory_pane_shown = false;
    // The hovered variable's value, in the source view.
    FittingTooltip variable_tooltip;
    // Shows or hides it, widening or narrowing the window by it.
    void ToggleMemoryPane();
    // The source line the Source tab last scrolled to, so it only scrolls when that changes.
    std::string last_scrolled_file;
    int last_scrolled_line = -1;
    // A breakpoint clicked in the list: the code tabs show its code instead of the PC's until
    // the CPU next stops (or "Back to PC").
    bool viewing_code = false;
    uint16_t view_address = 0;
    uint8_t view_bank = 0;
    bool was_stopped = false;
    // Whether the code tabs have shown a stop yet: the first one opens the Source tab, if it has source.
    bool shown_a_stop = false;
    enum class CodeTab { None, Disassembly, Source };
    CodeTab tab_to_select = CodeTab::None; // switched to on the next frame
    void ViewCode(uint16_t address, uint8_t bank);
    void ShowPcCode();

    // Draws a line of the Source tab, underlining the variables in scope (and their members),
    // with a tooltip of the value under the mouse.
    void RenderSourceLine(const std::string& text, ImVec4 color);
    // Whether chain names something the Source tab can show: a variable in scope at the PC
    // (from variables.dbg), or for a lone name, a RAM symbol (from the memory map).
    bool CanDescribe(const VariableMap::Chain& chain);
    std::string Describe(const VariableMap::Chain& chain);
    // Where chain's value is in memory, for clicking it: false if it's not in memory.
    bool Locate(const VariableMap::Chain& chain, uint16_t& address, uint32_t& size);
    // A RAM symbol's address, for names without debug info (asm variables, cc65 builds).
    bool FindRamSymbol(const std::string& name, uint16_t& address);
protected:
    ImVec2 Render();
public:
    SteppingWindow(Timekeeper& timekeeper,
        MemoryMap*& memorymap,
        mos6502* cpu,
        GameConfig& gameconfig,
        CartridgeState& cartridgestate,
        std::function<uint8_t(uint16_t, bool)> mem_read,
        std::function<void(uint16_t, uint32_t)> show_memory) :
        DebugWindow("Debugger", SIZE_NAME),
        timekeeper(timekeeper),
        memorymap(memorymap),
        cpu(cpu),
        gameconfig(gameconfig),
        cartridgestate(cartridgestate),
        mem_read(mem_read),
        show_memory(show_memory),
        memory_pane(memorymap, mem_read, SIZE_NAME, "memory_top_row") {
        int shown = 0;
        memory_pane_shown = Settings::LoadValue(SIZE_NAME, MEMORY_PANE_KEY, shown) && (shown != 0);
    };
};
