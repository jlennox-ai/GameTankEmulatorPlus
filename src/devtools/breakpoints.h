#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "memory_map.h"

using std::vector;

typedef struct Breakpoint {
    uint16_t address;
    uint8_t bank;
    bool bank_set;
    std::string name;
    bool by_address;
    bool enabled;
    //not persisted below:
    bool linked; //flag for whether the var name has been searched and refreshed
    bool linkFailed;
    // A source line breakpoint (from the Source tab): its line, and that line's text and the
    // ones either side when it was last placed, to find the line again after an edit moves it
    // (Breakpoints::linkSourceBreakpoints). Persisted.
    bool by_source_line = false;
    std::string source_file;
    int source_line = 0;
    std::string line_text;
    std::string line_before;
    std::string line_after;
} Breakpoint;

class SourceMap;

enum class SourceBreakpointToggle {
    Added,
    Removed,
    NoCode,
};

class Breakpoints {
public:
    static int breakCooldown;
    static bool enabled;
    static vector<Breakpoint> breakpoints;
    static bool checkBreakpoint(uint16_t address, uint8_t bank);
    static void linkBreakpoints(MemoryMap& memorymap);

    // Source line breakpoints are identified by their name, "file:line".
    static std::string sourceLineName(const std::string& fileName, int line);
    static bool hasBreakpointNamed(const std::string& name);
    static void addSourceBreakpoint(SourceMap& sourceMap, const std::string& fileName, int line, uint16_t address,
        uint8_t bank);
    // Finds each source line breakpoint's line again in the source as it is now (by its text,
    // nearby, when an edit has moved it), then that line's code in the new build. One whose line
    // or code can't be found is marked linkFailed. Returns whether any moved or changed, for
    // saving.
    static bool linkSourceBreakpoints(SourceMap& sourceMap);
    // "file:line" back into its parts: false if name isn't one.
    static bool parseSourceLineName(const std::string& name, std::string& fileName, int& line);
    // Removes the line's breakpoint if it has one, otherwise adds one and turns breakpoints on.
    // The caller saves the game config when this returns Added or Removed.
    static SourceBreakpointToggle toggleSourceBreakpoint(SourceMap& sourceMap, const std::string& fileName, int line);
};