#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// "Break on uninitialized memory reads" (the Tools menu). Main RAM, audio RAM and graphics RAM each
// keep a flag per byte, set when the byte is written and cleared at power-on (a cold boot, or
// loading a ROM). While the option is on, whatever reads a byte whose flag is clear reports it
// here, and the emulator stops the CPU, opens the debugger and shows the report there. Each
// byte is reported once: reading it sets its flag.
//
// The flags are a byte each, not packed bits: the audio thread and the main thread both set
// audio RAM's, and neighbouring bits of one byte would trample each other.
class UninitializedMemory {
public:
    enum class Kind { RAM, AudioRAM, GraphicsRAM };
    enum class Reader { CPU, AudioCPU, Blitter };

    struct Access {
        Kind kind;
        Reader reader;
        // RAM: the CPU's address. Audio RAM: the offset into it. Graphics RAM: the offset into
        // all of it (which holds the bank, the quadrant, the row and the column).
        uint32_t location;
        // The CPU: the instruction that read it. The audio CPU: where it had got to. The
        // blitter: unused (the CPU stops at the write that started the blit).
        uint16_t pc;
    };

    // Whether reads are checked.
    static std::atomic<bool> enabled;
    // What the debugger shows while the CPU is stopped on a report (the main thread's): empty
    // otherwise.
    static std::string message;

    // Whether a read of the byte with this flag is one to report. Sets the flag if it is.
    static bool IsFirstRead(bool& initialized) {
        if(initialized || !enabled) {
            return false;
        }
        initialized = true;
        return true;
    }

    // From any thread. Only one report waits at a time: the first, until it's taken.
    static void Report(const Access& access);
    // The main thread's: false if no report is waiting.
    static bool TakeReport(Access& access);
    static std::string Describe(const Access& access);
};

// Spots startup code wiping RAM, so that the wipe doesn't count as initializing it: otherwise a
// program that clears all of RAM first could never read "uninitialized" RAM at all. A wipe is
// one instruction writing one value to most of a RAM bank's worth of bytes (in any order), soon
// after a reset. Its bytes go back to counting as never written. Zeroing a program's variables
// is far fewer bytes, so that does count, as does anything written later.
class RamWipeDetector {
public:
    explicit RamWipeDetector(uint32_t ramSize);
    // The CPU was reset, at this cycle count.
    void Restart(uint64_t cycle);
    // The instruction at pc has written value to a byte of RAM (index: where in all of RAM),
    // whose flag in initialized the caller has just set.
    void Write(uint64_t cycle, uint16_t pc, uint32_t index, uint8_t value, bool* initialized);

private:
    // What one instruction has written since it last changed value: a wipe once it's enough.
    struct Run {
        bool active = false;
        uint16_t pc = 0;
        uint8_t value = 0;
        // written[index] == generation: the run has written that byte, and no other
        // instruction has since. Starting a run afresh is a new generation, not a cleared array.
        std::vector<uint16_t> written;
        uint16_t generation = 1;
        uint32_t written_count = 0;
        bool is_wipe = false;
        uint64_t last_used = 0;

        bool Has(uint32_t index) const { return written[index] == generation; }
        void Start(uint16_t newPc, uint8_t newValue);
    };

    // The instructions followed at once: a wiping loop also has a counter or two to write.
    static const int RUN_COUNT = 4;
    Run runs[RUN_COUNT];
    uint32_t ram_size;
    uint64_t reset_cycle = 0;
    uint64_t use_count = 0;
};
