#include "breakpoints.h"
#include "source_map.h"
#include <algorithm>
int Breakpoints::breakCooldown = 0;
bool Breakpoints::enabled = false;
vector<Breakpoint> Breakpoints::breakpoints;

#define BREAKPOINT_COOLDOWN 16

bool Breakpoints::checkBreakpoint(uint16_t address, uint8_t bank) {
    if(!enabled) return false;
    if(breakCooldown) {
        --breakCooldown;
        return false;
    }
    //for some reason it was getting stuck so wait a number of cycles between breakpoint activations

    // bank is the cartridge bank register, whose top bit is set while ROM is mapped in (0x80 | bank number),
    // while breakpoints from the source map store just the bank number. Compare only the bank numbers.
    auto matcher = [address, bank](Breakpoint i) { return (i.address == address) && (!i.bank_set || ((i.bank & 127) == (bank & 127)) || (((i.bank & 127) == 127) && address >= 0xC000   )) && i.enabled && !i.linkFailed; };
    auto it = std::find_if(begin(breakpoints), end(breakpoints), matcher);

    if(it != std::end(breakpoints)) {
        breakCooldown = BREAKPOINT_COOLDOWN;
    }
    return breakCooldown == BREAKPOINT_COOLDOWN;;
}

//Scan memory map for 
void Breakpoints::linkBreakpoints(MemoryMap& memorymap) {
    for(auto& bp : breakpoints) {
        if(!bp.by_address) {
            if((!bp.linked) && (!bp.linkFailed)) {
                if(memorymap.FindName(bp.address, bp.name)) {
                    bp.linked = true;
                } else {
                    bp.linkFailed = true;
                }
            }
        }
    }
}

std::string Breakpoints::sourceLineName(const std::string& fileName, int line) {
    return fileName + ":" + std::to_string(line);
}

bool Breakpoints::hasBreakpointNamed(const std::string& name) {
    return std::any_of(begin(breakpoints), end(breakpoints), [&name](const Breakpoint& bp) { return bp.name == name; });
}

// How far either way a source line breakpoint's text is looked for after an edit moves it.
#define SOURCE_LINE_SEARCH_RADIUS 200
// How many lines on from its line a source line breakpoint looks for code, when its own line
// has none (an edit left it blank or a comment).
#define SOURCE_CODE_SEARCH_LINES 5

static std::string trimmed(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r");
    if(first == std::string::npos) {
        return "";
    }
    size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// A line of the file (numbered from 1), trimmed; empty past either end.
static std::string line_text(const std::vector<std::string>& lines, int line) {
    if((line < 1) || (line > (int) lines.size())) {
        return "";
    }
    return trimmed(lines[line - 1]);
}

static void remember_line(Breakpoint& bp, const std::vector<std::string>& lines) {
    bp.line_text = line_text(lines, bp.source_line);
    bp.line_before = line_text(lines, bp.source_line - 1);
    bp.line_after = line_text(lines, bp.source_line + 1);
}

// Where the breakpoint's line is now: the line with its text, preferring one whose neighbours
// match too (so a "}" finds its own "}"), then the nearest. 0: not found.
static int find_moved_line(const std::vector<std::string>& lines, const Breakpoint& bp) {
    int best = 0;
    int best_score = -1;
    for(int distance = 0; distance <= SOURCE_LINE_SEARCH_RADIUS; ++distance) {
        for(int direction : { 1, -1 }) {
            if((distance == 0) && (direction == -1)) {
                continue;
            }
            int line = bp.source_line + (direction * distance);
            if(line_text(lines, line) != bp.line_text) {
                continue;
            }
            int score = (line_text(lines, line - 1) == bp.line_before) + (line_text(lines, line + 1) == bp.line_after);
            // Nearer lines come first, so on a tie the nearer one stays.
            if(score > best_score) {
                best = line;
                best_score = score;
            }
        }
    }
    return best;
}

bool Breakpoints::linkSourceBreakpoints(SourceMap& sourceMap) {
    bool changed = false;
    for(auto& bp : breakpoints) {
        if(!bp.by_source_line) {
            continue;
        }
        const std::vector<std::string>* lines = sourceMap.GetFileLines(bp.source_file);
        int line = bp.source_line;
        if((lines != nullptr) && !bp.line_text.empty()) {
            int found = find_moved_line(*lines, bp);
            if(found != 0) {
                line = found;
            }
        }
        SourceMapReverseSearchResult code = { false, 0, 0 };
        int code_line = line;
        for(; code_line < line + SOURCE_CODE_SEARCH_LINES; ++code_line) {
            code = sourceMap.ReverseSearch(bp.source_file, code_line);
            if(code.found) {
                break;
            }
        }
        if(!code.found) {
            // Shown red in the list, and never hit, until a rebuild gives it code again.
            bp.linkFailed = true;
            continue;
        }
        bp.linkFailed = false;
        bp.linked = true;
        bp.address = code.address;
        bp.bank = code.bank;
        bp.bank_set = true;
        if(code_line != bp.source_line) {
            bp.source_line = code_line;
            bp.name = sourceLineName(bp.source_file, code_line);
            changed = true;
        }
        if(lines != nullptr) {
            std::string old_text = bp.line_text + "\n" + bp.line_before + "\n" + bp.line_after;
            remember_line(bp, *lines);
            changed = changed || (old_text != bp.line_text + "\n" + bp.line_before + "\n" + bp.line_after);
        }
    }
    return changed;
}

void Breakpoints::addSourceBreakpoint(SourceMap& sourceMap, const std::string& fileName, int line, uint16_t address,
    uint8_t bank) {
    Breakpoint bp;
    bp.name = sourceLineName(fileName, line);
    bp.address = address;
    bp.by_address = true;
    bp.bank_set = true;
    bp.bank = bank;
    bp.enabled = true;
    bp.linked = true;
    bp.linkFailed = false;
    bp.by_source_line = true;
    bp.source_file = fileName;
    bp.source_line = line;
    const std::vector<std::string>* lines = sourceMap.GetFileLines(fileName);
    if(lines != nullptr) {
        remember_line(bp, *lines);
    }
    breakpoints.push_back(bp);
}

bool Breakpoints::parseSourceLineName(const std::string& name, std::string& fileName, int& line) {
    size_t colon = name.rfind(':');
    if((colon == std::string::npos) || (colon == 0) || (colon + 1 >= name.size())) {
        return false;
    }
    std::string digits = name.substr(colon + 1);
    if((digits.size() > 9) || (digits.find_first_not_of("0123456789") != std::string::npos)) {
        return false;
    }
    fileName = name.substr(0, colon);
    line = std::stoi(digits);
    return true;
}

SourceBreakpointToggle Breakpoints::toggleSourceBreakpoint(SourceMap& sourceMap, const std::string& fileName, int line) {
    auto name = sourceLineName(fileName, line);
    if(hasBreakpointNamed(name)) {
        std::erase_if(breakpoints, [&name](const Breakpoint& bp) { return bp.name == name; });
        return SourceBreakpointToggle::Removed;
    }

    auto searchResult = sourceMap.ReverseSearch(fileName, line);
    if(!searchResult.found) return SourceBreakpointToggle::NoCode;

    addSourceBreakpoint(sourceMap, fileName, line, searchResult.address, searchResult.bank);
    // A new breakpoint is no use if breakpoints are switched off.
    enabled = true;
    return SourceBreakpointToggle::Added;
}