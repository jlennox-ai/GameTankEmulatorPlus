#include "../imgui/imgui.h"
#include "profiler_window.h"
#include "implot.h"
#include "../audio_coprocessor.h"
#include <algorithm>
#include <cstdio>
#include <string>

static float prof_R[8] = {1,    1, 1, 0, 0, 0.5f, 0.5f, 1};
static float prof_G[8] = {0, 0.5f, 1, 1, 0,    0, 0.5f, 1};
static float prof_B[8] = {0,    0, 0, 0, 1, 0.5f, 0.5f, 1};

void ProfilerWindow::recurse_tree_nodes(Profiler::DeepProfileCallNode* node, uint64_t totalCycles, uint64_t startTime) {
    ImGui::PushID(node);
    bool opened = ImGui::TreeNode(node, "%s : %d", node->name.c_str(), node->duration);
    ImGui::SameLine(ImGui::GetWindowWidth()-288);
    if(ImGui::Button(">")) {
        _profiler.deepProfileZoomFocus = node;
    }
    ImGui::SameLine(ImGui::GetWindowWidth()-256);
    ImGui::Text("%.1f%%", (100.0f * (float) node->duration) / ((float) totalCycles));
    ImGui::SameLine(ImGui::GetWindowWidth()-216);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = (200.0f * (float) node->duration) / ((float) totalCycles);
    float offset = (200.0f * (float) (node->offset - startTime)) / ((float) totalCycles);
    if(width < 1) width = 1;
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(p.x,p.y), ImVec2(p.x+200, p.y+16),
        IM_COL32( 64, 64,128,255));
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(p.x+offset,p.y), ImVec2(p.x+width+offset, p.y+16),
        IM_COL32(255,255,0,255));
    //ImGui::GetForegroundDrawList()->AddText(ImVec2(p.x-32,p.y),IM_COL32_WHITE, "%%");
    ImGui::NewLine();
    if(opened) {
        for(auto& child : node->children) {
            recurse_tree_nodes(child, totalCycles, startTime);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ProfilerWindow::AddCalls(CallTreeNode& into, const std::vector<Profiler::DeepProfileCallNode*>& calls) {
    for(auto call : calls) {
        std::string name = call->name.empty() ? "(no symbol)" : call->name;
        CallTreeNode* merged = nullptr;
        for(auto& existing : into.children) {
            if(existing.name == name) {
                merged = &existing;
                break;
            }
        }
        if(merged == nullptr) {
            into.children.emplace_back();
            merged = &into.children.back();
            merged->name = name;
        }
        uint64_t callee_cycles = 0;
        for(auto callee : call->children) {
            callee_cycles += callee->duration;
        }
        merged->total += call->duration;
        merged->self += (callee_cycles < call->duration) ? (call->duration - callee_cycles) : 0;
        merged->calls += 1;
        AddCalls(*merged, call->children);
    }
}

void ProfilerWindow::SortCallTree(CallTreeNode& node) {
    auto before = [this](const CallTreeNode& first, const CallTreeNode& second) {
        const CallTreeNode& left = sort_descending ? second : first;
        const CallTreeNode& right = sort_descending ? first : second;
        switch(sort_column) {
            case CallTreeColumn::Function: return left.name < right.name;
            case CallTreeColumn::Self: return left.self < right.self;
            case CallTreeColumn::Total: return left.total < right.total;
        }
        return false;
    };
    std::stable_sort(node.children.begin(), node.children.end(), before);
    for(auto& child : node.children) {
        SortCallTree(child);
    }
}

// "12345 (12.3%)" of the total shown.
static std::string cycles_and_share(uint64_t cycles, uint64_t shownTotal) {
    char share[16];
    std::snprintf(share, sizeof(share), "%.1f%%", (shownTotal == 0) ? 0.0 : (100.0 * (double) cycles / (double) shownTotal));
    return std::to_string(cycles) + " (" + share + ")";
}

void ProfilerWindow::RenderCallTreeRow(const CallTreeNode& node, uint64_t shownTotal, std::vector<std::string>& path) {
    path.push_back(node.name);
    ImGui::PushID(node.name.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    bool leaf = node.children.empty();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns
        | (leaf ? (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen) : ImGuiTreeNodeFlags_None);
    std::string label = node.name + ((node.calls > 1) ? ("  (" + std::to_string(node.calls) + " calls)") : "");
    bool open = ImGui::TreeNodeEx("##call", flags, "%s", label.c_str());
    if(ImGui::BeginPopupContextItem("call menu")) {
        if(ImGui::MenuItem("Show only this")) {
            focus_path = path;
        }
        ImGui::EndPopup();
    }
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(cycles_and_share(node.self, shownTotal).c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(cycles_and_share(node.total, shownTotal).c_str());
    if(open && !leaf) {
        for(auto& child : node.children) {
            RenderCallTreeRow(child, shownTotal, path);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
    path.pop_back();
}

void ProfilerWindow::RenderCallTree() {
    const Profiler::DeepProfileCallNode* root = _profiler.lastDeepProfileRoot;
    if(root == nullptr) {
        ImGui::Text("Run a deep profile (Tools > Deep Profile) to see results here");
        return;
    }
    if(call_tree_source != root) {
        call_tree = CallTreeNode();
        call_tree.name = "root";
        call_tree.total = root->duration;
        call_tree.calls = 1;
        AddCalls(call_tree, root->children);
        uint64_t called = 0;
        for(auto& child : call_tree.children) {
            called += child.total;
        }
        call_tree.self = (called < call_tree.total) ? (call_tree.total - called) : 0;
        SortCallTree(call_tree);
        call_tree_source = root;
        focus_path.clear();
    }

    // What's shown as the root: all of it, or the node "Show only this" picked.
    const CallTreeNode* shown = &call_tree;
    for(auto& name : focus_path) {
        const CallTreeNode* next = nullptr;
        for(auto& child : shown->children) {
            if(child.name == name) {
                next = &child;
            }
        }
        if(next == nullptr) {
            focus_path.clear();
            shown = &call_tree;
            break;
        }
        shown = next;
    }
    if(focus_path.empty()) {
        ImGui::Text("Total: %s cycles, %s of them outside any call", std::to_string(shown->total).c_str(),
            std::to_string(shown->self).c_str());
    } else {
        if(ImGui::Button("<##show all")) {
            focus_path.clear();
        }
        ImGui::SameLine();
        std::string trail;
        for(auto& name : focus_path) {
            trail += (trail.empty() ? "" : " > ") + name;
        }
        ImGui::Text("%s: %s cycles", trail.c_str(), std::to_string(shown->total).c_str());
    }

    ImGuiTableFlags table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV
        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;
    if(!ImGui::BeginTable("call tree", 3, table_flags)) {
        return;
    }
    float time_width = ImGui::CalcTextSize("0000000 (100.0%)").x;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 0,
        (ImGuiID) CallTreeColumn::Function);
    ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
        time_width, (ImGuiID) CallTreeColumn::Self);
    ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending
        | ImGuiTableColumnFlags_DefaultSort, time_width, (ImGuiID) CallTreeColumn::Total);
    ImGui::TableHeadersRow();
    ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs();
    if((sort_specs != nullptr) && sort_specs->SpecsDirty && (sort_specs->SpecsCount > 0)) {
        sort_column = (CallTreeColumn) sort_specs->Specs[0].ColumnUserID;
        sort_descending = (sort_specs->Specs[0].SortDirection == ImGuiSortDirection_Descending);
        SortCallTree(call_tree);
        sort_specs->SpecsDirty = false;
    }
    std::vector<std::string> path = focus_path;
    for(auto& child : shown->children) {
        RenderCallTreeRow(child, shown->total, path);
    }
    ImGui::EndTable();
}

ImVec2 ProfilerWindow::Render() {

    ImVec2 sizeOut = {0, 0};

    // Fill the SDL window, whatever size it has been resized to.
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Profiler", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::BeginTabBar("profilertabs", 0);
    if(ImGui::BeginTabItem("Stopwatches")) {
        ImGui::Text("FPS: %d", _profiler.fps);
        ImGui::Text("ACP: %d", AudioCoprocessor::singleton_acp_state->last_irq_cycles);
        ImGui::Text("Graph max:");
        ImGui::SameLine();
        ImGui::InputFloat("##", &max_scale, 0, 0, "%.2f");

        ImGui::Checkbox("Per frame", &_profiler.measure_by_frameflip);
        if (ImGui::IsItemActive() || ImGui::IsItemHovered())
            ImGui::SetTooltip("Aggregate per buffer swap instead of per vsync");

        if(ImPlot::BeginPlot("Timer history")) {
            ImPlot::SetupAxes("", "");
            ImPlot::SetupAxesLimits(0,256, 0, max_scale, ImPlotCond_Always);
            for(int i = 0; i < PROFILER_ENTRIES; ++i) {
                if(profilerSeen[i] && profilerVis[i]) {
                    ImPlot::PlotLine<float>("", _profiler.profilingHistory[i], PROFILER_HISTORY, 1, 0, ImPlotSpec(
                        ImPlotProp_LineColor, ImVec4(prof_R[i % 7], prof_G[i % 7], prof_B[i % 7], 1.0f),
                        ImPlotProp_Offset, _profiler.history_num
                    ));
                }
            }
            ImPlot::PlotLine<float>("Blitter", _profiler.blitter_history, PROFILER_HISTORY, 1, 0, ImPlotSpec(
                        ImPlotProp_LineColor, ImVec4(prof_R[7], prof_G[7], prof_B[7], 1.0f),
                        ImPlotProp_Offset, _profiler.history_num
            ));
            ImPlot::EndPlot();
        }
        


        ImGui::BeginChild("Scrolling");
        ImGui::Text("Blit Pixels/Frame: %lu px", _profiler.last_blitter_activity);
        for(int i = 0; i < PROFILER_ENTRIES; ++i) {
            if(_profiler.profilingLastSample[i] != 0) {
                if(!profilerSeen[i]) {
                    profilerVis[i] = true;
                }
                profilerSeen[i] = true;
            }
            if(profilerSeen[i]) {
                ImGui::PushID(i);
                ImGui::Checkbox("##", &profilerVis[i]);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(prof_R[i % 8], prof_G[i % 8], prof_B[i % 8], 1.0f), 
                "Timer %02d: %ld / %ld", i, _profiler.profilingLastSample[i], _profiler.profilingLastSampleCount[i]);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    if(ImGui::BeginTabItem("Deep Profile")) {
        if(_profiler.lastDeepProfileRoot == nullptr) {
            ImGui::Text("Run a deep profile to see results here");
        } else {

            if(_profiler.deepProfileZoomFocus == nullptr) {
                ImGui::Text("Total: %d cycles", _profiler.lastDeepProfileRoot->duration);
                for(auto& node : _profiler.lastDeepProfileRoot->children) {
                    recurse_tree_nodes(node, _profiler.lastDeepProfileRoot->duration, 0);
                }
            } else {
                ImGui::Text("%s: %d cycles", _profiler.deepProfileZoomFocus->name.c_str(), _profiler.deepProfileZoomFocus->duration);
                ImGui::SameLine();
                if(ImGui::Button("<##unzoom")) {
                    _profiler.deepProfileZoomFocus = nullptr;
                } else {
                    for(auto& node : _profiler.deepProfileZoomFocus->children) {
                        recurse_tree_nodes(node, _profiler.deepProfileZoomFocus->duration, _profiler.deepProfileZoomFocus->offset);
                    }
                }
            }
        }
        ImGui::EndTabItem();
    }
    if(ImGui::BeginTabItem("Call Tree")) {
        RenderCallTree();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    sizeOut = ImVec2(800, 800); // the initial size; the window can be resized from there
    ImGui::End();
    return sizeOut;
}
