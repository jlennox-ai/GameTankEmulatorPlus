#include "SDL_inc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <time.h>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <vector>
#include <thread>
#include <algorithm>
#ifdef WASM_BUILD
#include "emscripten.h"
#include <emscripten/html5.h>
#else
#include "tinyfd/tinyfiledialogs.h"
#endif

#include "joystick_adapter.h"
#include "audio_coprocessor.h"
#include "save_state.h"
#include "blitter.h"
#include "palette.h"

#include "timekeeper.h"
#include "system_state.h"
#include "emulator_config.h"
#include "game_config.h"

#include "mos6502/mos6502.h"

#include "devtools/memory_map.h"
#include "devtools/breakpoints.h"
#include "devtools/source_map.h"
#include "devtools/source_stepper.h"
#include "devtools/variable_map.h"
#include "devtools/settings.h"

#include "ui/ui_utils.h"
#include "devtools/profiler.h"
#include "devtools/disassembler.h"
#include "paddle.h"

#ifndef WASM_BUILD
#include "devtools/profiler_window.h"
#include "devtools/mem_browser_window.h"
#include "devtools/vram_window.h"
#include "devtools/stepping_window.h"
#include "devtools/patching_window.h"
#include "devtools/controller_options_window.h"
#include "imgui.h"
#include "implot.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"
#include "whereami/whereami.h"
#endif

#ifndef WINDOW_TITLE
#define WINDOW_TITLE "GameTank Emulator"
#endif

using namespace std;

const int GT_WIDTH = 128;
const int GT_HEIGHT = 128;
const int VIEWPORT_ASPECT_W = 4;
const int VIEWPORT_ASPECT_H = 3;
const int MIN_DISPLAY_SCALE = 2;
int display_scale = 4;

int viewport_width(int scale) {
	return (GT_HEIGHT * scale * VIEWPORT_ASPECT_W + VIEWPORT_ASPECT_H - 1) / VIEWPORT_ASPECT_H;
}

int viewport_height(int scale) {
	return GT_HEIGHT * scale;
}

RomType loadedRomType;

mos6502 *cpu_core;
Blitter *blitter;
AudioCoprocessor *soundcard;
JoystickAdapter *joysticks;
SystemState system_state;
CartridgeState cartridge_state;

RGB_Color *palette;

MemoryMap* loadedMemoryMap;
GameConfig* gameconfig;
std::string currentRomFilePath;
std::string nvramFileFullPath;
std::string flashFileFullPath;

#define MAX_FRAME_FLIP_PROFILE_VSYNCS 8

bool vsyncProfileArmed = false;
bool vsyncProfileRunning = false;
bool frameFlipProfileArmed = false;
bool frameFlipProfileRunning = false;
int frameFlipProfileVsyncLimit = MAX_FRAME_FLIP_PROFILE_VSYNCS;
// The frame flips a frame flip profile has left to record.
int frameFlipProfileFramesLeft = 0;
// The choices of Deep Profile > Several Frame Flips.
const int SEVERAL_FRAME_FLIP_COUNTS[] = { 2, 5, 10, 30, 60 };

// Records the next frames frame flips, or until MAX_FRAME_FLIP_PROFILE_VSYNCS vsyncs a frame go by.
void armFrameFlipProfile(int frames) {
	frameFlipProfileArmed = true;
	frameFlipProfileFramesLeft = frames;
	frameFlipProfileVsyncLimit = MAX_FRAME_FLIP_PROFILE_VSYNCS * frames;
}

// A vsync NMI that came due while the CPU was stopped (at a breakpoint or the end of a step):
// raised when it runs again, so a stop never lands in the NMI handler by itself.
bool vsyncNmiPending = false;

bool showMenu = false;
bool menuOpening = false;
#ifdef WRAPPER_MODE
const bool menuBarAlwaysShown = false;
#else
// The menu bar is always shown, with the game drawn below it (WRAPPER_MODE's Esc menu goes over
// the game instead).
const bool menuBarAlwaysShown = true;
#endif
// The menu bar and toolbar's height together, measured as they're drawn (0 until then, and
// without them): the game is drawn below them.
int topBarsHeight = 0;
// Whether the 4:3 viewport's margins beside the screen show its rightmost column stretched out
// (otherwise they're black): Tools > Display Screen Sides, kept between runs.
bool showScreenSides = true;
const char* MAIN_WINDOW_SETTINGS = "main_window";
const char* SCREEN_SIDES_KEY = "screen_sides";
// Where refreshScreen last drew the game's viewport, for screenshots.
SDL_Rect lastViewport = { 0, 0, 0, 0 };
#define GRID_NONE 0
#define GRID_25   1
#define GRID_50   2
#define GRID_FULL 3
int grid_mode = GRID_NONE;
int resetQueued = 0;
// Ctrl+R: reload the ROM file (and its debug files) before the queued reset, to pick up a rebuild.
bool reloadQueued = false;
#define MUTE_SOURCE_MANUAL 1
#define MUTE_SOURCE_MENU 2
int muteMask = 0;
bool mouse_paddle_enabled = false;
bool paddle_touch_mode = false;

void SaveNVRAM() {
	fstream file;
	if(loadedRomType != RomType::FLASH2M_RAM32K) return;
	printf("SAVING %s\n", nvramFileFullPath.c_str());
	file.open(nvramFileFullPath.c_str(), ios_base::out | ios_base::binary | ios_base::trunc);
	file.write((char*) cartridge_state.save_ram, CARTRAMSIZE);
	file.close();
}

void LoadNVRAM() {
	fstream file;
	if(loadedRomType != RomType::FLASH2M_RAM32K) return;
	printf("LOADING %s\n", nvramFileFullPath.c_str());
	file.open(nvramFileFullPath.c_str(), ios_base::in | ios_base::binary);
	file.read((char*) cartridge_state.save_ram, CARTRAMSIZE);
	file.close();
}

std::thread savingThread;

void SaveModifiedFlash() {
	if(EmulatorConfig::noSave) return;
	fstream file_out, file_in;
	uint8_t* rom_cursor = cartridge_state.rom;
	uint8_t buf[256];
	file_in.open(currentRomFilePath, ios_base::in | ios_base::binary);
	file_out.open(flashFileFullPath.c_str(), ios_base::out | ios_base::binary | ios_base::trunc);
	while(file_in) {
		file_in.read((char*) buf, 256);
		size_t bytesRead = file_in.gcount();
		if(bytesRead) {
			for(int i = 0; i < bytesRead; ++i) {
				buf[i] ^= *(rom_cursor++);
			}
			file_out.write((char*) buf, bytesRead);
		}
	}
	file_in.close();
	file_out.close();
#ifdef WASM_BUILD
	EM_ASM(
		FS.syncfs(false, function (err) {
			assert(!err);
			});
	);
#endif
}

fstream orig_rom, xor_file;
void LoadModifiedFlash() {
	uint8_t* rom_cursor = cartridge_state.rom;
	uint8_t buf[256];
	uint8_t bufx[256];
	size_t bytes_read = 0;
	std::cout << "opening " << currentRomFilePath << " and " << flashFileFullPath << "\n";
	orig_rom.open(currentRomFilePath, ios_base::in | ios_base::binary);
	xor_file.open(flashFileFullPath, ios_base::in | ios_base::binary);
	std::cout << "XORing files together... \n";
	while(orig_rom && xor_file) {
		orig_rom.read((char*) buf, 256);
		xor_file.read((char*) bufx, 256); 
		for(int i = 0; i < orig_rom.gcount(); ++i) {
			*(rom_cursor++) = buf[i] ^ bufx[i];
		}
		bytes_read += 256;
	}
	std::cout << bytes_read << " bytes read from xor file\n";
#ifndef WASM_BUILD
	orig_rom.close();
	xor_file.close();
#endif
}

const uint8_t VIA_ORB    = 0x0;
const uint8_t VIA_ORA    = 0x1;
const uint8_t VIA_DDRB   = 0x2;
const uint8_t VIA_DDRA   = 0x3;
const uint8_t VIA_T1CL   = 0x4;
const uint8_t VIA_T1CH   = 0x5;
const uint8_t VIA_T1LL   = 0x6;
const uint8_t VIA_T1LH   = 0x7;
const uint8_t VIA_T2CL   = 0x8;
const uint8_t VIA_T2CH   = 0x9;
const uint8_t VIA_SR     = 0xA;
const uint8_t VIA_ACR    = 0xB;
const uint8_t VIA_PCR    = 0xC;
const uint8_t VIA_IFR    = 0xD;
const uint8_t VIA_IER    = 0xE;
const uint8_t VIA_ORA_NH = 0xF;

//Pins of VIA Port A used for Serial comms (or other misc cartridge use)
const uint8_t VIA_SPI_BIT_CLK  = 0b00000001;
const uint8_t VIA_SPI_BIT_MOSI = 0b00000010;
const uint8_t VIA_SPI_BIT_CS   = 0b00000100;
const uint8_t VIA_SPI_BIT_MISO = 0b10000000;

#define RAM_HIGHBITS_SHIFT 7

#define FULL_RAM_ADDRESS(x) (((system_state.banking & BANK_RAM_MASK) << RAM_HIGHBITS_SHIFT) | (x))

extern unsigned char font_map[];

Timekeeper timekeeper;
Profiler profiler(timekeeper);

SDL_Surface* gRAM_Surface = NULL;
SDL_Surface* vRAM_Surface = NULL;

SDL_Window* mainWindow = NULL;
SDL_Window* buffers_window = NULL;
Uint32 rmask, gmask, bmask, amask;

#ifndef WASM_BUILD
ImGuiContext* main_imgui_ctx;
ImPlotContext* main_implot_ctx;

std::vector<BaseWindow*> toolWindows;
#endif

SDL_Renderer* mainRenderer = NULL;
SDL_Texture* framebufferTexture = NULL;
SDL_Texture* gridOverlayTexture = NULL;
int gridOverlayScale = 0;

