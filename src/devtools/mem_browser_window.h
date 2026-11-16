
#include "debug_window.h"
#include "memory_map.h"
#include "memory_view.h"
#include "../game_config.h"
#include <functional>

class MemBrowserWindow : public DebugWindow {
private:
    static constexpr const char* SIZE_NAME = "mem_browser";

    MemoryMap*& memorymap;
    const std::function<uint8_t(uint16_t, bool)> mem_read;
    const std::function<uint8_t*(uint16_t)> ram_read;
    bool decimal = false;
    GameConfig &gameconfig;
    // The Explorer tab.
    MemoryView explorer;
protected:
    ImVec2 Render();
public:
    MemBrowserWindow(MemoryMap*& map, std::function<uint8_t(uint16_t, bool)> reader,
    std::function<uint8_t*(uint16_t)> ram_read, GameConfig &gameconfig):
        DebugWindow("Memory Browser", SIZE_NAME),
        memorymap(map),
        mem_read(reader),
        ram_read(ram_read),
        gameconfig(gameconfig),
        explorer(map, reader, SIZE_NAME, "top_row") {};
    // Scrolls the Explorer tab to size bytes at address, and highlights them.
    void ShowMemory(uint16_t address, uint32_t size);
};
