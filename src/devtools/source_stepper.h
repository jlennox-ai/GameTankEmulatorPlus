#pragma once
#include <cstdint>
#include <unordered_map>
#include "source_map.h"

// "Step src", "Step over" and "Step out" for the code stepper. While one runs, the CPU runs
// freely and ShouldStop, called at every instruction fetch, says when the step is done:
//   Step src:  at the first instruction on a different source line than the one the step
//              started on (code without source, like a library's, runs through).
//   Step over: the same, but not inside a function called on the way: it counts each JSR in
//              and each RTS out, and only stops where the count is back to zero or below
//              (below: the function returned, so it stops in the caller). Counting calls, not
//              the stack pointer, since a function's prologue pushes onto the stack too.
//   Step out:  just after the RTS (or RTI) that returns from the function the step started in,
//              i.e. the first one that takes the stack pointer above where it started.
// All three run through interrupt handlers (from an interrupt vector's target to the RTI returning
// from it), so a vsync NMI doesn't end a step.
class SourceStepper {
public:
    typedef uint8_t (*MemoryReader)(uint16_t address, bool stateful);

    static void StartStepSource(uint16_t pc, uint8_t bank, uint8_t stackPointer);
    static void StartStepOver(uint16_t pc, uint8_t bank, uint8_t stackPointer);
    static void StartStepOut(uint8_t stackPointer);
    // address is the instruction about to run; opcode is its first byte. True: stop before it.
    static bool ShouldStop(uint16_t address, uint8_t bank, uint8_t stackPointer, uint8_t opcode,
        MemoryReader read);

private:
    enum StepKind { STEP_SOURCE, STEP_OVER, STEP_OUT };
    struct SourceLocation {
        bool found; // false: no source here
        unsigned int file;
        unsigned int line;
    };

    static StepKind kind;
    static SourceLocation startLocation;
    static uint8_t startStackPointer;
    static uint8_t lastOpcode;
    static int callDepth; // Step over: calls entered (JSR) less returned from (RTS) so far
    static bool vectorsRead;
    static uint16_t nmiTarget;
    static uint16_t irqTarget;
    static bool inInterrupt;
    static uint8_t interruptStackPointer;
    static SourceMap* cacheOwner;
    static std::unordered_map<uint32_t, SourceLocation> locationCache;

    static void Start(StepKind stepKind, uint8_t stackPointer);
    static SourceLocation LookUp(uint16_t address, uint8_t bank);
};