void rebuildGridOverlay(int scale) {
	if(gridOverlayTexture && gridOverlayScale == scale) {
		return;
	}

	if(gridOverlayTexture) {
		SDL_DestroyTexture(gridOverlayTexture);
		gridOverlayTexture = NULL;
	}

	int w = GT_WIDTH * scale;
	int h = GT_HEIGHT * scale;
	gridOverlayTexture = SDL_CreateTexture(mainRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
	SDL_SetTextureBlendMode(gridOverlayTexture, SDL_BLENDMODE_BLEND);

	SDL_Texture* prev_target = SDL_GetRenderTarget(mainRenderer);
	SDL_SetRenderTarget(mainRenderer, gridOverlayTexture);
	SDL_SetRenderDrawBlendMode(mainRenderer, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(mainRenderer, 0, 0, 0, 0);
	SDL_RenderClear(mainRenderer);
	SDL_SetRenderDrawColor(mainRenderer, 0, 0, 0, 255);

	SDL_Rect line;
	for(int i = 0; i < GT_WIDTH; ++i) {
		line.x = i * scale;
		line.y = 0;
		line.w = 1;
		line.h = h;
		SDL_RenderFillRect(mainRenderer, &line);
	}
	for(int i = 1; i < GT_HEIGHT; ++i) {
		line.x = 0;
		line.y = i * scale;
		line.w = w;
		line.h = 1;
		SDL_RenderFillRect(mainRenderer, &line);
	}

	SDL_SetRenderTarget(mainRenderer, prev_target);
	SDL_SetTextureScaleMode(gridOverlayTexture, SDL_ScaleModeNearest);
	gridOverlayScale = scale;
}

bool isFullScreen = false;

bool profiler_open = false;
bool buffers_open = false;
int profiler_x_axis = 0;

uint8_t open_bus() {
	return rand() % 256;
}

uint8_t VDMA_Read(uint16_t address) {
	blitter->CatchUp();
	if(system_state.dma_control & DMA_COPY_ENABLE_BIT) {
		return open_bus();
	} else {
		uint8_t* bufPtr;
		uint32_t offset = 0;
		if(system_state.dma_control & DMA_CPU_TO_VRAM) {
			bufPtr = system_state.vram;
			if(system_state.banking & BANK_VRAM_MASK) {
				offset = 0x4000;
			}
		} else {
			bufPtr = system_state.gram;
			offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) << 14;
		}
		return bufPtr[(address & 0x3FFF) | offset];
	}
}

void VDMA_Write(uint16_t address, uint8_t value) {
	blitter->CatchUp();
	if(system_state.dma_control & DMA_COPY_ENABLE_BIT) {
		blitter->SetParam(address, value);
	} else {
		uint8_t* bufPtr;
		uint32_t offset = 0;
		SDL_Surface* targetSurface = NULL;
		uint32_t yShift = 0;
		if(system_state.dma_control & DMA_CPU_TO_VRAM) {
			bufPtr = system_state.vram;
			targetSurface = vRAM_Surface;
			if(system_state.banking & BANK_VRAM_MASK) {
				offset = 0x4000;
				yShift = GT_HEIGHT;
			}
		} else {
			bufPtr = system_state.gram;
			targetSurface = gRAM_Surface;
			yShift = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) * GT_HEIGHT;
			offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) << 14;
		}
		bufPtr[(address & 0x3FFF) | offset] = value;

		uint8_t x, y;
		x = address & 127;
		y = (address >> 7) & 127;
		put_pixel32(targetSurface, x, y + yShift, Palette::ConvertColor(targetSurface, value));
	}
}

void UpdateFlashShiftRegister(uint8_t nextVal) {
	//TODO: Care about DDR bits
	//For now assuming that if we're using Flash2M hardware we're behaving ourselves
	uint8_t oldVal = system_state.VIA_regs[VIA_ORA];
	uint8_t risingBits = nextVal & ~oldVal;
	if(risingBits & VIA_SPI_BIT_CLK) {
		cartridge_state.bank_shifter = cartridge_state.bank_shifter << 1;
		cartridge_state.bank_shifter &= 0xFE;
		cartridge_state.bank_shifter |= !!(oldVal & VIA_SPI_BIT_MOSI);
	} else if(risingBits & VIA_SPI_BIT_CS) {
		//flash cart CS is connected to latch clock
		if((cartridge_state.bank_mask ^ cartridge_state.bank_shifter) & 0x80) {
			SaveNVRAM();
		}
		cartridge_state.bank_mask = cartridge_state.bank_shifter;
		if(loadedRomType != RomType::FLASH2M_RAM32K) {
			cartridge_state.bank_mask |= 0x80;
		}
		//printf("Flash highbits set to %x\n", cartridge_state.bank_mask);
	}
}

uint8_t MemoryRead_Flash2M(uint16_t address) {
	if(address & 0x4000) {
		return cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)];
	} else {
		if(!(cartridge_state.bank_mask & 0x80))
			return cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)];
		else return cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)];
	}
}

uint8_t MemoryRead_Unknown(uint16_t address) {
	//If cartridge_state.size is smaller than unbanked ROM range, align end with 0xFFFF and wrap
	//If cartridge_state.size is bigger than unbanked ROM range, access mainWindow at end of file.
	//TODO: Decide if unknown ROM type should just terminate emulator :P
	if(cartridge_state.size <= 32768) {
		return cartridge_state.rom[((address & 0x7FFF) + 32768 - cartridge_state.size) % cartridge_state.size];
	} else {
		return cartridge_state.rom[((address & 0x7FFF) + cartridge_state.size - 32768)];
	}
}

uint8_t* GetRAM(const uint16_t address) {
	return &(system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)]);
}

uint8_t MemoryReadResolve(const uint16_t address, bool stateful) {
	if(address & 0x8000) {
		switch(loadedRomType) {
			case RomType::EEPROM8K:
			return cartridge_state.rom[address & 0x1FFF];
			case RomType::EEPROM32K:
			return cartridge_state.rom[address & 0x7FFF];
			case RomType::FLASH2M:
			case RomType::FLASH2M_RAM32K:
			return MemoryRead_Flash2M(address);
			case RomType::UNKNOWN:
			return MemoryRead_Unknown(address);
		}
	} else if(address & 0x4000) {
		return VDMA_Read(address);
	} else if((address >= 0x3000) && (address <= 0x3FFF)) {
		return soundcard->ram_read(address);
	} else if((address >= 0x2800) && (address <= 0x2FFF)) {
		return system_state.VIA_regs[address & 0xF];
	} else if(address < 0x2000) {
		if(stateful) {
			if(!system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)]) {
				//printf("WARNING! Uninitialized RAM read at %x (Bank %x)\n", address, system_state.banking >> 5);
			}
		}
		return *GetRAM(address);
	} else if((address == 0x2008) || (address == 0x2009)) {
		return joysticks->read((uint8_t) address, stateful);
	}
	if(stateful) {
		printf("Attempted to read write-only device, may be unintended? %x\n", address);
	}
	return open_bus();
}

uint8_t MemoryRead(uint16_t address) {
	return MemoryReadResolve(address, true);
}

uint8_t MemorySync(uint16_t address) {
	bool stepping = (timekeeper.clock_mode == CLOCKMODE_STEPSOURCE) || (timekeeper.clock_mode == CLOCKMODE_STEPOUT);
	if((timekeeper.clock_mode == CLOCKMODE_NORMAL) || stepping) {
		uint8_t opcode = MemoryReadResolve(address, false);
		bool stop = Breakpoints::checkBreakpoint(address, cartridge_state.bank_mask);
		if(stepping && SourceStepper::ShouldStop(address, cartridge_state.bank_mask, cpu_core->sp, opcode, MemoryReadResolve)) {
			stop = true;
		}
		if(stop) {
			timekeeper.clock_mode = CLOCKMODE_STOPPED;
			Disassembler::Decode(MemoryReadResolve, loadedMemoryMap, address, 32);
			cpu_core->Freeze();
		}
		if(opcode == 0x20) { //JSR
			uint16_t jsr_dest = MemoryReadResolve(address+1, false) | (MemoryReadResolve(address+2, false) << 8);
			profiler.LogJSR(address, cartridge_state.bank_mask, jsr_dest);
		} else if(opcode == 0x60) { //RTS
			profiler.LogRTS(address, cartridge_state.bank_mask);
		}
	}
	return MemoryRead(address);
}

