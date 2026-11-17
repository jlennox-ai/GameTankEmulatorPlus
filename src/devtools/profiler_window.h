
#include "debug_window.h"
#include "profiler.h"
#include <string>
#include <vector>

class ProfilerWindow : public DebugWindow {
private:
    Profiler& _profiler;
    ImVec2 Render();
    float max_scale = 2.0f;
    bool profilerVis[PROFILER_ENTRIES] = {0};
    bool profilerSeen[PROFILER_ENTRIES] = {0};
    void recurse_tree_nodes(Profiler::DeepProfileCallNode* node, uint64_t totalCycles, uint64_t startTime);

    // The Call Tree tab: the deep profile with calls to the same function from the same place
    // in the tree merged, with their total cycles (callees included), self cycles (not) and count.
    struct CallTreeNode {
        std::string name;
        uint64_t total = 0;
        uint64_t self = 0;
        uint32_t calls = 0;
        std::vector<CallTreeNode> children;
    };
    enum class CallTreeColumn { Function, Self, Total };
    CallTreeNode call_tree;
    // The profile the tree was built from, to rebuild it after a new one.
    const Profiler::DeepProfileCallNode* call_tree_source = nullptr;
    CallTreeColumn sort_column = CallTreeColumn::Total;
    bool sort_descending = true;
    // "Show only this": the names from the root down to the node shown as the root (empty: all).
    std::vector<std::string> focus_path;

    static void AddCalls(CallTreeNode& into, const std::vector<Profiler::DeepProfileCallNode*>& calls);
    void SortCallTree(CallTreeNode& node);
    void RenderCallTree();
    void RenderCallTreeRow(const CallTreeNode& node, uint64_t shownTotal, std::vector<std::string>& path);
public:
    ProfilerWindow(Profiler& profiler): DebugWindow("Profiler", "profiler"), _profiler(profiler) {};
};