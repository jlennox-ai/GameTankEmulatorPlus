#include "vram_window.h"
#include "settings.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

#define SETTINGS_NAME "vram"
#define ZOOM_KEY "zoom"
#define QUADRANT_GRID_KEY "quadrant_grid"

#define DEFAULT_WIDTH 1120
#define DEFAULT_HEIGHT 660
#define DEFAULT_ZOOM 2
#define MIN_ZOOM 1
#define MAX_ZOOM 8
#define ZOOM_SLIDER_WIDTH 120

// A framebuffer, and a quadrant of graphics RAM: what the CPU sees at BLOCK_ADDRESS.
#define BLOCK_SIZE 128
#define BLOCK_ADDRESS 0x4000
#define FRAMEBUFFER_COUNT 2
#define GRAM_BANK_COUNT 8
#define QUADRANTS_WIDE 2
#define QUADRANTS_HIGH 2
#define QUADRANTS_PER_BANK (QUADRANTS_WIDE * QUADRANTS_HIGH)

#define BORDER_COLOR IM_COL32(110, 110, 110, 255)
#define GRID_COLOR IM_COL32(255, 230, 0, 200)

static const char* quadrant_names[QUADRANTS_PER_BANK] = { "NW", "NE", "SW", "SE" };

VRAMWindow::VRAMWindow(
    SDL_Surface* vram, SDL_Surface* gram,
    SystemState* system, mos6502* cpu,
    CartridgeState* cartridge) : DebugWindow("VRAM Viewer", SETTINGS_NAME) {

    vRAM_Surface = vram;
    gRAM_Surface = gram;
    system_state = system;
    cpu_core = cpu;
    cartridge_state = cartridge;
    vram_texture = CreateTextureFor(vRAM_Surface);
    gram_texture = CreateTextureFor(gRAM_Surface);

    zoom = DEFAULT_ZOOM;
    Settings::LoadValue(SETTINGS_NAME, ZOOM_KEY, zoom);
    zoom = std::clamp(zoom, MIN_ZOOM, MAX_ZOOM);
    int grid_setting = 1;
    Settings::LoadValue(SETTINGS_NAME, QUADRANT_GRID_KEY, grid_setting);
    show_quadrant_grid = (grid_setting != 0);
}

SDL_Texture* VRAMWindow::CreateTextureFor(SDL_Surface* source) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, source->format->format, SDL_TEXTUREACCESS_STREAMING,
        source->w, source->h);
    if(texture == NULL) {
        throw std::runtime_error(std::string("VRAM viewer: can't create a texture: ") + SDL_GetError());
    }
    // The surfaces' alpha isn't meaningful, and enlarged pixels stay square.
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
    return texture;
}

