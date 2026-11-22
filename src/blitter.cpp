#include "blitter.h"
#include "save_state.h"
#include "uninitialized_memory.h"

Uint32 get_pixel32( SDL_Surface *surface, int x, int y )
{
    //Convert the pixels to 32 bit
    Uint32 *pixels = (Uint32 *)surface->pixels;
    
    //Get the requested pixel
    return pixels[ ( y * surface->w ) + x ];
}

void put_pixel32( SDL_Surface *surface, int x, int y, Uint32 pixel )
{
    //Convert the pixels to 32 bit
    Uint32 *pixels = (Uint32 *)surface->pixels;
    
    //Set the pixel
    pixels[ ( y * surface->w ) + x ] = pixel;
}

void Blitter::SetParam(uint8_t address, uint8_t value) {
    if((address % DMA_PARAMS_COUNT) == PARAM_TRIGGER) {
        trigger = value & 1;
        irq = false;
        cpu_core->ClearIRQ();
        if(trigger) {
            CheckSourceInitialized();
            uint32_t cycles = ((params[Blitter::PARAM_HEIGHT] & 0x7F) * (params[Blitter::PARAM_WIDTH] & 0x7F));
            cpu_core->ScheduleIRQ(cycles, &(system_state->dma_control_irq));
            if(instant_mode) {
                CatchUp(cycles);
            }
        }
    } else {
        params[address % DMA_PARAMS_COUNT] = value;
    }
}

#define ROWCOMPLETE (counterW == 0)
#define XRELOAD (ROWCOMPLETE || init)
#define COPYDONE (counterH == 0)
#define XDIR (!!(params[PARAM_WIDTH] & 0x80))
#define YDIR (!!(params[PARAM_HEIGHT] & 0x80))
#define GCARRY(val) ((system_state->dma_control & DMA_GCARRY_BIT) ? val+1 : ((val & 0xF0) | ((val+1) & 0x0F)))

uint32_t Blitter::GramIndex(uint8_t gx, uint8_t gy) {
    uint32_t gOffset = ((system_state->banking & BANK_GRAM_MASK) << 16) +
            (!!(gy & 0x80) << 15) +
            (!!(gx & 0x80) << 14);
    return ((gy & 0x7F) << 7) | (gx & 0x7F) | gOffset;
}

bool Blitter::Clipped(uint8_t vx, uint8_t vy) {
    return ((vx & 0x80) && (system_state->banking & BANK_WRAPX_MASK))
        || ((vy & 0x80) && (system_state->banking & BANK_WRAPY_MASK));
}

// Walks the blit's pixels as CatchUp will, by the parameters as they are now.
void Blitter::CheckSourceInitialized() {
    if(!UninitializedMemory::enabled) {
        return;
    }
    // A fill reads no graphics RAM, and with copying off nothing is drawn.
    if((system_state->dma_control & DMA_COLORFILL_ENABLE_BIT) || !(system_state->dma_control & DMA_COPY_ENABLE_BIT)) {
        return;
    }
    uint8_t width = params[PARAM_WIDTH] & 0x7F;
    uint8_t height = params[PARAM_HEIGHT] & 0x7F;
    bool reported = false;
    uint8_t gy = params[PARAM_GY];
    uint8_t vy = params[PARAM_VY];
    for(uint8_t row = 0; row < height; ++row) {
        uint8_t gx = params[PARAM_GX];
        uint8_t vx = params[PARAM_VX];
        for(uint8_t column = 0; column < width; ++column) {
            // A pixel that's clipped away isn't drawn from what's read, so that's no read.
            if(!Clipped(vx, vy)) {
                uint32_t index = GramIndex(XDIR ? (uint8_t) ~gx : gx, YDIR ? (uint8_t) ~gy : gy);
                // Every pixel's flag is set, so the blit is reported once: for its first.
                if(UninitializedMemory::IsFirstRead(system_state->gram_initialized[index]) && !reported) {
                    UninitializedMemory::Report({ UninitializedMemory::Kind::GraphicsRAM,
                        UninitializedMemory::Reader::Blitter, index, 0 });
                    cpu_core->Freeze();
                    reported = true;
                }
            }
            ++vx;
            gx = GCARRY(gx);
        }
        ++vy;
        gy = GCARRY(gy);
    }
}