void MemoryWrite(uint16_t address, uint8_t value) {
	if(address & 0x8000) {
		if(loadedRomType == RomType::FLASH2M_RAM32K) {
			if(!(address & 0x4000)) {
				if(!(cartridge_state.bank_mask & 0x80)) {
					cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)] = value;
				}
			}
		}
		if(loadedRomType == RomType::FLASH2M) {
			if(cartridge_state.write_mode) {
				uint8_t* location;
				if(address & 0x4000) {
					location = &(cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)]);
				} else {
					location = &(cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)]);
				}
				*location &= value;
				cartridge_state.write_mode = false;
			} else {
				//Skipping over details like bypass and unlock commands for now
				//So off-spec flash operation will be inaccurate
				if(value == 0x10) {
					//Chip Erase
					for(int i = 0; i < (1 << 21); ++i) {
						cartridge_state.rom[i] = 0xFF;
					}
				} else if (value == 0x30) {
					//Sector erase
					uint8_t sectorBits = ((address & (1 << 13)) >> 13) | ((cartridge_state.bank_mask & 0x7F) << 1);
					uint8_t sectorNum = sectorBits >> 3;
					if(sectorNum < 31) {
						//most of the sector table
						uint32_t x = sectorNum << 16;
						for(uint32_t i = 0; i < (1 << 16); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if((sectorBits & 4) == 0) {
						uint32_t x = 0x1F0000;
						for(uint32_t i = 0; i < (1 << 15); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if(sectorBits == 0b11111100) {
						uint32_t x = 0x1F8000;
						for(uint32_t i = 0; i < (1 << 13); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if(sectorBits == 0b11111101) {
						uint32_t x = 0x1FA000;
						for(uint32_t i = 0; i < (1 << 13); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					} else if((sectorBits >> 1) == 0b1111111) {
						uint32_t x = 0x1FC000;
						for(uint32_t i = 0; i < (1 << 14); ++i) {
							cartridge_state.rom[x] = 0xFF;
							++x;
						}
					}
				} else if(value == 0xA0) {
					cartridge_state.write_mode = true;
				} else if(value == 0x90) {
					//first byte of lock command should be a good time to write to file
#ifdef WASM_BUILD
					SaveModifiedFlash();
#else
					if(savingThread.joinable()) {
						savingThread.join();
					}
					savingThread = std::thread(SaveModifiedFlash);
#endif
				}
			}
		}
	}
	else if(address & 0x4000) {
		VDMA_Write(address, value);
	} else if(address >= 0x3000 && address <= 0x3FFF) {
		soundcard->ram_write(address, value);
	} else if((address & 0x2000)) {
		if(address & 0x800) {
			if(loadedRomType == RomType::FLASH2M) {
				if((address & 0xF) == VIA_ORA) {
					UpdateFlashShiftRegister(value);
				}
			}
			if((address & 0xF) == VIA_ORB) {
				if((system_state.VIA_regs[VIA_ORB] & 0x80) && !(value & 0x80)) {
					//falling edge of high bit of ORB
					if(value & 0x40) {
						//report duration
						profiler.LogTime(value & 0x3F);
					} else {
						//store timestamp
						profiler.profilingTimeStamps[value & 0x3F] = timekeeper.totalCyclesCount;
					}
				}
			}
			system_state.VIA_regs[address & 0xF] = value;
		} else {
			if((address & 0x000F) == 0x0007) {
				blitter->CatchUp();
				if((value & DMA_VID_OUT_PAGE_BIT) != (system_state.dma_control & DMA_VID_OUT_PAGE_BIT)) {
					profiler.bufferFlipCount++;
					if(profiler.measure_by_frameflip) {
						profiler.ResetTimers();
						profiler.last_blitter_activity = blitter->pixels_this_frame;
						blitter->pixels_this_frame = 0;
					}
					if(frameFlipProfileArmed) {
						profiler.DeepProfileStart();
						frameFlipProfileArmed = false;
						frameFlipProfileRunning = true;
					} else if(frameFlipProfileRunning) {
						--frameFlipProfileFramesLeft;
						if(frameFlipProfileFramesLeft <= 0) {
							profiler.DeepProfileStop(loadedMemoryMap, SourceMap::singleton);
							frameFlipProfileRunning = false;
						}
					}
				}
				system_state.dma_control = value;
				system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
				if(system_state.dma_control & DMA_TRANSPARENCY_BIT) {
					SDL_SetColorKey(gRAM_Surface, SDL_TRUE, SDL_MapRGB(gRAM_Surface->format, 0, 0, 0));
				} else {
					SDL_SetColorKey(gRAM_Surface, SDL_FALSE, 0);
				}
			} else if((address & 0x000F) == 0x0005) {
				blitter->CatchUp();
				system_state.banking = value;
				//printf("banking reg set to %x\n", value);
			} else {
				soundcard->register_write(address, value);
			}
		}
	}
	else if(address < 0x2000) {
		/*if(!system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)]) {
			printf("First RAM write at %x (Bank %x) (Value %x)\n", address, system_state.banking >> 6, value);
		}*/
		system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)] = true;
		system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)] = value;
	}
}

SDL_Event e;
bool running = true;
bool gofast = false;
bool paused = true;
bool lshift = false;
bool rshift = false;

void randomize_vram() {
	for(int i = 0; i < VRAM_BUFFER_SIZE; i ++) {
		system_state.vram[i] = rand() % 256;
		put_pixel32(vRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(vRAM_Surface, system_state.vram[i]));
	}
	for(int i = 0; i < GRAM_BUFFER_SIZE; i ++) {
		system_state.gram[i] = rand() % 256;
		put_pixel32(gRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(gRAM_Surface, system_state.gram[i]));
	}
}

void randomize_memory() {
	for(int i = 0; i < RAMSIZE; i++) {
		system_state.ram[i] = rand() % 256;
		system_state.ram_initialized[i] = false;
	}

	for(int i = 0; i < VRAM_BUFFER_SIZE; i++) {
		system_state.vram[i] = rand() % 256;	
	}

	for(int i = 0; i < GRAM_BUFFER_SIZE; i++) {
		system_state.gram[i] = rand() % 256;	
	}
	
	system_state.dma_control = rand() % 256;
	system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
	system_state.banking = rand() % 256;
	blitter->gram_mid_bits = rand() % 4;
}

// Redraws the VRAM viewer's copies of video and graphics RAM, after they change wholesale.
void redraw_memory_surfaces() {
	for(int i = 0; i < VRAM_BUFFER_SIZE; i ++) {
		put_pixel32(vRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(vRAM_Surface, system_state.vram[i]));
	}
	for(int i = 0; i < GRAM_BUFFER_SIZE; i ++) {
		put_pixel32(gRAM_Surface, i & 127, i >> 7, Palette::ConvertColor(gRAM_Surface, system_state.gram[i]));
	}
}

// Save states: one slot, <rom>.state next to the ROM. They hold the machine (RAM, video and
// graphics RAM, the CPU, the blitter, the audio coprocessor, the cartridge's banking and save RAM
// and the frame timing) but not the ROM itself, so a state loads onto a rebuilt ROM: handy for
// trying a change from the same spot, though code that moved won't line up with the saved CPU.
const char SAVE_STATE_MAGIC[8] = { 'G', 'T', 'S', 'T', 'A', 'T', 'E', '\0' };
const uint32_t SAVE_STATE_VERSION = 1;

std::string saveStatePath() {
	std::filesystem::path path(currentRomFilePath);
	path.replace_extension("state");
	return path.string();
}

void serializeMachineState(StateFile& file) {
	file.Value(system_state.dma_control);
	file.Value(system_state.dma_control_irq);
	file.Value(system_state.banking);
	file.Value(system_state.ram);
	file.Value(system_state.ram_initialized);
	file.Value(system_state.vram);
	file.Value(system_state.gram);
	file.Value(system_state.VIA_regs);
	file.Value(cartridge_state.bank_shifter);
	file.Value(cartridge_state.bank_mask);
	file.Value(cartridge_state.save_ram);
	file.Value(cartridge_state.write_mode);
	file.Value(timekeeper.totalCyclesCount);
	file.Value(timekeeper.cycles_since_vsync);
	file.Value(vsyncNmiPending);
	cpu_core->SerializeState(file);
	blitter->SerializeState(file);
	soundcard->SerializeState(file);
}

// The header: what the state is, and the ROM size it was saved with (a state only fits a ROM
// of the same size: the banking differs).
void serializeSaveStateHeader(StateFile& file, char (&magic)[8], uint32_t& version, int& romSize) {
	file.Value(magic);
	file.Value(version);
	file.Value(romSize);
}

void showSaveStateMessage(const std::string& message, bool failed) {
	printf("%s\n", message.c_str());
	std::string title = std::string(WINDOW_TITLE) + " | " + currentRomFilePath + " | " + message;
	SDL_SetWindowTitle(mainWindow, title.c_str());
#ifdef TINYFILEDIALOGS_H
	if(failed) {
		tinyfd_notifyPopup("Save state", message.c_str(), "warning");
	}
#endif
}

void saveState() {
	if(currentRomFilePath.empty()) {
		showSaveStateMessage("No ROM loaded to save the state of", true);
		return;
	}
	std::vector<uint8_t> bytes;
	StateFile file(StateFile::Mode::Save, bytes);
	char magic[8];
	std::memcpy(magic, SAVE_STATE_MAGIC, sizeof(magic));
	uint32_t version = SAVE_STATE_VERSION;
	int romSize = cartridge_state.size;
	serializeSaveStateHeader(file, magic, version, romSize);
	serializeMachineState(file);

	std::string path = saveStatePath();
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write((const char*) bytes.data(), bytes.size());
	out.close();
	if(!out) {
		showSaveStateMessage("Couldn't write the state to " + path, true);
		return;
	}
	showSaveStateMessage("state saved", false);
}

