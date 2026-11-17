#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include "../timekeeper.h"
#include "memory_map.h"
#include "source_map.h"

#define PROFILER_ENTRIES 64
#define PROFILER_HISTORY 256

class Profiler {
    enum ProfileEventType {
        CALL,
        RETURN
    };
    struct ProfileEvent {
        ProfileEventType type;
        uint16_t origin;
        uint16_t destination;
        uint8_t bank;
        uint64_t cycle_number;

        ProfileEvent(ProfileEventType type, uint16_t origin, uint16_t destination, uint8_t bank, uint64_t cycle_number)
            : type(type), origin(origin), destination(destination), bank(bank), cycle_number(cycle_number)
        {}
    };

public: 
    class DeepProfileCallNode {
        public:
        std::string name;
        SourceMapLine line;
        // In cycles: 64-bit, like the cycle count they're taken from (32 bits wrap after about 20
        // minutes of emulation).
        uint64_t duration;
        uint64_t offset;
        std::vector<DeepProfileCallNode*> children;
    };

private:
    Timekeeper& timekeeper;
    bool deepProfileIsRecording = false;
    // Started by the Profiler window's Start (stopped by its Stop), not a timed capture.
    bool deepProfileIsManual = false;
    uint64_t deepProfileStartCycleCount = 0;
    uint64_t deepProfileCallCount = 0;
    uint32_t deepProfileNumber = 0;

    std::string profileEventTypeToString(ProfileEventType type);
    void recursiveNodePrintToFile(std::ofstream& file, DeepProfileCallNode* node, uint16_t depth);
    void exportTimelineToJSON(const std::vector<ProfileEvent>& timeline, const std::string& filename, MemoryMap* memory_map, SourceMap* source_map);
public:
    Profiler(Timekeeper& tk) : timekeeper(tk) {};
    int zeroConsec = 0;
    uint8_t bufferFlipCount = 0;
    void LogTime(uint8_t index);
    void ResetTimers();
    uint64_t profilingTimeStamps[PROFILER_ENTRIES];
    uint64_t profilingTimes[PROFILER_ENTRIES];
    uint64_t profilingCounts[PROFILER_ENTRIES];
    uint32_t profilingLastSample[PROFILER_ENTRIES];
    uint32_t profilingLastSampleCount[PROFILER_ENTRIES];
    float profilingHistory[PROFILER_ENTRIES][PROFILER_HISTORY];
    float blitter_history[PROFILER_HISTORY];
    uint64_t last_blitter_activity = 0;
    int fps = 60;
    int history_num = 0;
    bool measure_by_frameflip = false;
    std::vector<ProfileEvent> timeline;
    void LogJSR(uint16_t address, uint8_t bank, uint16_t dest);
    void LogRTS(uint16_t address, uint8_t bank);
    // manual: started by the Profiler window, to run until its Stop.
    void DeepProfileStart(bool manual = false);
    void DeepProfileStop(MemoryMap* memory_map, SourceMap* source_map);
    bool IsDeepProfileRecording() const { return deepProfileIsRecording; }
    bool IsDeepProfileManual() const { return deepProfileIsManual; }
    // The recording so far: its length in seconds of emulated time, and the calls in it.
    double DeepProfileRecordedSeconds() const;
    uint64_t DeepProfileRecordedCalls() const { return deepProfileCallCount; }
    // Counts finished deep profiles, so a view of one can tell a new one has replaced it.
    uint32_t DeepProfileNumber() const { return deepProfileNumber; }
    DeepProfileCallNode* lastDeepProfileRoot;
    DeepProfileCallNode* deepProfileZoomFocus;
    std::vector<DeepProfileCallNode*> cleanupList;
};