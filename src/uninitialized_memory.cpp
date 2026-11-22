#include "uninitialized_memory.h"
#include <algorithm>
#include <cstdio>
#include <mutex>

#define AUDIO_RAM_ADDRESS 0x3000
#define GRAPHICS_RAM_ADDRESS 0x4000
// A graphics RAM offset: the bank, the quadrant (bit 0: its right half, bit 1: its lower half),
// the row and the column.
#define GRAM_BANK_SHIFT 16
#define GRAM_QUADRANT_SHIFT 14
#define GRAM_QUADRANT_MASK 3
#define GRAM_ROW_SHIFT 7
#define GRAM_QUADRANT_SIZE 128
#define GRAM_QUADRANT_OFFSET_MASK 0x3FFF

std::atomic<bool> UninitializedMemory::enabled(false);
std::string UninitializedMemory::message;

static std::mutex report_mutex;
static bool report_waiting = false;
static UninitializedMemory::Access waiting_report;

void UninitializedMemory::Report(const Access& access) {
    std::lock_guard<std::mutex> lock(report_mutex);
    if(report_waiting) {
        return;
    }
    waiting_report = access;
    report_waiting = true;
}

bool UninitializedMemory::TakeReport(Access& access) {
    std::lock_guard<std::mutex> lock(report_mutex);
    if(!report_waiting) {
        return false;
    }
    access = waiting_report;
    report_waiting = false;
    return true;
}

// Three quarters of an 8K RAM bank: more than any program's variables, which leave room for a
// stack.
#define WIPE_BYTE_COUNT 6144
// How long after a reset a wipe can be: about half a second of CPU time.
#define WIPE_WINDOW_CYCLES 2000000

RamWipeDetector::RamWipeDetector(uint32_t ramSize) : ram_size(ramSize) {
    for(Run& run : runs) {
        run.written.assign(ramSize, 0);
    }
}

void RamWipeDetector::Restart(uint64_t cycle) {
    reset_cycle = cycle;
    for(Run& run : runs) {
        run.active = false;
    }
}

void RamWipeDetector::Run::Start(uint16_t newPc, uint8_t newValue) {
    active = true;
    pc = newPc;
    value = newValue;
    written_count = 0;
    is_wipe = false;
    ++generation;
    if(generation == 0) {
        // Wrapped: stale marks from 65535 runs ago mustn't match again.
        std::fill(written.begin(), written.end(), 0);
        generation = 1;
    }
}

void RamWipeDetector::Write(uint64_t cycle, uint16_t pc, uint32_t index, uint8_t value, bool* initialized) {
    if(cycle - reset_cycle > WIPE_WINDOW_CYCLES) {
        return;
    }
    Run* run = nullptr;
    Run* least_recent = &runs[0];
    for(Run& candidate : runs) {
        if(candidate.active && (candidate.pc == pc)) {
            run = &candidate;
        } else if(candidate.active && candidate.Has(index)) {
            // Another instruction has written over this run's byte: it's that one's now.
            candidate.written[index] = 0;
            --candidate.written_count;
        }
        if(candidate.last_used < least_recent->last_used) {
            least_recent = &candidate;
        }
    }
    if(run == nullptr) {
        run = least_recent;
        run->Start(pc, value);
    } else if(run->value != value) {
        run->Start(pc, value);
    }
    run->last_used = ++use_count;
    if(!run->Has(index)) {
        run->written[index] = run->generation;
        ++run->written_count;
    }

    if(run->is_wipe) {
        initialized[index] = false;
    } else if(run->written_count >= WIPE_BYTE_COUNT) {
        run->is_wipe = true;
        for(uint32_t wiped = 0; wiped < ram_size; ++wiped) {
            if(run->Has(wiped)) {
                initialized[wiped] = false;
            }
        }
    }
}

std::string UninitializedMemory::Describe(const Access& access) {
    char text[256];
    switch(access.kind) {
        case Kind::RAM:
            snprintf(text, sizeof text,
                "Attempted to access uninitialized memory location $%04X (by the instruction at $%04X)",
                (unsigned) access.location, access.pc);
            break;
        case Kind::AudioRAM:
            if(access.reader == Reader::AudioCPU) {
                snprintf(text, sizeof text,
                    "The audio coprocessor attempted to access uninitialized memory location $%04X of its RAM (near its $%04X)",
                    (unsigned) access.location, access.pc);
            } else {
                snprintf(text, sizeof text,
                    "Attempted to access uninitialized memory location $%04X, in audio RAM (by the instruction at $%04X)",
                    (unsigned) (AUDIO_RAM_ADDRESS + access.location), access.pc);
            }
            break;
        case Kind::GraphicsRAM: {
            unsigned bank = access.location >> GRAM_BANK_SHIFT;
            unsigned quadrant = (access.location >> GRAM_QUADRANT_SHIFT) & GRAM_QUADRANT_MASK;
            unsigned x = (access.location % GRAM_QUADRANT_SIZE) + ((quadrant & 1) * GRAM_QUADRANT_SIZE);
            unsigned y = ((access.location >> GRAM_ROW_SHIFT) % GRAM_QUADRANT_SIZE)
                + ((quadrant >> 1) * GRAM_QUADRANT_SIZE);
            if(access.reader == Reader::Blitter) {
                snprintf(text, sizeof text,
                    "A blit attempted to access uninitialized graphics RAM: bank %u, x %u, y %u", bank, x, y);
            } else {
                snprintf(text, sizeof text,
                    "Attempted to access uninitialized memory location $%04X: graphics RAM bank %u, x %u, y %u (by the instruction at $%04X)",
                    (unsigned) (GRAPHICS_RAM_ADDRESS + (access.location & GRAM_QUADRANT_OFFSET_MASK)), bank, x, y,
                    access.pc);
            }
            break;
        }
    }
    return text;
}