void loadState() {
	if(currentRomFilePath.empty()) {
		showSaveStateMessage("No ROM loaded to load a state into", true);
		return;
	}
	std::string path = saveStatePath();
	std::ifstream in(path, std::ios::binary);
	if(!in) {
		showSaveStateMessage("No saved state yet (" + path + ")", true);
		return;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	// Checked before anything is loaded, so a bad file leaves the machine as it was.
	StateFile header(StateFile::Mode::Load, bytes);
	char magic[8] = {};
	uint32_t version = 0;
	int romSize = 0;
	try {
		serializeSaveStateHeader(header, magic, version, romSize);
	} catch(const std::runtime_error&) {
		showSaveStateMessage(path + " isn't a save state", true);
		return;
	}
	if(std::memcmp(magic, SAVE_STATE_MAGIC, sizeof(magic)) != 0) {
		showSaveStateMessage(path + " isn't a save state", true);
		return;
	}
	if(version != SAVE_STATE_VERSION) {
		showSaveStateMessage(path + " is from another version of the emulator", true);
		return;
	}
	if(romSize != cartridge_state.size) {
		showSaveStateMessage(path + " was saved with a different size of ROM", true);
		return;
	}
	// Every field has a fixed size, so the current state's size is the size a good file has.
	std::vector<uint8_t> current;
	StateFile measure(StateFile::Mode::Save, current);
	serializeSaveStateHeader(measure, magic, version, romSize);
	serializeMachineState(measure);
	if(bytes.size() != current.size()) {
		showSaveStateMessage(path + " is the wrong size for a save state", true);
		return;
	}

	StateFile file(StateFile::Mode::Load, bytes);
	serializeSaveStateHeader(file, magic, version, romSize);
	serializeMachineState(file);
	if(!file.AtEnd()) {
		throw std::logic_error("a save state of the right size wasn't read to its end");
	}
	redraw_memory_surfaces();
	if(timekeeper.clock_mode == CLOCKMODE_STOPPED) {
		Disassembler::Decode(MemoryReadResolve, loadedMemoryMap, cpu_core->pc, 32);
	}
	showSaveStateMessage("state loaded", false);
}

extern "C" {
void PauseEmulation() {
  paused = true;

  AudioCoprocessor::singleton_acp_state->isEmulationPaused = true;
}

void ResumeEmulation() {
  paused = false;

  AudioCoprocessor::singleton_acp_state->isEmulationPaused = false;
}
}

void CPUStopped() {
	PauseEmulation();
	printf("CPU stopped");
#ifdef TINYFILEDIALOGS_H
	tinyfd_notifyPopup("Alert",
		"CPU has stopped either due to STP opcode",
		"info");
#endif
}

const char * open_rom_dialog() {
	char const * lFilterPatterns[1] = {"*.gtr"};
#ifdef TINYFILEDIALOGS_H
	return tinyfd_openFileDialog(
		"Select a GameTank ROM file",
		"",
		1,
		lFilterPatterns,
		"GameTank Rom",
		0);
#else
	return EMBED_ROM_FILE;
#endif
}

// Where a ROM's debug file (out.map, sourcemap.dbg) is: next to the ROM if it's there, else
// in ../build, the GameTank SDK's project layout.
static std::filesystem::path FindDebugFile(const std::filesystem::path& romPath, const char* name) {
	std::filesystem::path besideRom = romPath.parent_path() / name;
	if(std::filesystem::exists(besideRom)) {
		return besideRom;
	}
	return romPath.parent_path() / "../build" / name;
}

extern "C" {
	// Attempts to load a rom by filename into a buffer
	// 0 on success
	// -1 on failure (e.g. file by name doesn't exist)
	int LoadRomFile(const char* filename) {
		std::filesystem::path filepath(filename);
		currentRomFilePath = filepath.string();
#ifdef WASM_BUILD
		std::filesystem::path nvramPath("/idbfs");
		nvramPath /= std::filesystem::path(currentRomFilePath).filename();
#else
		std::filesystem::path nvramPath(filename);
#endif
		nvramPath.replace_extension("sav");
		nvramFileFullPath = nvramPath.string();
		if (EmulatorConfig::xorFile != NULL) {
		  flashFileFullPath = std::string(EmulatorConfig::xorFile);
		} else {
		    nvramPath.replace_extension("xor");
		    flashFileFullPath = nvramPath.string();
		}
		nvramPath.replace_extension("gtrcfg");

		gameconfig = new GameConfig(nvramPath.string().c_str());

		std::filesystem::path defaultMemMapFilePath = FindDebugFile(filepath, "out.map");
		std::filesystem::path defaultSourceMapFilePath = FindDebugFile(filepath, "sourcemap.dbg");

		if(std::filesystem::exists(defaultMemMapFilePath)) {
			printf("found default memory map file location %s\n", defaultMemMapFilePath.c_str());
			loadedMemoryMap = new MemoryMap(defaultMemMapFilePath.string());
			Breakpoints::linkBreakpoints(*loadedMemoryMap);
		} else {
			loadedMemoryMap = new MemoryMap();
			printf("default memory map file %s not found\n", defaultMemMapFilePath.c_str());
		}

		if(std::filesystem::exists(defaultSourceMapFilePath)) {
			printf("found default source map file location %s\n", defaultSourceMapFilePath.c_str());
			std::string sourceMapPathString = defaultSourceMapFilePath.string();
			SourceMap::singleton = new SourceMap(sourceMapPathString);
			// Source line breakpoints follow their lines through edits and rebuilds.
			if(Breakpoints::linkSourceBreakpoints(*SourceMap::singleton)) {
				gameconfig->Save();
			}
		} else {
			printf("default source map file %s not found\n", defaultSourceMapFilePath.c_str());
		}

		// The source view's variable hovers (written by llvm-mos builds; cc65 builds have none).
		delete VariableMap::singleton;
		VariableMap::singleton = nullptr;
		std::filesystem::path variablesFilePath = FindDebugFile(filepath, "variables.dbg");
		if(std::filesystem::exists(variablesFilePath)) {
			VariableMap::singleton = new VariableMap(variablesFilePath.string());
		}

		printf("loading %s\n", filename);
		FILE* romFileP = fopen(filename, "rb");
		if(!romFileP) {
			printf("Unable to open file: %s\n", filename);
			return -1;
		}

		fseek(romFileP, 0L, SEEK_END);
		cartridge_state.size = ftell(romFileP);
		//cartridge_state.rom = new uint8_t [cartridge_state.size];
		cartridge_state.write_mode = false;
		rewind(romFileP);
		switch(cartridge_state.size) {
			case 8192:
			loadedRomType = RomType::EEPROM8K;
			printf("Detected 8K (EEPROM)\n");
			break;
			case 32768:
			loadedRomType = RomType::EEPROM32K;
			printf("Detected 32K (EEPROM)\n");
			break;
			case 2097152:
			loadedRomType = RomType::FLASH2M;
			printf("Detected 2M (Flash)\n");
			break;
			default:
			loadedRomType = RomType::UNKNOWN;
			printf("Unknown ROM type: Size is %d bytes\n", cartridge_state.size);
			break;
		}
		fread(cartridge_state.rom, sizeof(uint8_t), cartridge_state.size, romFileP);
		fclose(romFileP);
		if(cpu_core) {
			ResumeEmulation();
			cpu_core->Reset();
			cartridge_state.write_mode = false;
		}

		if(loadedRomType == RomType::FLASH2M) {

			if(std::filesystem::exists(flashFileFullPath.c_str())) {
				std::cout << "Loading flash save from " << flashFileFullPath << "\n";
				LoadModifiedFlash();
			} else {
				std::cout << "Couldn't find " << flashFileFullPath << "\n";
			}

			if(
				(cartridge_state.rom[0x1FFFF0] == 'S') &&
				(cartridge_state.rom[0x1FFFF1] == 'A') &&
				(cartridge_state.rom[0x1FFFF2] == 'V') &&
				(cartridge_state.rom[0x1FFFF3] == 'E')) {
					loadedRomType = RomType::FLASH2M_RAM32K;
					if(std::filesystem::exists(nvramFileFullPath.c_str())) {
						LoadNVRAM();
					}
				}
		}
		return 0;
	}

	void SetButtons(int buttonMask) {
		if(joysticks != NULL) {
			joysticks->SetHeldButtons(buttonMask);
		}
	}

	void takeScreenShot() {
		// The game's viewport, not the menu bar above it.
		SDL_Surface *screenshot = SDL_CreateRGBSurface(0, lastViewport.w, lastViewport.h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
		SDL_RenderReadPixels(mainRenderer, &lastViewport, SDL_PIXELFORMAT_ARGB8888, screenshot->pixels, screenshot->pitch);
		SDL_SaveBMP(screenshot, "screenshot.bmp");
		SDL_FreeSurface(screenshot);
	}

#ifdef WASM_BUILD
	extern "C" {
		EMSCRIPTEN_KEEPALIVE
		void SetPaddleMode(bool enabled) {
			mouse_paddle_enabled = enabled;
			if (mouse_paddle_enabled){
				if (paddle_touch_mode) {
					SDL_SetRelativeMouseMode(SDL_FALSE);
				}
				else
				{
					SDL_SetRelativeMouseMode(SDL_TRUE);
				}
			}
		}
		void SetPaddleTouchMode(bool enabled) {
			paddle_touch_mode = enabled;
			if (paddle_touch_mode) {
				SDL_SetRelativeMouseMode(SDL_FALSE);
			}
			else
			{
				SDL_SetRelativeMouseMode(SDL_TRUE);
			}
		}
		void EMSCRIPTEN_KEEPALIVE SetPaddleValue(int val) {
			if (joysticks != nullptr) {
				joysticks->SetPaddleBitsDirect(val); 
			}
    	}

		void EMSCRIPTEN_KEEPALIVE UpdatePaddleFromMouseJS(int index, int dx) {
			joysticks->UpdatePaddleFromMouse(0, dx);
		}

	}
	#endif
}
#ifndef WASM_BUILD
template <typename T>
void closeToolByType() {
    toolWindows.erase(
        std::remove_if(
            toolWindows.begin(),
            toolWindows.end(),
            [](BaseWindow* window) {
                if(dynamic_cast<T*>(window) != nullptr) {
					delete window;
					return true;
				}
				return false;
            }
        ),
        toolWindows.end()
    );
}

template <typename T>
bool toolTypeIsOpen() {
    for (const auto& window : toolWindows) {
        if (dynamic_cast<T*>(window) != nullptr) {
            return true;
        }
    }
    return false;
}

void toggleProfilerWindow() {
	if(!toolTypeIsOpen<ProfilerWindow>()) {
		toolWindows.push_back(new ProfilerWindow(profiler, loadedMemoryMap));
	} else {
		closeToolByType<ProfilerWindow>();
	}
}

void toggleMemBrowserWindow() {
	if(!toolTypeIsOpen<MemBrowserWindow>()) {
		toolWindows.push_back(new MemBrowserWindow(loadedMemoryMap, MemoryReadResolve, GetRAM, *gameconfig));
	} else {
		closeToolByType<MemBrowserWindow>();
	}
}

void toggleVRAMWindow() {
	if(!toolTypeIsOpen<VRAMWindow>()) {
		toolWindows.push_back(new VRAMWindow(vRAM_Surface, gRAM_Surface,
			&system_state, cpu_core, &cartridge_state));
	} else {
		closeToolByType<VRAMWindow>();
	}
}

// Memory the code stepper asked the memory browser to show (a clicked variable). It's shown
// after the tool windows draw: the browser may need opening, which changes the list being drawn.
bool memoryToShowPending = false;
uint16_t memoryToShowAddress = 0;
uint32_t memoryToShowSize = 0;

void requestShowMemory(uint16_t address, uint32_t size) {
	memoryToShowPending = true;
	memoryToShowAddress = address;
	memoryToShowSize = size;
}

void showRequestedMemory() {
	if(!memoryToShowPending) {
		return;
	}
	memoryToShowPending = false;
	MemBrowserWindow* browser = nullptr;
	for(BaseWindow* window : toolWindows) {
		if(dynamic_cast<MemBrowserWindow*>(window) != nullptr) {
			browser = dynamic_cast<MemBrowserWindow*>(window);
		}
	}
	if(browser == nullptr) {
		browser = new MemBrowserWindow(loadedMemoryMap, MemoryReadResolve, GetRAM, *gameconfig);
		toolWindows.push_back(browser);
	}
	browser->ShowMemory(memoryToShowAddress, memoryToShowSize);
	browser->Raise();
}

void toggleSteppingWindow() {
	if(!toolTypeIsOpen<SteppingWindow>()) {
		toolWindows.push_back(new SteppingWindow(timekeeper, loadedMemoryMap, cpu_core, *gameconfig, cartridge_state, MemoryReadResolve,
			requestShowMemory));
	} else {
		closeToolByType<SteppingWindow>();
	}
}

void togglePatchingWindow() {
	if(!toolTypeIsOpen<PatchingWindow>()) {
		toolWindows.push_back(new PatchingWindow(loadedMemoryMap, gameconfig));
	} else {
		closeToolByType<PatchingWindow>();
	}
}

void doRamDump() {
	soundcard->dump_ram("audio_debug.dat");
	ofstream dumpfile ("ram_debug.dat", ios::out | ios::binary);
	dumpfile.write((char*) system_state.ram, RAMSIZE);
	dumpfile.close();
}

// Space and Pause stop the CPU, as a breakpoint does (the code stepper's Step and Continue
// work from there), and start it again.
void togglePause() {
	bool pausing = (timekeeper.clock_mode != CLOCKMODE_STOPPED);
	timekeeper.clock_mode = pausing ? CLOCKMODE_STOPPED : CLOCKMODE_NORMAL;
	std::string title = std::string(WINDOW_TITLE) + " | " + currentRomFilePath + (pausing ? " | paused" : "");
	SDL_SetWindowTitle(mainWindow, title.c_str());
}

bool isToolWindow(Uint32 windowID) {
	for(BaseWindow* window : toolWindows) {
		if(window->OwnsWindow(windowID)) {
			return true;
		}
	}
	return false;
}

// A key typed into a debugger window's text box (a breakpoint address, say) isn't a hotkey.
bool isTypingInToolWindow(Uint32 windowID) {
	for(BaseWindow* window : toolWindows) {
		if(window->OwnsWindow(windowID) && window->IsTypingText()) {
			return true;
		}
	}
	return false;
}

void toggleControllerOptionsWindow() {
	if(!toolTypeIsOpen<ControllerOptionsWindow>()) {
		toolWindows.push_back(new ControllerOptionsWindow(joysticks));
	} else {
		closeToolByType<ControllerOptionsWindow>();
	}
}

#endif

void toggleFullScreen() {
	if(isFullScreen) {
		SDL_SetWindowFullscreen(mainWindow, 0);
		isFullScreen = false;
	} else {
		SDL_SetWindowFullscreen(mainWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
		isFullScreen = true;
	}
	timekeeper.scaling_increment = INITIAL_SCALING_INCREMENT;
}

void toggleMute() {
	muteMask = muteMask ^ MUTE_SOURCE_MANUAL;
	AudioCoprocessor::singleton_acp_state->isMuted = (muteMask != 0);
}

void setMenuMute(bool muted) {
	muteMask &= ~MUTE_SOURCE_MENU;
	if(muted) {
		muteMask |= MUTE_SOURCE_MENU;
	}
	AudioCoprocessor::singleton_acp_state->isMuted = (muteMask != 0);
}

typedef struct HotkeyAssignment {
	void (*func)();
	SDL_Keycode  key;
} HotkeyAssignment;

HotkeyAssignment hotkeys[] = {
	{&toggleFullScreen, SDLK_F11},
	{&toggleMute, SDLK_m},
#if !defined(WASM_BUILD) && !defined(WRAPPER_MODE)
	{&doRamDump, SDLK_F6},
	{&saveState, SDLK_F4},
	{&loadState, SDLK_F5},
	{&toggleSteppingWindow, SDLK_F7},
	{&takeScreenShot, SDLK_F8},
	{&toggleMemBrowserWindow, SDLK_F9},
	{&toggleVRAMWindow, SDLK_F10},
	{&toggleProfilerWindow, SDLK_F12},
#endif
};

bool checkHotkey(SDL_Keycode  key) {
	for(HotkeyAssignment assignment : hotkeys) {
		if(assignment.key == key) {
			assignment.func();
			return true;
		}
	}
	return false;
}

#ifndef EM_BOOL
#define EM_BOOL int
#endif

#if !defined(WASM_BUILD) && !defined(WRAPPER_MODE)
// A toolbar button that opens and closes a tool window, drawn pressed while it's open.
void toolbarButton(const char* label, const char* tooltip, bool open, void (*toggle)()) {
	if(open) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	}
	bool clicked = ImGui::Button(label);
	if(open) {
		ImGui::PopStyleColor();
	}
	if(ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", tooltip);
	}
	if(clicked) {
		toggle();
	}
	ImGui::SameLine();
}

// A toolbar button that does something once per click.
void toolbarActionButton(const char* label, const char* tooltip, void (*action)()) {
	bool clicked = ImGui::Button(label);
	if(ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", tooltip);
	}
	if(clicked) {
		action();
	}
	ImGui::SameLine();
}
#endif

void refreshScreen() {
	SDL_Rect src, dest, viewport_dest;
	int scr_w, scr_h;
	src.x = 0;
	src.y = (system_state.dma_control & DMA_VID_OUT_PAGE_BIT) ? GT_HEIGHT : 0;
	src.w = GT_WIDTH;
	src.h = GT_HEIGHT;
	SDL_GetWindowSize(mainWindow, &scr_w, &scr_h);
	scr_h -= topBarsHeight;
	int scale_h = scr_h / GT_HEIGHT;
	int scale_w = (scr_w * VIEWPORT_ASPECT_H) / (GT_HEIGHT * VIEWPORT_ASPECT_W);
	int scale = min(scale_h, scale_w);
	if(scale < MIN_DISPLAY_SCALE) scale = MIN_DISPLAY_SCALE;
	viewport_dest.w = viewport_width(scale);
	viewport_dest.h = viewport_height(scale);
	viewport_dest.x = (scr_w - viewport_dest.w) / 2;
	viewport_dest.y = topBarsHeight + (scr_h - viewport_dest.h) / 2;
	lastViewport = viewport_dest;
	dest.w = GT_WIDTH * scale;
	dest.h = GT_HEIGHT * scale;
	dest.x = viewport_dest.x + (viewport_dest.w - dest.w) / 2;
	dest.y = viewport_dest.y;
	SDL_UpdateTexture(framebufferTexture, NULL, vRAM_Surface->pixels, vRAM_Surface->pitch);

	SDL_RenderClear(mainRenderer);
	SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);

	if(grid_mode != GRID_NONE && scale >= MIN_DISPLAY_SCALE) {
		static const Uint8 grid_alphas[] = { 0, 64, 128, 255 };
		rebuildGridOverlay(scale);
		SDL_SetTextureAlphaMod(gridOverlayTexture, grid_alphas[grid_mode]);
		SDL_SetTextureBlendMode(gridOverlayTexture, SDL_BLENDMODE_BLEND);
		SDL_RenderCopy(mainRenderer, gridOverlayTexture, NULL, &dest);
	}
	
	src.x = GT_WIDTH-1;
	src.w = 1;
	dest.w = dest.w * 86.0 / 512.0;
	dest.x -= dest.w;

	if(showScreenSides) {
		SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);
	}

	dest.x += dest.w + dest.h;

	if(showScreenSides) {
		SDL_RenderCopy(mainRenderer, framebufferTexture, &src, &dest);
	}

	if(showScreenSides && (grid_mode != GRID_NONE) && (scale >= MIN_DISPLAY_SCALE)) {
		static const Uint8 grid_alphas[] = { 0, 64, 128, 255 };
		src.x = 0;
		src.y = 0;
		src.w = dest.w;
		src.h = dest.h;
		SDL_SetTextureAlphaMod(gridOverlayTexture, grid_alphas[grid_mode]);
		SDL_SetTextureBlendMode(gridOverlayTexture, SDL_BLENDMODE_BLEND);
		SDL_RenderCopy(mainRenderer, gridOverlayTexture, &src, &dest);
		dest.x -= dest.w + dest.h;
		src.x += scale - (dest.w % scale);
		SDL_RenderCopy(mainRenderer, gridOverlayTexture, &src, &dest);
	}

#if !defined(WASM_BUILD)
	ImGui::SetCurrentContext(main_imgui_ctx);
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	if(menuBarAlwaysShown || showMenu) {
#ifndef WRAPPER_MODE
		int menu_height = 0;
		if(ImGui::BeginMainMenuBar()) {
			menu_height = (int) ImGui::GetWindowSize().y;
			if (ImGui::BeginMenu("File")) {
				if(ImGui::MenuItem("Open Rom")) {
					const char* rom_file_name = open_rom_dialog();
					if(rom_file_name) {
						LoadRomFile(rom_file_name);
					}	
				}
				ImGui::Separator();
				if(ImGui::MenuItem("Save State", "F4")) {
					saveState();
				}
				if(ImGui::MenuItem("Load State", "F5")) {
					loadState();
				}
				ImGui::Separator();
				if(ImGui::MenuItem("Exit")) {
					running = false;
				}
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu("Settings")) {
				if(ImGui::MenuItem("Controllers")) {
					toggleControllerOptionsWindow();
				}
				ImGui::MenuItem("Toggle Instant Blits", NULL, &(blitter->instant_mode));
				ImGui::SliderInt("Volume", &AudioCoprocessor::singleton_acp_state->volume, 0, 256);
				ImGui::Checkbox("Mute", &AudioCoprocessor::singleton_acp_state->isMuted);
				if (ImGui::BeginMenu("Paddle Support")) {
					if (ImGui::Checkbox("Enable Mouse Paddle", &mouse_paddle_enabled)) {
						joysticks->SetHeldButtons(0);//clear bits on change just in case
					}
					if (ImGui::Checkbox("Enable Joystick Paddle", &joystick_paddle_enabled)) {
						joysticks->SetHeldButtons(0);//clear bits on change just in case
					}
					ImGui::EndMenu();
				}
				if(ImGui::BeginMenu("Pallete")) {
					ImGui::RadioButton("Unscaled Capture", &palette_select, PALETTE_SELECT_CAPTURE);
					ImGui::RadioButton("Full Contrast", &palette_select, PALETTE_SELECT_SCALED);
					ImGui::RadioButton("Cheap HDMI converter", &palette_select, PALETTE_SELECT_HDMI);
					ImGui::RadioButton("Flawed Theory (Legacy)", &palette_select, PALETTE_SELECT_OLD);
					ImGui::EndMenu();
				}
				if(ImGui::BeginMenu("Scale")) {
					int prev_scale = display_scale;
					ImGui::RadioButton("2x", &display_scale, 2);
					ImGui::RadioButton("3x", &display_scale, 3);
					ImGui::RadioButton("4x", &display_scale, 4);
					ImGui::RadioButton("5x", &display_scale, 5);
					ImGui::RadioButton("6x", &display_scale, 6);
					if(display_scale != prev_scale && !isFullScreen) {
						SDL_SetWindowSize(mainWindow, viewport_width(display_scale), viewport_height(display_scale) + topBarsHeight);
					}
					ImGui::EndMenu();
				}
				if(ImGui::BeginMenu("Grid")) {
					ImGui::RadioButton("No grid", &grid_mode, GRID_NONE);
					ImGui::RadioButton("25% opacity", &grid_mode, GRID_25);
					ImGui::RadioButton("50% opacity", &grid_mode, GRID_50);
					ImGui::RadioButton("Full black grid", &grid_mode, GRID_FULL);
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu("Tools")) {
				if(ImGui::MenuItem("Profiler (F12)")) {
					toggleProfilerWindow();
				}
				if(ImGui::MenuItem("Memory Browser (F9)")) {
					toggleMemBrowserWindow();
				}
				if(ImGui::MenuItem("VRAM Viewer (F10)")) {
					toggleVRAMWindow();
				}
				if(ImGui::MenuItem("Debugger (F7)")) {
					toggleSteppingWindow();
				}
				if(ImGui::MenuItem("Patching Window")) {
					togglePatchingWindow();
				}
				if(ImGui::MenuItem("Update Patches")) {
					gameconfig->UpdateAllPatches(cartridge_state.rom);
				}
				if(ImGui::MenuItem("Dump RAM to file (F6)")) {
					doRamDump();
				}
				if(ImGui::BeginMenu("Deep Profile")) {
					if(ImGui::MenuItem("Single Vsync")) {
						vsyncProfileArmed = true;
					}
					if(ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Capture function call timing between the next two VSyncs");
					}
					if(ImGui::MenuItem("Single Frame Flip")) {
						armFrameFlipProfile(1);
					}
					if(ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Capture function call timing between the next two frame flips. Capped at 8 VSync periods");
					}
					if(ImGui::BeginMenu("Several Frame Flips")) {
						for(int frames : SEVERAL_FRAME_FLIP_COUNTS) {
							std::string label = std::to_string(frames) + " frames";
							if(ImGui::MenuItem(label.c_str())) {
								armFrameFlipProfile(frames);
							}
							if(ImGui::IsItemHovered()) {
								ImGui::SetTooltip("Capture function call timing over the next %d frame flips. Capped at 8 VSync periods a frame", frames);
							}
						}
						ImGui::EndMenu();
					}
					ImGui::EndMenu();
				}
				ImGui::Separator();
				if(ImGui::MenuItem("Display Screen Sides", nullptr, &showScreenSides)) {
					Settings::SaveValue(MAIN_WINDOW_SETTINGS, SCREEN_SIDES_KEY, showScreenSides ? 1 : 0);
				}
				if(ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Fill the margins beside the screen with its rightmost column, stretched");
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}

		// The toolbar, under the menu bar: a button per tool window, drawn pressed while it's open.
		ImGui::SetNextWindowPos(ImVec2(0, (float) menu_height));
		// A height of 0 fits the buttons.
		ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
		ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
			| ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
		toolbarButton("Debugger", "Debugger (F7)", toolTypeIsOpen<SteppingWindow>(), toggleSteppingWindow);
		toolbarButton("Memory", "Memory Browser (F9)", toolTypeIsOpen<MemBrowserWindow>(), toggleMemBrowserWindow);
		toolbarButton("VRAM", "VRAM Viewer (F10)", toolTypeIsOpen<VRAMWindow>(), toggleVRAMWindow);
		toolbarButton("Profiler", "Profiler (F12)", toolTypeIsOpen<ProfilerWindow>(), toggleProfilerWindow);
		toolbarButton("Patching", "Patching Window", toolTypeIsOpen<PatchingWindow>(), togglePatchingWindow);
		// The save state buttons, at the right end.
		const ImGuiStyle& style = ImGui::GetStyle();
		float right_buttons_width = ImGui::CalcTextSize("Save").x + ImGui::CalcTextSize("Load").x
			+ (style.FramePadding.x * 4) + style.ItemSpacing.x;
		// Never over the tool buttons, in a narrow window.
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - right_buttons_width));
		toolbarActionButton("Save", "Save State: to the ROM's .state file (F4)", saveState);
		toolbarActionButton("Load", "Load State: from the ROM's .state file (F5)", loadState);
		int toolbar_height = (int) ImGui::GetWindowSize().y;
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);

		int height = menu_height + toolbar_height;
		if(height != topBarsHeight) {
			// The window grows by the bars, so the game keeps its size below them.
			int window_w, window_h;
			SDL_GetWindowSize(mainWindow, &window_w, &window_h);
			if(!isFullScreen) {
				SDL_SetWindowSize(mainWindow, window_w, window_h + height - topBarsHeight);
			}
			SDL_SetWindowMinimumSize(mainWindow, viewport_width(MIN_DISPLAY_SCALE), viewport_height(MIN_DISPLAY_SCALE) + height);
			topBarsHeight = height;
		}
#else
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.5f)); // 50% transparent black
		ImGui::Begin("OverlayBackground", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoInputs | 
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus);
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);

		// Now create the actual menu window in the top-left corner
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.9f);
		if(!ImGui::IsAnyItemFocused()) {
			ImGui::SetNextWindowFocus();
		}
		menuOpening = false;
		ImGui::Begin("MainMenu", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoTitleBar);

			ImGui::SetWindowFontScale(2.0f);

		if (ImGui::IsWindowAppearing())
    		ImGui::SetKeyboardFocusHere(-1);
		

		if (ImGui::BeginMenu("Options")) {
			// These are items inside the pop-out menu
			if (ImGui::MenuItem("Toggle Full Screen")) {
				toggleFullScreen();
			}
			ImGui::SliderInt("Volume", &AudioCoprocessor::singleton_acp_state->volume, 0, 256, "", ImGuiSliderFlags_NoInput);
			bool appMute = (muteMask & MUTE_SOURCE_MANUAL) != 0;
			ImGui::Checkbox("Mute Audio", &appMute);
			if(appMute) muteMask |= MUTE_SOURCE_MANUAL;
			else muteMask &= ~MUTE_SOURCE_MANUAL;
			AudioCoprocessor::singleton_acp_state->isMuted = (muteMask != 0);
			ImGui::Separator();
			if (ImGui::BeginMenu("Paddle Support")) {
					if (ImGui::Checkbox("Enable Mouse Paddle", &mouse_paddle_enabled)) {
						joysticks->SetHeldButtons(0);//clear bits on change just in case
					}
					if (ImGui::Checkbox("Enable Joystick Paddle", &joystick_paddle_enabled)) {
						joysticks->SetHeldButtons(0);//clear bits on change just in case
					}
					ImGui::EndMenu();
				}
			ImGui::EndMenu();
		}

		if(ImGui::Selectable("Reset")) {
			resetQueued = 2;
			showMenu = false;
			setMenuMute(showMenu);
			joysticks->Reset();
		}

		if(ImGui::Selectable("Exit")) {
			running = false;
		}

		ImGui::End();
