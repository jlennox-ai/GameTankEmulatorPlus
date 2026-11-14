#include "source_stepper.h"

#define OPCODE_JSR 0x20
#define OPCODE_RTI 0x40
#define OPCODE_RTS 0x60
#define NMI_VECTOR 0xFFFA
#define IRQ_VECTOR 0xFFFE
// An interrupt pushes the return address and the status: three bytes.
#define INTERRUPT_STACK_BYTES 3

SourceStepper::StepKind SourceStepper::kind = SourceStepper::STEP_SOURCE;
SourceStepper::SourceLocation SourceStepper::startLocation = { false, 0, 0 };
uint8_t SourceStepper::startStackPointer = 0;
uint8_t SourceStepper::lastOpcode = 0;
int SourceStepper::callDepth = 0;
bool SourceStepper::vectorsRead = false;
uint16_t SourceStepper::nmiTarget = 0;
uint16_t SourceStepper::irqTarget = 0;
bool SourceStepper::inInterrupt = false;
uint8_t SourceStepper::interruptStackPointer = 0;
SourceMap* SourceStepper::cacheOwner = nullptr;
std::unordered_map<uint32_t, SourceStepper::SourceLocation> SourceStepper::locationCache;

void SourceStepper::Start(StepKind stepKind, uint8_t stackPointer) {
    kind = stepKind;
    startStackPointer = stackPointer;
    lastOpcode = 0;
    callDepth = 0;
    // The vectors are read through the emulator's memory on the first fetch (ShouldStop).
    vectorsRead = false;
    inInterrupt = false;
}

void SourceStepper::StartStepSource(uint16_t pc, uint8_t bank, uint8_t stackPointer) {
    Start(STEP_SOURCE, stackPointer);
    startLocation = LookUp(pc, bank);
}

void SourceStepper::StartStepOver(uint16_t pc, uint8_t bank, uint8_t stackPointer) {
    Start(STEP_OVER, stackPointer);
    startLocation = LookUp(pc, bank);
}

void SourceStepper::StartStepOut(uint8_t stackPointer) {
    Start(STEP_OUT, stackPointer);
}

// The source line at an address, from the loaded source map. Searching the map is slow and
// a step can run many thousands of instructions, so each address's answer is kept.
SourceStepper::SourceLocation SourceStepper::LookUp(uint16_t address, uint8_t bank) {
    SourceLocation none = { false, 0, 0 };
    // RAM can't be mapped to source (SourceMap::Search warns about it on every call).
    if((SourceMap::singleton == nullptr) || (address < 0x8000)) {
        return none;
    }
    if(cacheOwner != SourceMap::singleton) {
        locationCache.clear();
        cacheOwner = SourceMap::singleton;
    }
    uint32_t key = (((uint32_t) bank) << 16) | address;
    auto cached = locationCache.find(key);
    if(cached != locationCache.end()) {
        return cached->second;
    }
    SourceMapSearchResult result = SourceMap::singleton->Search(address, bank);
    SourceLocation location = result.found ? SourceLocation{ true, result.file->id, result.line->line } : none;
    locationCache[key] = location;
    return location;
}

bool SourceStepper::ShouldStop(uint16_t address, uint8_t bank, uint8_t stackPointer, uint8_t opcode,
    MemoryReader read) {
    if(!vectorsRead) {
        nmiTarget = read(NMI_VECTOR, false) | (read(NMI_VECTOR + 1, false) << 8);
        irqTarget = read(IRQ_VECTOR, false) | (read(IRQ_VECTOR + 1, false) << 8);
        vectorsRead = true;
    }
    uint8_t previousOpcode = lastOpcode;
    lastOpcode = opcode;

    // Counted before an interrupt is noticed: one can come straight after a JSR. An interrupt
    // handler's own calls aren't counted (it's skipped as a whole).
    if(!inInterrupt) {
        if(previousOpcode == OPCODE_JSR) {
            ++callDepth;
        } else if(previousOpcode == OPCODE_RTS) {
            --callDepth;
        }
    }

    if(inInterrupt) {
        // The RTI back out of the handler restores the stack pointer to where it was.
        if((previousOpcode != OPCODE_RTI) || (stackPointer != interruptStackPointer)) {
            return false;
        }
        inInterrupt = false;
    } else if((address == nmiTarget) || (address == irqTarget)) {
        // An interrupt has just been taken (it pushed and jumped, without a fetch here).
        inInterrupt = true;
        interruptStackPointer = stackPointer + INTERRUPT_STACK_BYTES;
        return false;
    }

    if(kind == STEP_OUT) {
        return ((previousOpcode == OPCODE_RTS) || (previousOpcode == OPCODE_RTI))
            && (stackPointer > startStackPointer);
    }
    if((kind == STEP_OVER) && (callDepth > 0)) {
        return false; // inside a function the line called
    }
    SourceLocation location = LookUp(address, bank);
    if(!location.found) {
        return false;
    }
    // A step started where there's no source stops at the first line there is.
    return !startLocation.found || (location.file != startLocation.file) || (location.line != startLocation.line);
}