void Blitter::CatchUp(uint64_t cycles) {
    if(cycles == 0) {
        cycles = timekeeper->totalCyclesCount - last_updated_cycle;
    }
    last_updated_cycle = timekeeper->totalCyclesCount;
    uint8_t colorbus;
    while(cycles--) {
        //PHASE 0
            //Decrement Width Counter
            //or Load if INIT
            if(running) {
                --counterW;
            }

            if(init) {
                counterW = params[PARAM_WIDTH] & 0x7F;
            }

        //PHASE 1
            //Decrement Height counter or load if INIT
            //Increment counterVX, counterVY, GX, GY counters or load if INIT
            if(running) {
                ++counterVX;
                counterGX = GCARRY(counterGX);
                
                if(ROWCOMPLETE) {
                    ++counterVY;
                    counterGY = GCARRY(counterGY);
                    --counterH;
                }
            }

            if(XRELOAD) {
                counterVX = params[PARAM_VX];
                counterGX = params[PARAM_GX];
            }

            if(init) {
                counterVY = params[PARAM_VY];
                counterGY = params[PARAM_GY];
                counterH = params[PARAM_HEIGHT] & 0x7F;
            }

            if(COPYDONE) {
                running = false;
                irq = true;
            }

        //PHASE 2
            //Reload width counter if it ran out
            //Set RUNNING and reset TRIGGER if TRIGGER is set
            running = running || init;
            if(ROWCOMPLETE) counterW = params[PARAM_WIDTH] & 0x7F;

        //PHASE 3
            //Framebuffer write strobe
            //Depending on spritecolor, transen, offscr_x/y, wrap_x/y
            //Set INIT if TRIGGER, else reset INIT
            init = trigger;
            trigger = trigger && !init;

            if(running) {
                counterGX = XDIR ? ~counterGX : counterGX;
                counterGY = YDIR ? ~counterGY : counterGY;
                gram_mid_bits = (!!(counterGY & 0x80) * 2) + (!!(counterGX & 0x80) * 1);
                if(system_state->dma_control & DMA_COLORFILL_ENABLE_BIT) {
                    colorbus = ~(params[PARAM_COLOR]);
                } else {
                    colorbus = system_state->gram[GramIndex(counterGX, counterGY)];
                }
                counterGX = XDIR ? ~counterGX : counterGX;
                counterGY = YDIR ? ~counterGY : counterGY;

                if(system_state->dma_control & DMA_COPY_ENABLE_BIT) {
                    if(((system_state->dma_control & DMA_TRANSPARENCY_BIT) || (colorbus != 0))
                        && !Clipped(counterVX, counterVY)) {
                        int yShift = (system_state->banking & BANK_VRAM_MASK) ? 128 : 0;
                        int vOffset = yShift << 7;
                        system_state->vram[((counterVY & 0x7F) << 7) | (counterVX & 0x7F) | vOffset] = colorbus;
                        put_pixel32(vram_surface, counterVX & 0x7F, (counterVY & 0x7F) + yShift, Palette::ConvertColor(vram_surface, colorbus));
                    }
                    ++pixels_this_frame;
                }
            }

    }
}

void Blitter::SerializeState(StateFile& file) {
    file.Value(counterVX);
    file.Value(counterVY);
    file.Value(counterGX);
    file.Value(counterGY);
    file.Value(counterW);
    file.Value(counterH);
    file.Value(params);
    file.Value(trigger);
    file.Value(init);
    file.Value(irq);
    file.Value(running);
    file.Value(last_updated_cycle);
    file.Value(gram_mid_bits);
    file.Value(pixels_this_frame);
}