#endif
	}
	ImGui::Render();
	ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), mainRenderer);
#endif
	SDL_RenderPresent(mainRenderer);
}

char titlebuf[256];
int32_t intended_cycles = 0;

#ifdef WASM_BUILD
double target_frame_period_ms = 1000.0 / 60.0;
double last_raf_time = 0;
double frame_time_accumulator = 0;
#endif

EM_BOOL mainloop(double time, void* userdata) {
#ifdef WASM_BUILD
        double delta_time = time - last_raf_time;
        frame_time_accumulator += delta_time;
        last_raf_time = time;
        if(frame_time_accumulator < target_frame_period_ms) {
                return true;
        }
        frame_time_accumulator -= target_frame_period_ms;
#else 

if (mouse_paddle_enabled) {
	if (paddle_touch_mode){ //touch / absolute
		int mx, my, winW, winH;
		SDL_GetMouseState(&mx, &my);
		SDL_GetWindowSize(mainWindow, &winW, &winH);
		joysticks->UpdatePaddleFromCursorPos(0, mx, winW);
	} else { //mouse mode delta
		if (showMenu) {
			if (SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_FALSE);
		} else {
			if (!SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_TRUE);
			int dx, dy;
			SDL_GetRelativeMouseState(&dx, &dy);
			joysticks->UpdatePaddleFromMouse(0, dx);
		}
	}
}//mouse paddle emulation
else if (joystick_paddle_enabled) {
		// We treat the full joystick range as our "Window Width"
		// Logical range of SDL Axis is 65535 units wide
		const int virtualWidth = 65535;
		// Offset the raw value (-32768 to 32767) to be 0 to 65535
		int normalizedX = currentPaddleRawValue + 32768;
		joysticks->UpdatePaddleFromCursorPos(0, normalizedX, virtualWidth);
} 
else {
    if(SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_FALSE);
}
	
#endif

#ifdef WRAPPER_MODE
	if(!paused && !showMenu) {
#else
	if(!paused) {
#endif
			timekeeper.actual_cycles = timekeeper.totalCyclesCount;
#ifndef WASM_BUILD
			switch(timekeeper.clock_mode) {
				case CLOCKMODE_NORMAL:
				case CLOCKMODE_STEPSOURCE:
				case CLOCKMODE_STEPOUT:
					cpu_core->freeze = false;
					intended_cycles = timekeeper.cycles_per_vsync;
					break;
				case CLOCKMODE_SINGLE:
					cpu_core->freeze = false;
					Disassembler::Decode(MemoryReadResolve, loadedMemoryMap, cpu_core->pc, 32);
					intended_cycles = 1;
					timekeeper.clock_mode = CLOCKMODE_STOPPED;
					break;
				case CLOCKMODE_STOPPED:
					intended_cycles = 0;
					break;
			}
			if(intended_cycles) {
				if(vsyncNmiPending) {
					cpu_core->NMI();
					vsyncNmiPending = false;
				}
				cpu_core->Run(intended_cycles, timekeeper.totalCyclesCount);
			}
#else
			intended_cycles = timekeeper.cycles_per_vsync;
			cpu_core->Run(intended_cycles, timekeeper.totalCyclesCount);
#endif
			timekeeper.actual_cycles = timekeeper.totalCyclesCount - timekeeper.actual_cycles;
			if(cpu_core->illegalOpcode) {
				printf("Hit illegal opcode %x\npc = %x\n", cpu_core->illegalOpcodeSrc, cpu_core->pc);
				PauseEmulation();
			} else if((timekeeper.clock_mode == CLOCKMODE_NORMAL) && (timekeeper.actual_cycles == 0)) {
				profiler.zeroConsec++;
				if(profiler.zeroConsec == 10) {
					printf("(Got stuck at 0x%x)\n", cpu_core->pc);
					PauseEmulation();
				}
				timekeeper.totalCyclesCount += intended_cycles;
			} else {
				profiler.zeroConsec = 0;
			}

#ifndef WASM_BUILD
			if(!gofast) {
				SDL_Delay(timekeeper.time_scaling * intended_cycles/timekeeper.system_clock);
			} else {
				timekeeper.lastTicks = 0;
			}
			timekeeper.currentTicks = SDL_GetTicks();

			if(timekeeper.clock_mode == CLOCKMODE_NORMAL) {
				if(timekeeper.lastTicks != 0) {
					int time_error = (timekeeper.currentTicks - timekeeper.lastTicks) - (1000 * intended_cycles/timekeeper.system_clock);
					if(timekeeper.frameCount == 100) {
#ifndef WRAPPER_MODE						
					  sprintf(titlebuf, "%s | %s | s: %.1f inc: %.1f err: %d\n", WINDOW_TITLE, currentRomFilePath.c_str(), timekeeper.time_scaling, timekeeper.scaling_increment, time_error);
						SDL_SetWindowTitle(mainWindow, titlebuf);
#endif
						profiler.fps = profiler.bufferFlipCount * 60 / 100;
						timekeeper.frameCount = 0;
						profiler.bufferFlipCount = 0;
					}
					bool overlong = time_error > 0;

					if(overlong == timekeeper.prev_overlong) {
						//scaling_increment = 1;
					} else if(timekeeper.scaling_increment > 1) {
						timekeeper.scaling_increment -= 1;
					}
					if((timekeeper.scaling_increment > 1) || (abs(time_error) > 2)) {
						if(overlong) {
							timekeeper.time_scaling -= timekeeper.scaling_increment;
						} else {
							timekeeper.time_scaling += timekeeper.scaling_increment;
						}
					}
					timekeeper.prev_overlong = overlong;

					if(timekeeper.time_scaling < 100) {
						timekeeper.time_scaling = 100;
					} else if(timekeeper.time_scaling > 2000) {
						timekeeper.time_scaling = 2000;
					}
				}
				timekeeper.lastTicks = timekeeper.currentTicks;
				timekeeper.frameCount++;
			}
#endif
			// Running, a pass counts as the cycles asked for. Stopped part-way (a breakpoint, or
			// the end of a step), only what ran counts, so vsync still comes at the right time
			// while debugging.
			uint64_t elapsed_cycles = (timekeeper.clock_mode == CLOCKMODE_STOPPED) ? timekeeper.actual_cycles : intended_cycles;
			timekeeper.totalCyclesCount -= timekeeper.actual_cycles;
			timekeeper.totalCyclesCount += elapsed_cycles;
			timekeeper.cycles_since_vsync += elapsed_cycles;
			if(timekeeper.cycles_since_vsync >= timekeeper.cycles_per_vsync) {
				timekeeper.cycles_since_vsync -= timekeeper.cycles_per_vsync;
				if(system_state.dma_control & DMA_VSYNC_NMI_BIT) {
					if(timekeeper.clock_mode == CLOCKMODE_STOPPED) {
						vsyncNmiPending = true;
					} else {
						cpu_core->NMI();
					}
					if(vsyncProfileArmed) {
						profiler.DeepProfileStart();
						vsyncProfileArmed = false;
						vsyncProfileRunning = true;
					} else if(vsyncProfileRunning) {
						profiler.DeepProfileStop(loadedMemoryMap, SourceMap::singleton);
						vsyncProfileRunning = false;
					} else if(frameFlipProfileRunning) {
						if(frameFlipProfileVsyncLimit == 0) {
							profiler.DeepProfileStop(loadedMemoryMap, SourceMap::singleton);
							frameFlipProfileRunning = false;
						} else {
							--frameFlipProfileVsyncLimit;
						}
					}
				}
				if(!profiler.measure_by_frameflip) {
					profiler.ResetTimers();
					profiler.last_blitter_activity = blitter->pixels_this_frame;
					blitter->pixels_this_frame = 0;
				}
			}
		} else {
				SDL_Delay(16);
		}
		blitter->CatchUp();
		

		if(EmulatorConfig::noSound) {
			AudioCoprocessor::fill_audio(AudioCoprocessor::singleton_acp_state, NULL, AudioCoprocessor::singleton_acp_state->samples_per_frame);
		}

		while( SDL_PollEvent( &e ) != 0 )
        {
#ifndef WASM_BUILD

#ifdef WRAPPER_MODE
			if(true){
#else
			if(SDL_GetMouseFocus() == mainWindow) {
#endif
				ImGui::SetCurrentContext(main_imgui_ctx);
				ImPlot::SetCurrentContext(main_implot_ctx);
				ImGui_ImplSDL2_ProcessEvent(&e);
			}
			for (auto toolWindow : toolWindows) {
				toolWindow->HandleEvent(e);
			}

			ImGui::SetCurrentContext(main_imgui_ctx);
			ImPlot::SetCurrentContext(main_implot_ctx);

#ifndef WRAPPER_MODE
			if(ImGui::GetIO().WantCaptureKeyboard && ((e.type == SDL_KEYDOWN) || (e.type == SDL_KEYUP))) {
				continue;
			}
#endif //WRAPPER_MODE
#endif //WASM_BUILD
            //User requests quit
            if( e.type == SDL_QUIT )
            {
               running = false;
            } else if(e.type == SDL_WINDOWEVENT)
			{
				if(e.window.event == SDL_WINDOWEVENT_CLOSE) {
					SDL_Window* closedWindow = SDL_GetWindowFromID(e.window.windowID);
					if(closedWindow == mainWindow) {
						running = false;
					}
				}
				else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
					SDL_SetRelativeMouseMode(SDL_FALSE);
				} 
				else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
					if (mouse_paddle_enabled && !paddle_touch_mode) {
						SDL_SetRelativeMouseMode(SDL_TRUE);
					}
				}

			} else if((e.type == SDL_KEYDOWN) || (e.type == SDL_KEYUP)) {
				// A key typed into a debugger window's text box (a search, an address) is text, not a
				// hotkey (R resets) or a game button. Key releases still go through, so a held button
				// is let go.
				// The same for a debugger window's own Ctrl shortcuts (the memory browser's Ctrl+C and
				// Ctrl+G), all but Ctrl+R, which reloads the ROM from any window.
				bool typedIntoToolWindow = false;
	#ifndef WASM_BUILD
				bool toolWindowShortcut = (e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.sym != SDLK_r)
					&& isToolWindow(e.key.windowID);
				typedIntoToolWindow = (e.type == SDL_KEYDOWN) && (isTypingInToolWindow(e.key.windowID) || toolWindowShortcut);
	#endif
				if((e.key.repeat == 0) && !typedIntoToolWindow) {
					if((e.type == SDL_KEYUP) || !checkHotkey(e.key.keysym.sym)) {
						switch(e.key.keysym.sym) {
							case SDLK_LSHIFT:
								lshift = (e.type == SDL_KEYDOWN);
								break;
							case SDLK_RSHIFT:
								rshift = (e.type == SDL_KEYDOWN);
								break;							
							case SDLK_ESCAPE:
	#if !defined(DISABLE_ESC)
								// With the menu bar always shown, there's no menu to open.
								if((e.type == SDL_KEYDOWN) && !menuBarAlwaysShown) {
									showMenu = !showMenu;
									menuOpening = showMenu;
	#ifdef WRAPPER_MODE
									setMenuMute(showMenu);
	#endif
								}
								#endif
								break;
	#ifndef WRAPPER_MODE
							case SDLK_BACKQUOTE:
								gofast = (e.type == SDL_KEYDOWN);
								break;
							case SDLK_r:
								//TODO add menu item for reset
								if(e.type == SDL_KEYDOWN) {
									if(e.key.keysym.mod & KMOD_CTRL) {
										reloadQueued = true;
										resetQueued = 2;
									} else if(lshift || rshift) {
										resetQueued = 2;
									} else {
										resetQueued = 1;
									}
								}
								break;
	#ifndef WASM_BUILD
							case SDLK_SPACE:
							case SDLK_PAUSE:
								if(e.type == SDL_KEYDOWN) {
									togglePause();
								}
								break;
	#endif
							case SDLK_o:
								if(e.type == SDL_KEYDOWN) {
									const char* rom_file_name = open_rom_dialog();
									if(rom_file_name) {
										LoadRomFile(rom_file_name);
									} else {
	#ifdef TINYFILEDIALOGS_H
										tinyfd_notifyPopup("Alert",
										"No ROM was loaded",
										"warning");
	#endif
									}
								}
								break;
	#endif
							default:
								joysticks->update(&e, showMenu || resetQueued);
								break;
						}
					}
				}
            }
#ifndef WASM_BUILD
			else if (e.type == SDL_JOYAXISMOTION) {
				if (paddleDetected && e.jaxis.axis == 0) {
					if ( e.jaxis.which == paddle_instanceID) {
						currentPaddleRawValue = e.jaxis.value;
					}               
				}
            } else if (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) {
                if (paddleDetected && e.jbutton.button == 0) {
					bool isDown = (e.type == SDL_JOYBUTTONDOWN);
					joysticks->SetPaddleAButtonDirect(isDown);
                }
			 } else if (e.type == SDL_JOYDEVICEREMOVED) {
				if (paddleDetected && e.jdevice.which == paddle_instanceID) {
					paddleDetected = false;
					paddle_instanceID = -1; // Reset it
					printf("Paddle/JoyStick Disconnected\n");
				}
				PaddleInit();
			} else if (e.type == SDL_JOYDEVICEADDED) {
				PaddleInit();
            } 
#endif
			 else {
				joysticks->update(&e, showMenu || resetQueued);
			}
        }

		if(joysticks->CheckSystemButtonPressed() && !menuBarAlwaysShown) {
	#if !defined(DISABLE_ESC)
									showMenu = !showMenu;
									menuOpening = showMenu;
	#ifdef WRAPPER_MODE
									setMenuMute(showMenu);
	#endif
	#endif
		}

		refreshScreen();
		SDL_UpdateWindowSurface(mainWindow);

#ifndef WASM_BUILD
		for (auto& window : toolWindows) {
			if(window->IsOpen()) {
				window->Draw();
			}
		}

		auto const to_be_removed = std::partition(begin(toolWindows), end(toolWindows), [](auto w){ return w->IsOpen(); });
		std::for_each(to_be_removed, end(toolWindows), [](auto w) {
			delete w;
		});
		toolWindows.erase(to_be_removed, end(toolWindows));
		showRequestedMemory();
#endif
		
	if(!running) {
#ifdef WASM_BUILD
		emscripten_cancel_main_loop();
#else
		for (auto& window : toolWindows) {
			delete window;
		}
		toolWindows.clear();

		ImGui::SetCurrentContext(main_imgui_ctx);
		ImPlot::SetCurrentContext(main_implot_ctx);
		ImPlot::DestroyContext(main_implot_ctx);
		ImGui_ImplSDLRenderer2_Shutdown();
    	ImGui_ImplSDL2_Shutdown();
    	ImGui::DestroyContext(main_imgui_ctx);
#endif
    	SDL_DestroyRenderer(mainRenderer);
		SDL_DestroyWindow(mainWindow);
	}

	if(reloadQueued) {
		reloadQueued = false;
		// LoadRomFile takes the path by pointer and replaces currentRomFilePath.
		std::string romFilePath = currentRomFilePath;
		if(LoadRomFile(romFilePath.c_str()) == -1) {
#ifdef TINYFILEDIALOGS_H
			tinyfd_notifyPopup("Alert", "Couldn't reload the ROM", "warning");
#endif
		}
	}
	if(resetQueued) {
		ResumeEmulation();
		if(lshift || rshift || (resetQueued == 2)) {
			randomize_memory();
			randomize_vram();
		}
		cpu_core->Reset();
		cartridge_state.write_mode = false;
		joysticks->SetHeldButtons(0);//clear paddle bits before reset
		joysticks->Reset();
		resetQueued = 0;
		currentPaddleRawValue = 0;
	}
	return running;
}