ImVec2 VRAMWindow::Render() {
    SDL_UpdateTexture(vram_texture, NULL, vRAM_Surface->pixels, vRAM_Surface->pitch);
    SDL_UpdateTexture(gram_texture, NULL, gRAM_Surface->pixels, gRAM_Surface->pitch);

    // Fill the SDL window, whatever size it has been resized to.
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("VRAM Viewer", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ImGui::SetNextItemWidth(ZOOM_SLIDER_WIDTH);
    if(ImGui::SliderInt("Zoom", &zoom, MIN_ZOOM, MAX_ZOOM, "%dx", ImGuiSliderFlags_AlwaysClamp)) {
        Settings::SaveValue(SETTINGS_NAME, ZOOM_KEY, zoom);
    }
    ImGui::SameLine();
    if(ImGui::Checkbox("Quadrant grid", &show_quadrant_grid)) {
        Settings::SaveValue(SETTINGS_NAME, QUADRANT_GRID_KEY, show_quadrant_grid ? 1 : 0);
    }
    ImGui::SameLine();
    ImGui::Text("  DMA: %02x  BANK: %02x  PC: %04x  STATUS: %02x  WAIT: %d  HIBITS: %x",
        system_state->dma_control, system_state->banking, cpu_core->pc, cpu_core->status,
        cpu_core->waiting, cartridge_state->bank_mask);

    char labels[FRAMEBUFFER_COUNT + GRAM_BANK_COUNT][32];
    Panel panels[FRAMEBUFFER_COUNT + GRAM_BANK_COUNT];
    int panel_count = 0;
    for(int framebuffer = 0; framebuffer < FRAMEBUFFER_COUNT; ++framebuffer) {
        snprintf(labels[panel_count], sizeof labels[panel_count], "Framebuffer %d", framebuffer);
        panels[panel_count] = { labels[panel_count], vram_texture, FRAMEBUFFER_COUNT, system_state->vram,
            framebuffer, 1, 1, -1 };
        ++panel_count;
    }
    for(int bank = 0; bank < GRAM_BANK_COUNT; ++bank) {
        snprintf(labels[panel_count], sizeof labels[panel_count], "Graphics RAM bank %d", bank);
        panels[panel_count] = { labels[panel_count], gram_texture, GRAM_BANK_COUNT * QUADRANTS_PER_BANK,
            system_state->gram, bank * QUADRANTS_PER_BANK, QUADRANTS_WIDE, QUADRANTS_HIGH, bank };
        ++panel_count;
    }

    // The panels, left to right, wrapping at the window's edge.
    ImGui::BeginChild("panels", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for(int index = 0; index < panel_count; ++index) {
        RenderPanel(panels[index]);
        if(index + 1 < panel_count) {
            float next_width = (float) (panels[index + 1].blocks_wide * BLOCK_SIZE * zoom);
            if(ImGui::GetItemRectMax().x + spacing + next_width <= right_edge) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
    return ImVec2(DEFAULT_WIDTH, DEFAULT_HEIGHT);
}

void VRAMWindow::RenderPanel(const Panel& panel) {
    float block_size = (float) (BLOCK_SIZE * zoom);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(panel.label);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size(block_size * panel.blocks_wide, block_size * panel.blocks_high);
    ImVec2 corner(origin.x + size.x, origin.y + size.y);
    // Takes the panel's space in the layout, and the mouse over it.
    ImGui::InvisibleButton(panel.label, size);
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    for(int row = 0; row < panel.blocks_high; ++row) {
        for(int column = 0; column < panel.blocks_wide; ++column) {
            int texture_block = panel.first_block + row * panel.blocks_wide + column;
            ImVec2 top_left(origin.x + column * block_size, origin.y + row * block_size);
            ImVec2 bottom_right(top_left.x + block_size, top_left.y + block_size);
            draw_list->AddImage((ImTextureID) (intptr_t) panel.texture, top_left, bottom_right,
                ImVec2(0, (float) texture_block / panel.texture_blocks),
                ImVec2(1, (float) (texture_block + 1) / panel.texture_blocks));
        }
    }
    if(show_quadrant_grid) {
        for(int column = 1; column < panel.blocks_wide; ++column) {
            float x = origin.x + column * block_size;
            draw_list->AddLine(ImVec2(x, origin.y), ImVec2(x, corner.y), GRID_COLOR);
        }
        for(int row = 1; row < panel.blocks_high; ++row) {
            float y = origin.y + row * block_size;
            draw_list->AddLine(ImVec2(origin.x, y), ImVec2(corner.x, y), GRID_COLOR);
        }
    }
    draw_list->AddRect(origin, corner, BORDER_COLOR);
    ImGui::EndGroup();

    if(hovered) {
        RenderPixelTooltip(panel, origin);
    }
}

void VRAMWindow::RenderPixelTooltip(const Panel& panel, ImVec2 origin) {
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int x = std::clamp((int) ((mouse.x - origin.x) / zoom), 0, panel.blocks_wide * BLOCK_SIZE - 1);
    int y = std::clamp((int) ((mouse.y - origin.y) / zoom), 0, panel.blocks_high * BLOCK_SIZE - 1);
    int block = (y / BLOCK_SIZE) * panel.blocks_wide + (x / BLOCK_SIZE);
    int offset_in_block = (y % BLOCK_SIZE) * BLOCK_SIZE + (x % BLOCK_SIZE);
    uint8_t value = panel.memory[(panel.first_block + block) * FRAME_BUFFER_SIZE + offset_in_block];

    pixel_tooltip.Begin();
    if(panel.gram_bank >= 0) {
        ImGui::Text("Graphics RAM bank %d, quadrant %s", panel.gram_bank, quadrant_names[block]);
    } else {
        ImGui::TextUnformatted(panel.label);
    }
    ImGui::Text("X: %d  Y: %d", x, y);
    ImGui::Text("Address: 0x%04X", BLOCK_ADDRESS + offset_in_block);
    ImGui::Text("Value: 0x%02X (hue %d, saturation %d, luminance %d)", value, value >> 5, (value >> 3) & 3,
        value & 7);
    pixel_tooltip.End();
}
