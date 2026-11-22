#pragma once

#include "debug_window.h"
#include "fitting_tooltip.h"
#include "../system_state.h"
#include "../mos6502/mos6502.h"

// The framebuffers and graphics RAM, enlarged, with the registers that pick among them. Each
// graphics RAM bank is drawn as the 256x256 page it is, its four 128x128 quadrants in place.
class VRAMWindow : public DebugWindow {
private:
    // What a panel shows: 128x128 blocks, which a surface (and the memory behind it) stacks from
    // the top down, laid out blocks_wide to a row.
    struct Panel {
        const char* label;
        SDL_Texture* texture;
        // How many blocks the texture stacks.
        int texture_blocks;
        const uint8_t* memory;
        int first_block;
        int blocks_wide;
        int blocks_high;
        // Graphics RAM: the bank shown. Framebuffers: -1.
        int gram_bank;
    };

    SDL_Surface* vRAM_Surface;
    SDL_Surface* gRAM_Surface;
    SystemState* system_state;
    mos6502* cpu_core;
    CartridgeState* cartridge_state;
    // The surfaces as textures of this window's renderer, which frees them with itself. Refreshed
    // every frame.
    SDL_Texture* vram_texture;
    SDL_Texture* gram_texture;
    // Screen pixels per GameTank pixel.
    int zoom;
    bool show_quadrant_grid;
    FittingTooltip pixel_tooltip;

    SDL_Texture* CreateTextureFor(SDL_Surface* source);
    void RenderPanel(const Panel& panel);
    void RenderPixelTooltip(const Panel& panel, ImVec2 origin);
protected:
    ImVec2 Render();
public:
    VRAMWindow(SDL_Surface* vram, SDL_Surface* gram, SystemState* system, mos6502* cpu, CartridgeState* cartridge);
};