int main(int argC, char* argV[]) {
	srand(time(NULL));
	cartridge_state.rom = new uint8_t[1 << 21];

	const char* rom_file_name = NULL;

#ifdef EMBED_ROM_FILE
	rom_file_name = EMBED_ROM_FILE;
#else
	for(int argIdx = 1; argIdx < argC; ++argIdx) {
		if((argV[argIdx])[0] == '-') {
			EmulatorConfig::parseArg(argV[argIdx]);
		} else if(!rom_file_name) {
			rom_file_name = argV[argIdx];
		}
	}
#endif

#ifdef DEFAULT_ROM_PATH
	if(argC == 1) {
		int execPathLength = wai_getExecutablePath(NULL, 0, NULL);
		if(execPathLength != -1) {
			char* path = (char*)malloc(execPathLength + 1);
			wai_getExecutablePath(path, execPathLength, NULL);
			path[execPathLength] = '\0';
			std::filesystem::path execPath(path);
			free(path);
			std::filesystem::path romPath = execPath.parent_path() / DEFAULT_ROM_PATH;
			std::string romPathStr = (execPath.parent_path() / DEFAULT_ROM_PATH).string();
			rom_file_name = strdup(romPathStr.c_str());
		}
	}
#endif

	//cartridge_state.rom = new uint8_t [cartridge_state.size];
		for(int i = 0; i < cartridge_state.size; i++) {
			cartridge_state.rom[i] = 0;
		}

	
	soundcard = new AudioCoprocessor();
	cpu_core = new mos6502(MemoryRead, MemoryWrite, CPUStopped, MemorySync);
	cpu_core->Reset();
	cartridge_state.write_mode = false;
	blitter = new Blitter(cpu_core, &timekeeper, &system_state, vRAM_Surface);
	randomize_memory();
	
	SDL_Init(SDL_INIT_VIDEO);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	atexit(SDL_Quit);

	bmpFont = SDL_CreateRGBSurfaceFrom(font_map, 128, 128, 32, 4 * 128, rmask, gmask, bmask, amask);

	vRAM_Surface = SDL_CreateRGBSurface(0, GT_WIDTH, GT_HEIGHT * 2, 32, rmask, gmask, bmask, amask);
	gRAM_Surface = SDL_CreateRGBSurface(0, GT_WIDTH, GT_HEIGHT * 32, 32, rmask, gmask, bmask, amask);

	SDL_SetColorKey(vRAM_Surface, SDL_FALSE, 0);
	SDL_SetColorKey(gRAM_Surface, SDL_FALSE, 0);

#ifndef WASM_BUILD
	int screenSidesShown;
	if(Settings::LoadValue(MAIN_WINDOW_SETTINGS, SCREEN_SIDES_KEY, screenSidesShown)) {
		showScreenSides = (screenSidesShown != 0);
	}
#endif
	mainWindow = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, viewport_width(display_scale), viewport_height(display_scale), SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	SDL_SetWindowMinimumSize(mainWindow, viewport_width(MIN_DISPLAY_SCALE), viewport_height(MIN_DISPLAY_SCALE));
	mainRenderer = SDL_CreateRenderer(mainWindow, -1, EmulatorConfig::defaultRendererFlags);
	framebufferTexture = SDL_CreateTexture(mainRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, GT_WIDTH, GT_HEIGHT * 2);
	SDL_SetTextureScaleMode(framebufferTexture, SDL_ScaleModeNearest);

#ifndef WASM_BUILD
	main_imgui_ctx = ImGui::CreateContext();
	main_implot_ctx = ImPlot::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigViewportsNoAutoMerge = true;
	io.IniFilename = NULL;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;
	ImGui::StyleColorsDark();
	ImGui_ImplSDL2_InitForSDLRenderer(mainWindow, mainRenderer);
	ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual);
	ImGui_ImplSDLRenderer2_Init(mainRenderer);
#endif

	//Init joystick handler AFTER init imgui
	joysticks = new JoystickAdapter();

	#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	    rmask = 0xff000000;
	    gmask = 0x00ff0000;
	    bmask = 0x0000ff00;
	    amask = 0x000000ff;
	#else
	    rmask = 0x000000ff;
	    gmask = 0x0000ff00;
	    bmask = 0x00ff0000;
	    amask = 0xff000000;
	#endif

	randomize_vram();

	if(!rom_file_name || LoadRomFile(rom_file_name) == -1) {
		PauseEmulation();
#ifdef TINYFILEDIALOGS_H
		if(rom_file_name) {
			tinyfd_notifyPopup("Alert",
			"No ROM was loaded",
			"warning");
		}
#endif
		
	} else {
		ResumeEmulation();
	}

#ifdef WASM_BUILD

	emscripten_request_animation_frame_loop(mainloop, 0);
#else
	PaddleInit();
	SDL_RaiseWindow(mainWindow);
	while(running) {
		mainloop(0, NULL);
	}
	joysticks->SaveBindings();
#endif

#ifndef WASM_BUILD
	if(savingThread.joinable()) {
		savingThread.join();
	}
#endif
	return 0;
}
