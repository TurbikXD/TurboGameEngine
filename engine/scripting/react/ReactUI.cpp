#include "engine/scripting/react/ReactUI.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <imgui.h>
#include <imgui_internal.h>
#include <lua.h>
#include "engine/scripting/ScriptingSystem.h"

namespace engine::scripting {
namespace {
int absolute(lua_State* vm, int index) {
    return index > 0 || index <= LUA_REGISTRYINDEX ? index : lua_gettop(vm) + index + 1;
}
void rawField(lua_State* vm, int table, const char* field) {
    table = absolute(vm, table);
    if (!lua_istable(vm,table)) {lua_pushnil(vm); return;}
    lua_pushstring(vm, field);
    lua_rawget(vm, table);
}
std::string stringField(lua_State* vm, int table, const char* field, const char* fallback = "") {
    rawField(vm, table, field);
    size_t length = 0;
    const char* text = lua_type(vm, -1) == LUA_TSTRING ? lua_tolstring(vm, -1, &length) : nullptr;
    std::string result = text ? std::string(text, std::min(length, size_t{32768})) : fallback;
    lua_pop(vm, 1);
    return result;
}
std::string enumField(lua_State* vm, int table, const char* field, const char* fallback) {
    rawField(vm, table, field);
    std::string result = fallback;
    if (lua_istable(vm, -1)) result = stringField(vm, -1, "Name", fallback);
    else if (lua_type(vm, -1) == LUA_TSTRING) {
        size_t length = 0; const auto* value = lua_tolstring(vm, -1, &length);
        result.assign(value, std::min(length, size_t{32768}));
    }
    lua_pop(vm, 1);
    return result;
}
float numberField(lua_State* vm, int table, const char* field, float fallback = 0.0F) {
    rawField(vm, table, field);
    const double number = lua_type(vm, -1) == LUA_TNUMBER ? lua_tonumber(vm, -1) : fallback;
    lua_pop(vm, 1);
    return std::isfinite(number) ? static_cast<float>(std::clamp(number, -1000000.0, 1000000.0)) : fallback;
}
bool booleanField(lua_State* vm, int table, const char* field, bool fallback = false) {
    rawField(vm, table, field);
    const bool result = lua_isnil(vm, -1) ? fallback : lua_toboolean(vm, -1) != 0;
    lua_pop(vm, 1);
    return result;
}
bool hasField(lua_State* vm, int table, const char* field) {
    rawField(vm, table, field);
    const bool result = !lua_isnil(vm, -1);
    lua_pop(vm, 1);
    return result;
}
struct Dim {
    float scale = 0.0F, offset = 0.0F;
    float resolve(float basis, float pixelScale=1.0F) const { return std::clamp(scale * basis + offset*pixelScale, -1000000.0F, 1000000.0F); }
};
Dim dimension(lua_State* vm, int table, const char* field, Dim fallback = {}) {
    rawField(vm, table, field);
    if (lua_istable(vm, -1)) fallback = { numberField(vm, -1, "Scale"), numberField(vm, -1, "Offset") };
    else if (lua_type(vm, -1) == LUA_TNUMBER) {
        const double number = lua_tonumber(vm, -1);
        if (std::isfinite(number)) fallback = { 0.0F, static_cast<float>(std::clamp(number, -1000000.0, 1000000.0)) };
    }
    lua_pop(vm, 1);
    return fallback;
}
bool dimensions(lua_State* vm, int table, const char* field, ImVec2 basis, ImVec2& result, float pixelScale=1.0F) {
    rawField(vm, table, field);
    const bool found = lua_istable(vm, -1);
    if (found) result = { dimension(vm, -1, "X").resolve(basis.x,pixelScale), dimension(vm, -1, "Y").resolve(basis.y,pixelScale) };
    lua_pop(vm, 1);
    return found;
}
ImVec2 vectorField(lua_State* vm, int table, const char* field) {
    rawField(vm, table, field);
    ImVec2 result;
    if (lua_istable(vm, -1)) result = { numberField(vm, -1, "X"), numberField(vm, -1, "Y") };
    lua_pop(vm, 1);
    return result;
}
ImVec4 colorField(lua_State* vm, int table, const char* field, ImVec4 fallback) {
    rawField(vm, table, field);
    if (lua_istable(vm, -1)) {
        // A Color3 supplies opaque RGB; transparency is its separate property,
        // independent of the current ImGui skin's (often transparent) ChildBg.
        fallback.w = 1.0F;
        const int color = lua_gettop(vm);
        const char* names[] = { "R", "G", "B" };
        float* channels[] = { &fallback.x, &fallback.y, &fallback.z };
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(vm, color, i + 1);
            const double legacy = lua_type(vm, -1) == LUA_TNUMBER ? lua_tonumber(vm, -1) : *channels[i];
            lua_pop(vm, 1);
            *channels[i] = std::clamp(numberField(vm, color, names[i], std::isfinite(legacy) ? static_cast<float>(legacy) : *channels[i]), 0.0F, 1.0F);
        }
    }
    lua_pop(vm, 1);
    return fallback;
}
void enqueue(lua_State* vm, int instance, const char* name, int valueIndex = 0, int argsIndex=0) {
    instance = absolute(vm, instance);
    valueIndex = valueIndex == 0 ? 0 : absolute(vm, valueIndex);
    argsIndex = argsIndex == 0 ? 0 : absolute(vm,argsIndex);
    rawField(vm, LUA_GLOBALSINDEX, "__turboUIEvents");
    if (lua_istable(vm, -1) && !lua_getreadonly(vm, -1)) {
        const int queue = lua_gettop(vm);
        if (lua_objlen(vm, queue) >= 1024) { lua_pop(vm, 1); return; }
        lua_createtable(vm, 0, 3);
        lua_pushvalue(vm, instance); lua_setfield(vm, -2, "instance");
        lua_pushstring(vm, name); lua_setfield(vm, -2, "name");
        if (valueIndex != 0) { lua_pushvalue(vm, valueIndex); lua_setfield(vm, -2, "value"); }
        if (argsIndex != 0) {lua_pushvalue(vm,argsIndex); lua_setfield(vm,-2,"args");}
        lua_rawseti(vm, queue, lua_objlen(vm, queue) + 1);
    }
    lua_pop(vm, 1);
}
struct Rect { ImVec2 origin, size; };
struct MeasuredSize { ImVec2 basis, size; float scale; };
struct Budget {
    int nodes = 4096, inspections = 16384, rootInspections = 8192, measurements = 8192, valueWrites = 128, inputWrites=128, meshVertices = 131072;
    std::unordered_map<const void*, MeasuredSize> measured;
    const ReactUI::ImageResolver* imageResolver = nullptr;
    const ReactUI::CanvasResolver* canvasResolver = nullptr;
    std::unordered_map<std::string, ReactUI::Image> images;
    size_t allocationBytes=0;
    int textBytes=32768;
    int canvasGroups=32,canvasDepth=0,capturedVertices=262144,capturedIndices=786432;
};
// Delimit retained UI output without executing any script or changing ImGui's
// layout/input state. The markers prevent ImGui from merging a group with a
// preceding command; they are removed before the main draw data is submitted.
void canvasMarker(const ImDrawList*,const ImDrawCmd*) {}
class CanvasCapture final {
    struct Snapshot {
        int commands=0,indices=0,vertices=0;
        unsigned int currentVertex=0;
        ImDrawCmdHeader header{};
    };
    ImGuiWindow* m_root=ImGui::GetCurrentWindow();
    std::unordered_map<ImGuiWindow*,Snapshot> m_before;
    bool m_finished=false;
    static bool descendant(ImGuiWindow* window,ImGuiWindow* root) {
        for (int depth=0;window && depth<=64;++depth,window=window->ParentWindow) if (window==root) return true;
        return false;
    }
    static Snapshot snapshot(ImDrawList& list) {
        return {list.CmdBuffer.Size,list.IdxBuffer.Size,list.VtxBuffer.Size,list._VtxCurrentIdx,list._CmdHeader};
    }
    static void restore(ImDrawList& list,const Snapshot& before) {
        list.CmdBuffer.resize(before.commands); list.IdxBuffer.resize(before.indices); list.VtxBuffer.resize(before.vertices);
        list._CmdHeader=before.header; list._VtxCurrentIdx=before.currentVertex;
        list._IdxWritePtr=list.IdxBuffer.Data ? list.IdxBuffer.Data+before.indices : nullptr;
        list._VtxWritePtr=list.VtxBuffer.Data ? list.VtxBuffer.Data+before.vertices : nullptr;
        if (list.CmdBuffer.empty()) list.AddDrawCmd();
    }
    static bool append(ImDrawList& output,const ImDrawList& source,const Snapshot& before,Budget& budget) {
        if (source.IdxBuffer.Size<before.indices || source.VtxBuffer.Size<before.vertices) return false;
        for (const auto& command:source.CmdBuffer) {
            const size_t begin=std::max(size_t{command.IdxOffset},static_cast<size_t>(before.indices));
            const size_t end=std::min(size_t{command.IdxOffset}+command.ElemCount,static_cast<size_t>(source.IdxBuffer.Size));
            if (begin>=end) continue;
            if (command.UserCallback) return false;
            size_t minimum=static_cast<size_t>(source.VtxBuffer.Size),maximum=0;
            for (size_t index=begin;index<end;++index) {
                const size_t vertex=size_t{source.IdxBuffer[static_cast<int>(index)]}+command.VtxOffset;
                if (vertex>=static_cast<size_t>(source.VtxBuffer.Size) || vertex<static_cast<size_t>(before.vertices)) return false;
                minimum=std::min(minimum,vertex); maximum=std::max(maximum,vertex);
            }
            const size_t vertices=maximum-minimum+1,indices=end-begin;
            if (vertices>static_cast<size_t>(budget.capturedVertices) || indices>static_cast<size_t>(budget.capturedIndices) ||
                (sizeof(ImDrawIdx)==2 && vertices>65536)) return false;
            budget.capturedVertices-=static_cast<int>(vertices); budget.capturedIndices-=static_cast<int>(indices);
            ImDrawCmd copy=command;
            copy.VtxOffset=static_cast<unsigned int>(output.VtxBuffer.Size); copy.IdxOffset=static_cast<unsigned int>(output.IdxBuffer.Size);
            copy.ElemCount=static_cast<unsigned int>(indices); copy.UserCallback=nullptr; copy.UserCallbackData=nullptr;
            const int vertexStart=output.VtxBuffer.Size,indexStart=output.IdxBuffer.Size;
            output.VtxBuffer.resize(vertexStart+static_cast<int>(vertices));
            std::memcpy(output.VtxBuffer.Data+vertexStart,source.VtxBuffer.Data+minimum,vertices*sizeof(ImDrawVert));
            output.IdxBuffer.resize(indexStart+static_cast<int>(indices));
            for (size_t index=0;index<indices;++index)
                output.IdxBuffer[indexStart+static_cast<int>(index)]=static_cast<ImDrawIdx>(size_t{source.IdxBuffer[static_cast<int>(begin+index)]}+command.VtxOffset-minimum);
            output.CmdBuffer.push_back(copy);
        }
        return true;
    }
public:
    explicit CanvasCapture(const Rect& rect) {
        const auto* context=ImGui::GetCurrentContext();
        for (auto* window:context->Windows) if (window->LastFrameActive==context->FrameCount && descendant(window,m_root)) {
            auto& list=*window->DrawList;
            m_before.emplace(window,snapshot(list)); list.AddDrawCmd(); list.AddCallback(canvasMarker,nullptr);
        }
        ImGui::PushClipRect(rect.origin,{rect.origin.x+rect.size.x,rect.origin.y+rect.size.y},true);
    }
    bool finish(ImDrawList& output,Budget& budget) {
        ImGui::PopClipRect();
        bool valid=true;
        const auto* context=ImGui::GetCurrentContext();
        // ImGui emits a parent before its native child windows. Copy the root
        // first, then child draw lists, including multiline text input widgets.
        const auto capture=[&](ImGuiWindow* window) {
            if (window->LastFrameActive!=context->FrameCount || !descendant(window,m_root)) return;
            auto& list=*window->DrawList;
            const auto found=m_before.find(window);
            Snapshot before;
            if (found!=m_before.end()) before=found->second;
            else {before.header=list._CmdHeader; before.header.VtxOffset=0;}
            if (valid && !append(output,list,before,budget)) valid=false;
            restore(list,before);
        };
        capture(m_root);
        for (auto* window:context->Windows) if (window!=m_root) capture(window);
        m_finished=true;
        return valid;
    }
    ~CanvasCapture() {
        if (m_finished) return;
        ImGui::PopClipRect();
        for (const auto& [window,before]:m_before) restore(*window->DrawList,before);
    }
};
bool drawableRoot(lua_State* vm, int instance, int depth, int& remaining) {
    if (depth > 64 || remaining-- <= 0 || !lua_istable(vm, instance)) return false;
    instance = absolute(vm, instance);
    if (booleanField(vm, instance, "hidden") || booleanField(vm, instance, "disposed")) return false;
    const auto type = stringField(vm, instance, "type");
    rawField(vm, instance, "props");
    bool visible = true, background = false;
    if (lua_istable(vm, -1)) {
        visible = booleanField(vm, -1, "Visible", true) && (type != "ScreenGui" || booleanField(vm, -1, "Enabled", true));
        background = hasField(vm, -1, "BackgroundColor3");
    }
    lua_pop(vm, 1);
    if (!visible) return false;
    if (!type.empty() && type != "Folder" && type != "PlayerGui" && (type != "ScreenGui" || background)) return true;
    rawField(vm, instance, "children");
    bool found = false;
    if (lua_istable(vm, -1)) {
        const int children = lua_gettop(vm), count = std::min(lua_objlen(vm, -1), remaining);
        for (int index = 1; index <= count && remaining > 0; ++index) {
            lua_rawgeti(vm, children, index);
            found = drawableRoot(vm, -1, depth+1, remaining);
            lua_pop(vm, 1);
            if (found) break;
        }
    }
    lua_pop(vm, 1);
    return found;
}
struct Child { int index; float order; std::string name; float zIndex; bool structural=false; };
struct Modifiers {
    bool list = false, grid = false, horizontal = false, sortByName = false, corner = false, padding = false;
    int listIndex = 0, strokeIndex = 0, gradientIndex = 0;
    float scale = 1.0F, aspect = 0.0F;
    bool dominantHeight = false, fitAspect = true, sizeConstraint = false;
    ImVec2 minimum, maximum{1000000,1000000};
    Dim cellX{0,100},cellY{0,100},cellPaddingX{0,5},cellPaddingY{0,5};
    int maxCells=0;
    std::string startCorner="TopLeft";
    std::string horizontalAlignment = "Left", verticalAlignment = "Top";
    Dim gap, radius{ 0.0F, 8.0F }, left, right, top, bottom;
    std::vector<Child> children;
    std::vector<Child> structural;
};
// Modifier/layout discovery is bounded globally as well as the actual draw
// traversal. Raw reads must never execute script metamethods on the native path.
Modifiers modifiers(lua_State* vm, int instance, Budget& budget) {
    Modifiers result;
    rawField(vm, instance, "children");
    if (lua_istable(vm, -1)) {
        const int children = lua_gettop(vm);
        const int count = std::min(lua_objlen(vm, children), budget.inspections);
        for (int i = 1; i <= count && budget.inspections-- > 0; ++i) {
            lua_rawgeti(vm, children, i);
            const int child = lua_gettop(vm);
            if (lua_istable(vm, child) && !booleanField(vm, child, "hidden")) {
                const auto type = stringField(vm, child, "type");
                rawField(vm, child, "props");
                const int props = lua_gettop(vm);
                if (lua_istable(vm, props) && booleanField(vm, props, "Visible", true)) {
                    if (type == "UICorner") {
                        if (!result.corner) result.radius = dimension(vm, props, "CornerRadius", result.radius);
                        result.corner = true;
                    } else if (type == "UIPadding") {
                        if (!result.padding) {
                            result.left = dimension(vm, props, "PaddingLeft"); result.right = dimension(vm, props, "PaddingRight");
                            result.top = dimension(vm, props, "PaddingTop"); result.bottom = dimension(vm, props, "PaddingBottom");
                        }
                        result.padding = true;
                    } else if (type == "UIListLayout" || type=="UIGridLayout") {
                        if (!result.list) {
                            result.listIndex = i;
                            result.grid=type=="UIGridLayout";
                            result.horizontal = enumField(vm, props, "FillDirection", result.grid ? "Horizontal" : "Vertical") == "Horizontal";
                            result.sortByName = enumField(vm, props, "SortOrder", "LayoutOrder") == "Name";
                            result.horizontalAlignment = enumField(vm, props, "HorizontalAlignment", "Left");
                            result.verticalAlignment = enumField(vm, props, "VerticalAlignment", "Top");
                            result.gap = dimension(vm, props, "Padding");
                            if (result.grid) {
                                rawField(vm,props,"CellSize");
                                if (lua_istable(vm,-1)) {result.cellX=dimension(vm,-1,"X",result.cellX); result.cellY=dimension(vm,-1,"Y",result.cellY);}
                                lua_pop(vm,1); rawField(vm,props,"CellPadding");
                                if (lua_istable(vm,-1)) {result.cellPaddingX=dimension(vm,-1,"X",result.cellPaddingX); result.cellPaddingY=dimension(vm,-1,"Y",result.cellPaddingY);}
                                lua_pop(vm,1);
                                result.maxCells=static_cast<int>(std::clamp(numberField(vm,props,"FillDirectionMaxCells"),0.0F,4096.0F));
                                result.startCorner=enumField(vm,props,"StartCorner","TopLeft");
                            }
                        }
                        result.list = true;
                    } else if (type == "UIStroke") {
                        if (!result.strokeIndex && booleanField(vm, props, "Enabled", true)) result.strokeIndex = i;
                    } else if (type == "UIGradient") {
                        if (!result.gradientIndex && booleanField(vm, props, "Enabled", true)) result.gradientIndex = i;
                    } else if (type == "UIScale") {
                        result.scale *= std::clamp(numberField(vm, props, "Scale", 1.0F), 0.0F, 100.0F);
                        result.scale = std::min(result.scale, 100.0F);
                    } else if (type == "UISizeConstraint") {
                        result.minimum = vectorField(vm, props, "MinSize");
                        if (hasField(vm, props, "MaxSize")) result.maximum = vectorField(vm, props, "MaxSize");
                        result.sizeConstraint = true;
                    } else if (type == "UIAspectRatioConstraint") {
                        result.aspect = std::clamp(numberField(vm, props, "AspectRatio", 1.0F), 0.0001F, 10000.0F);
                        result.dominantHeight = enumField(vm, props, "DominantAxis", "Width") == "Height";
                        result.fitAspect = enumField(vm, props, "AspectType", "FitWithinMaxSize") == "FitWithinMaxSize";
                    } else result.children.push_back({ i, numberField(vm, props, "LayoutOrder"), stringField(vm, props, "Name", type.c_str()),
                        type == "ScreenGui" ? numberField(vm, props, "DisplayOrder") : numberField(vm, props, "ZIndex", 1.0F),
                        type=="Folder" || type=="PlayerGui" || type=="ScreenGui" });
                }
                lua_pop(vm, 1);
            }
            lua_pop(vm, 1);
        }
    }
    lua_pop(vm, 1);
    if (result.list) {
        for (const auto& child:result.children) if (child.structural) result.structural.push_back(child);
        result.children.erase(std::remove_if(result.children.begin(),result.children.end(),[](const auto& child){return child.structural;}),result.children.end());
        std::stable_sort(result.children.begin(), result.children.end(), [&](const Child& a, const Child& b) {
            return result.sortByName ? a.name < b.name : a.order < b.order;
        });
    }
    else std::stable_sort(result.children.begin(), result.children.end(), [](const Child& a, const Child& b) { return a.zIndex < b.zIndex; });
    return result;
}
struct GridMetrics { ImVec2 cell,padding,total; int columns=0,rows=0; };
GridMetrics gridMetrics(const Modifiers& layout,ImVec2 basis,float scale) {
    GridMetrics result;
    result.cell={std::max(1.0F,layout.cellX.resolve(basis.x,scale)),std::max(1.0F,layout.cellY.resolve(basis.y,scale))};
    result.padding={std::max(0.0F,layout.cellPaddingX.resolve(basis.x,scale)),std::max(0.0F,layout.cellPaddingY.resolve(basis.y,scale))};
    const int count=static_cast<int>(layout.children.size());
    if (!count) return result;
    const float available=layout.horizontal ? basis.x : basis.y,cell=layout.horizontal ? result.cell.x : result.cell.y,padding=layout.horizontal ? result.padding.x : result.padding.y;
    int across=static_cast<int>(std::clamp(std::floor((available+padding)/(cell+padding)),1.0F,4096.0F));
    if (layout.maxCells>0) across=std::min(across,layout.maxCells);
    across=std::min(across,count);
    result.columns=layout.horizontal ? across : (count+across-1)/across;
    result.rows=layout.horizontal ? (count+across-1)/across : across;
    result.total={result.columns*result.cell.x+(result.columns-1)*result.padding.x,result.rows*result.cell.y+(result.rows-1)*result.padding.y};
    return result;
}
ImVec2 constrainedSize(ImVec2 size,const Modifiers& layout,float scale) {
    const ImVec2 minimum=layout.sizeConstraint ? ImVec2(std::max(0.0F,layout.minimum.x*scale),std::max(0.0F,layout.minimum.y*scale)) : ImVec2();
    const ImVec2 maximum=layout.sizeConstraint ? ImVec2(std::max(minimum.x,layout.maximum.x*scale),std::max(minimum.y,layout.maximum.y*scale)) : ImVec2(1000000,1000000);
    size.x=std::clamp(size.x,minimum.x,maximum.x); size.y=std::clamp(size.y,minimum.y,maximum.y);
    if (layout.aspect>0) {
        float width=layout.fitAspect ? std::min(size.x,size.y*layout.aspect) : layout.dominantHeight ? size.y*layout.aspect : size.x;
        const float lower=std::max(minimum.x,minimum.y*layout.aspect),upper=std::min(maximum.x,maximum.y*layout.aspect);
        if (lower<=upper) {width=std::clamp(width,lower,upper); size={width,width/layout.aspect};}
        // Contradictory min/max constraints take priority over aspect ratio.
    }
    return {std::clamp(size.x,0.0F,1000000.0F),std::clamp(size.y,0.0F,1000000.0F)};
}
void publishVector(lua_State* vm, int instance, int props, const char* property, ImVec2 value, Budget& budget) {
    instance = absolute(vm, instance); props = absolute(vm, props);
    if (budget.valueWrites <= 0 || budget.allocationBytes<512 || lua_getreadonly(vm, props)) return;
    value.x = std::clamp(value.x, -1000000.0F, 1000000.0F); value.y = std::clamp(value.y, -1000000.0F, 1000000.0F);
    rawField(vm, props, property);
    const bool same = lua_istable(vm, -1) && numberField(vm, -1, "X", -1000001.0F) == value.x &&
        numberField(vm, -1, "Y", -1000001.0F) == value.y;
    lua_pop(vm, 1);
    if (same) return;
    --budget.valueWrites;
    budget.allocationBytes=budget.allocationBytes>512 ? budget.allocationBytes-512 : 0;
    lua_createtable(vm, 0, 2);
    lua_pushnumber(vm, value.x); lua_setfield(vm, -2, "X");
    lua_pushnumber(vm, value.y); lua_setfield(vm, -2, "Y");
    rawField(vm, LUA_GLOBALSINDEX, "__turboVector2Metatable");
    if (lua_istable(vm, -1)) lua_setmetatable(vm, -2); else lua_pop(vm, 1);
    lua_setreadonly(vm, -1, true);
    const int vector = lua_gettop(vm);
    lua_pushstring(vm, property); lua_pushvalue(vm, vector); lua_rawset(vm, props);
    const auto event = std::string("Change.") + property;
    enqueue(vm, instance, event.c_str(), vector);
    lua_pop(vm, 1);
}
float textSize(lua_State* vm, int props) {
    return std::clamp(numberField(vm, props, "TextSize", ImGui::GetFontSize()), 6.0F, 128.0F);
}
ImVec2 nodeSize(lua_State* vm, int instance, int props, const std::string& type, ImVec2 basis, Budget& budget, int depth,
               float parentScale=1.0F, const Modifiers* known=nullptr) {
    instance = absolute(vm, instance); props = absolute(vm, props);
    const auto* identity = lua_topointer(vm, instance);
    const auto cached = budget.measured.find(identity);
    if (cached != budget.measured.end() && cached->second.basis.x == basis.x && cached->second.basis.y == basis.y && cached->second.scale==parentScale) return cached->second.size;
    ImVec2 result;
    if (type == "ScreenGui") return {std::max(0.0F,basis.x),std::max(0.0F,basis.y)};
    const auto discovered=known ? Modifiers{} : modifiers(vm,instance,budget);
    const auto& layout=known ? *known : discovered;
    const float scale=std::clamp(parentScale*layout.scale,0.0F,10000.0F);
    if (!dimensions(vm, props, "Size", basis, result,parentScale)) {
        const auto text = stringField(vm, props, "Text", type.c_str());
        const auto natural = ImGui::GetFont()->CalcTextSizeA(textSize(vm, props)*parentScale, FLT_MAX, 0.0F, text.c_str());
        const auto& style = ImGui::GetStyle();
        result = natural;
        if (type == "Frame" || type == "CanvasGroup" || type == "Window" || type=="ScrollingFrame") result = basis;
        else if (type=="ImageLabel" || type=="ImageButton" || type=="TextBox") result={100,100};
        else if (type == "TextButton") result = { natural.x + style.FramePadding.x * 2.0F, natural.y + style.FramePadding.y * 2.0F };
        else if (type == "Checkbox") result = { natural.x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x, ImGui::GetFrameHeight() };
        else if (type == "SliderFloat") result = { std::min(200.0F, basis.x), ImGui::GetFrameHeight() };
        else if (type == "Separator") result = { basis.x, 1.0F };
        else if (type == "SameLine") result = { 0.0F, 0.0F };
        if (hasField(vm,props,"Width")) result.x=numberField(vm,props,"Width")*parentScale;
        if (hasField(vm,props,"Height")) result.y=numberField(vm,props,"Height")*parentScale;
    }
    result.x*=layout.scale; result.y*=layout.scale;
    result.x = std::max(0.0F, result.x); result.y = std::max(0.0F, result.y);
    const auto automatic = enumField(vm, props, "AutomaticSize", "None");
    const bool autoX = automatic == "X" || automatic == "XY", autoY = automatic == "Y" || automatic == "XY";
    if ((autoX || autoY) && depth<64 && budget.measurements-- >0) {
    ImVec2 content;
    if (type == "TextLabel" || type == "TextButton" || type=="TextBox") {
        const auto text = stringField(vm, props, "Text");
        const bool wrap = booleanField(vm, props, "TextWrapped");
        content = ImGui::GetFont()->CalcTextSizeA(textSize(vm, props)*scale, FLT_MAX, wrap && !autoX ? std::max(1.0F, result.x) : 0.0F, text.c_str());
        if (autoY && !wrap && !autoX) content.y = result.y;
    } else if (type == "Frame" || type == "CanvasGroup" || type == "Window" || type=="ScrollingFrame") {
        const float left = std::max(0.0F, layout.left.resolve(result.x,scale)), right = std::max(0.0F, layout.right.resolve(result.x,scale));
        const float top = std::max(0.0F, layout.top.resolve(result.y,scale)), bottom = std::max(0.0F, layout.bottom.resolve(result.y,scale));
        const ImVec2 inner{ std::max(0.0F, result.x-left-right), std::max(0.0F, result.y-top-bottom) };
        if (layout.grid) content=gridMetrics(layout,inner,scale).total;
        else {
        rawField(vm, instance, "children");
        if (lua_istable(vm, -1)) {
            const int childTable = lua_gettop(vm);
            size_t measured = 0;
            for (const auto& child : layout.children) {
                if (budget.measurements <= 0) break;
                lua_rawgeti(vm, childTable, child.index); rawField(vm, -1, "props");
                const int childProps = lua_gettop(vm);
                const auto size = nodeSize(vm, childProps-1, childProps, stringField(vm, childProps-1, "type"), inner, budget, depth+1,scale);
                if (layout.list) {
                    if (layout.horizontal) { content.x += size.x; content.y = std::max(content.y, size.y); }
                    else { content.y += size.y; content.x = std::max(content.x, size.x); }
                } else {
                    ImVec2 position; dimensions(vm, childProps, "Position", inner, position,scale);
                    const auto anchor = vectorField(vm, childProps, "AnchorPoint");
                    content.x = std::max(content.x, position.x + size.x * (1.0F-anchor.x));
                    content.y = std::max(content.y, position.y + size.y * (1.0F-anchor.y));
                }
                ++measured; lua_pop(vm, 2);
            }
            if (layout.list && measured > 1) {
                const float gap = std::max(0.0F, layout.gap.resolve(layout.horizontal ? inner.x : inner.y,scale)) * static_cast<float>(measured-1);
                if (layout.horizontal) content.x += gap; else content.y += gap;
            }
        }
        lua_pop(vm, 1);
        }
        content.x += left+right; content.y += top+bottom;
    }
    if (autoX) result.x = std::max(result.x, std::min(content.x, std::max(result.x, basis.x)));
    if (autoY) result.y = std::max(result.y, std::min(content.y, std::max(result.y, basis.y)));
    }
    result=constrainedSize(result,layout,scale);
    if (budget.measured.size() < 4096) budget.measured[identity] = {basis, result,parentScale};
    return result;
}
float alignment(const std::string& value, const char* end) {
    return value == "Center" ? 0.5F : value == end ? 1.0F : 0.0F;
}
void drawNode(lua_State* vm, int instance, int depth, Budget& budget, const Rect& parent, bool canvas, const Rect* placed = nullptr, float scale=1.0F);
void drawChildren(lua_State* vm, int instance, int depth, Budget& budget, const Rect& parent, bool canvas, const Modifiers& layout, float scale=1.0F) {
    rawField(vm, instance, "children");
    if (!lua_istable(vm, -1)) { lua_pop(vm, 1); return; }
    const int children = lua_gettop(vm);
    std::vector<ImVec2> sizes;
    float total = 0.0F, cross = 0.0F;
    const auto grid=layout.grid ? gridMetrics(layout,parent.size,scale) : GridMetrics{};
    const float gap = std::max(0.0F, layout.gap.resolve(layout.horizontal ? parent.size.x : parent.size.y,scale));
    if (layout.list && !layout.grid) {
        sizes.reserve(layout.children.size());
        for (const auto& child : layout.children) {
            lua_rawgeti(vm, children, child.index); rawField(vm, -1, "props");
            const auto size = nodeSize(vm, -2, -1, stringField(vm, -2, "type"), parent.size, budget, depth,scale);
            sizes.push_back(size); total += layout.horizontal ? size.x : size.y;
            cross = std::max(cross, layout.horizontal ? size.y : size.x);
            lua_pop(vm, 2);
        }
        if (!sizes.empty()) total += gap * static_cast<float>(sizes.size() - 1);
    }
    if (layout.listIndex) {
            lua_rawgeti(vm, children, layout.listIndex); rawField(vm, -1, "props");
            if (lua_istable(vm, -1)) {
                publishVector(vm, -2, -1, "AbsoluteContentSize", layout.grid ? grid.total : layout.horizontal ? ImVec2(total, cross) : ImVec2(cross, total), budget);
                if (layout.grid) {publishVector(vm,-2,-1,"AbsoluteCellSize",grid.cell,budget); publishVector(vm,-2,-1,"AbsoluteCellCount",{static_cast<float>(grid.columns),static_cast<float>(grid.rows)},budget);}
            }
            lua_pop(vm, 2);
    }
    float offset = (layout.horizontal ? parent.size.x - total : parent.size.y - total) *
        (layout.horizontal ? alignment(layout.horizontalAlignment, "Right") : alignment(layout.verticalAlignment, "Bottom"));
    for (size_t i = 0; i < layout.children.size() && budget.nodes > 0; ++i) {
        lua_rawgeti(vm, children, layout.children[i].index);
        if (layout.grid) {
            const int column=layout.horizontal ? static_cast<int>(i)%grid.columns : static_cast<int>(i)/grid.rows;
            const int row=layout.horizontal ? static_cast<int>(i)/grid.columns : static_cast<int>(i)%grid.rows;
            const float x=layout.startCorner=="TopRight" || layout.startCorner=="BottomRight" ? grid.total.x-grid.cell.x-column*(grid.cell.x+grid.padding.x) : column*(grid.cell.x+grid.padding.x);
            const float y=layout.startCorner=="BottomLeft" || layout.startCorner=="BottomRight" ? grid.total.y-grid.cell.y-row*(grid.cell.y+grid.padding.y) : row*(grid.cell.y+grid.padding.y);
            const Rect rect{{parent.origin.x+x+(parent.size.x-grid.total.x)*alignment(layout.horizontalAlignment,"Right"),
                parent.origin.y+y+(parent.size.y-grid.total.y)*alignment(layout.verticalAlignment,"Bottom")},grid.cell};
            drawNode(vm,lua_gettop(vm),depth,budget,parent,true,&rect,scale);
        } else if (layout.list) {
            const auto size = sizes[i];
            const ImVec2 position = layout.horizontal
                ? ImVec2(parent.origin.x + offset, parent.origin.y + (parent.size.y - size.y) * alignment(layout.verticalAlignment, "Bottom"))
                : ImVec2(parent.origin.x + (parent.size.x - size.x) * alignment(layout.horizontalAlignment, "Right"), parent.origin.y + offset);
            const Rect rect{ position, size };
            drawNode(vm, lua_gettop(vm), depth, budget, parent, true, &rect,scale);
            offset += (layout.horizontal ? size.x : size.y) + gap;
        } else drawNode(vm, lua_gettop(vm), depth, budget, parent, canvas,nullptr,scale);
        lua_pop(vm, 1);
    }
    for (const auto& child:layout.structural) {
        if (budget.nodes<=0) break;
        lua_rawgeti(vm,children,child.index); drawNode(vm,-1,depth,budget,parent,canvas,nullptr,scale); lua_pop(vm,1);
    }
    lua_pop(vm, 1);
}
struct Gradient {
    struct Point { float time; ImVec4 color; };
    std::vector<Point> colors, transparency;
    float rotation = 0;
    float axisX=1,axisY=0;
    ImVec2 offset;
    ImVec4 sample(const ImVec2 point, const Rect& rect) const {
        const float t = std::clamp(0.5F + ((point.x-rect.origin.x)/std::max(1.0F,rect.size.x)-0.5F-offset.x)*axisX +
            ((point.y-rect.origin.y)/std::max(1.0F,rect.size.y)-0.5F-offset.y)*axisY, 0.0F, 1.0F);
        const auto interpolate = [t](const std::vector<Point>& points, ImVec4 fallback) {
            if (points.empty()) return fallback;
            if (t <= points.front().time) return points.front().color;
            for (size_t index=1; index<points.size(); ++index) if (t <= points[index].time) {
                const auto& a=points[index-1]; const auto& b=points[index];
                const float factor=(t-a.time)/std::max(0.00001F,b.time-a.time);
                return ImVec4(a.color.x+(b.color.x-a.color.x)*factor,a.color.y+(b.color.y-a.color.y)*factor,
                    a.color.z+(b.color.z-a.color.z)*factor,a.color.w+(b.color.w-a.color.w)*factor);
            }
            return points.back().color;
        };
        auto result=interpolate(colors,ImVec4(1,1,1,1));
        result.w=1.0F-std::clamp(interpolate(transparency,ImVec4(0,0,0,0)).x,0.0F,1.0F);
        return result;
    }
};
Gradient readGradient(lua_State* vm, int instance, int modifier) {
    Gradient result;
    if (!modifier) return result;
    rawField(vm,instance,"children");
    if (!lua_istable(vm,-1)) {lua_pop(vm,1); return result;}
    lua_rawgeti(vm,-1,modifier); rawField(vm,-1,"props");
    const int props=lua_gettop(vm);
    if (lua_istable(vm,props)) {
        result.rotation=numberField(vm,props,"Rotation"); result.offset=vectorField(vm,props,"Offset");
        const float radians=result.rotation*0.017453292519943295F; result.axisX=std::cos(radians); result.axisY=std::sin(radians);
        for (int kind=0; kind<2; ++kind) {
            auto& points=kind==0 ? result.colors : result.transparency;
            rawField(vm,props,kind==0 ? "Color" : "Transparency");
            if (lua_istable(vm,-1)) {
                rawField(vm,-1,"Keypoints");
                if (lua_istable(vm,-1)) {
                    const int table=lua_gettop(vm), count=std::min(lua_objlen(vm,table),20);
                    for (int index=1; index<=count; ++index) {
                        lua_rawgeti(vm,table,index);
                        if (lua_istable(vm,-1)) points.push_back({std::clamp(numberField(vm,-1,"Time"),0.0F,1.0F),
                            kind==0 ? colorField(vm,-1,"Value",ImVec4(1,1,1,1)) : ImVec4(std::clamp(numberField(vm,-1,"Value"),0.0F,1.0F),0,0,0)});
                        lua_pop(vm,1);
                    }
                    std::stable_sort(points.begin(),points.end(),[](const auto& a,const auto& b){return a.time<b.time;});
                }
                lua_pop(vm,1);
            }
            lua_pop(vm,1);
        }
    }
    lua_pop(vm,3);
    return result;
}
ImU32 tinted(const ImVec4 base, const Gradient* gradient, const ImVec2 position, const Rect& rect) {
    const auto color=gradient ? gradient->sample(position,rect) : ImVec4(1,1,1,1);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(base.x*color.x,base.y*color.y,base.z*color.z,base.w*color.w));
}
// A bounded row mesh follows the rounded parent's boundary. Unlike recoloring a
// four-corner quad, interior vertices preserve intermediate sequence keypoints.
void surface(const Rect& patch, const Rect& clip, float radius, ImVec2 uvMin, ImVec2 uvMax, ImVec4 color,
             const Gradient* gradient, Budget& budget, const ReactUI::Image* image=nullptr) {
    if (patch.size.x<=0 || patch.size.y<=0 || color.w<=0) return;
    auto* draw=ImGui::GetWindowDrawList();
    const bool largeIndices=(draw->Flags & ImDrawListFlags_AllowVtxOffset)!=0 || sizeof(ImDrawIdx)>2;
    const int available=std::min(budget.meshVertices,largeIndices ? budget.meshVertices : std::max(0,60000-draw->VtxBuffer.Size));
    if (available<4) return;
    const ImVec2 end{patch.origin.x+patch.size.x,patch.origin.y+patch.size.y};
    const bool same=patch.origin.x==clip.origin.x && patch.origin.y==clip.origin.y && patch.size.x==clip.size.x && patch.size.y==clip.size.y;
    if (!gradient && (radius<=0 || same)) {
        const int estimate=radius>0.5F ? 128 : 4;
        if (estimate>available) radius=0;
        budget.meshVertices-=radius>0.5F ? 128 : 4;
        if (image) draw->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(image->textureId))),patch.origin,end,uvMin,uvMax,ImGui::ColorConvertFloat4ToU32(color),radius);
        else draw->AddRectFilled(patch.origin,end,ImGui::ColorConvertFloat4ToU32(color),radius);
        return;
    }
    const float top=std::max(patch.origin.y,clip.origin.y), bottom=std::min(end.y,clip.origin.y+clip.size.y);
    if (bottom<=top) return;
    radius=std::clamp(radius,0.0F,std::min(clip.size.x,clip.size.y)*0.5F);
    std::vector<float> rows{top,bottom};
    if (gradient) for (int row=1; row<16; ++row) rows.push_back(top+(bottom-top)*static_cast<float>(row)/16.0F);
    if (radius>0) for (int step=0; step<=8; ++step) {
        const float extent=radius*(1.0F-std::cos(static_cast<float>(step)*1.57079632679F/8.0F));
        const float upper=clip.origin.y+extent, lower=clip.origin.y+clip.size.y-extent;
        if (upper>top && upper<bottom) rows.push_back(upper);
        if (lower>top && lower<bottom) rows.push_back(lower);
    }
    std::sort(rows.begin(),rows.end()); rows.erase(std::unique(rows.begin(),rows.end()),rows.end());
    const int columns=gradient ? 16 : 1;
    const int needed=static_cast<int>(rows.size()-1)*columns*4;
    if (needed>available) {
        const float rounding=same && available>=128 ? radius : 0;
        budget.meshVertices-=rounding>0.5F ? 128 : 4;
        const auto fallback=tinted(color,gradient,{patch.origin.x+patch.size.x*.5F,patch.origin.y+patch.size.y*.5F},clip);
        if (image) draw->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(image->textureId))),patch.origin,end,uvMin,uvMax,fallback,rounding);
        else draw->AddRectFilled(patch.origin,end,fallback,rounding);
        return;
    }
    budget.meshVertices-=needed;
    const auto range=[&](float y) {
        const float distance=std::min(y-clip.origin.y,clip.origin.y+clip.size.y-y);
        const float inset=distance<radius ? radius-std::sqrt(std::max(0.0F,radius*radius-(radius-distance)*(radius-distance))) : 0.0F;
        return ImVec2(std::max(patch.origin.x,clip.origin.x+inset),std::min(end.x,clip.origin.x+clip.size.x-inset));
    };
    const auto uv=[&](ImVec2 position) {
        return image ? ImVec2(uvMin.x+(uvMax.x-uvMin.x)*(position.x-patch.origin.x)/patch.size.x,
            uvMin.y+(uvMax.y-uvMin.y)*(position.y-patch.origin.y)/patch.size.y) : ImGui::GetFontTexUvWhitePixel();
    };
    if (image) draw->PushTexture(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(image->textureId))));
    for (size_t row=1; row<rows.size(); ++row) {
        const auto upper=range(rows[row-1]),lower=range(rows[row]);
        if (upper.y<upper.x || lower.y<lower.x) continue;
        for (int column=0; column<columns; ++column) {
            const float a=static_cast<float>(column)/columns,b=static_cast<float>(column+1)/columns;
            const std::array<ImVec2,4> positions{{{upper.x+(upper.y-upper.x)*a,rows[row-1]},{upper.x+(upper.y-upper.x)*b,rows[row-1]},
                {lower.x+(lower.y-lower.x)*b,rows[row]},{lower.x+(lower.y-lower.x)*a,rows[row]}}};
            draw->PrimReserve(6,4); const auto first=draw->_VtxCurrentIdx;
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(first)); draw->PrimWriteIdx(static_cast<ImDrawIdx>(first+1)); draw->PrimWriteIdx(static_cast<ImDrawIdx>(first+2));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(first)); draw->PrimWriteIdx(static_cast<ImDrawIdx>(first+2)); draw->PrimWriteIdx(static_cast<ImDrawIdx>(first+3));
            for (const auto position:positions) draw->PrimWriteVtx(position,uv(position),tinted(color,gradient,position,clip));
        }
    }
    if (image) draw->PopTexture();
}
void background(lua_State* vm, int props, const Rect& rect, float radius, const std::string& type, Budget& budget,
                const Gradient* gradient, bool hovered = false, bool active = false, float scale=1.0F) {
    const bool button=type=="TextButton" || type=="ImageButton";
    auto color = colorField(vm, props, "BackgroundColor3", ImGui::GetStyle().Colors[button ? ImGuiCol_Button : ImGuiCol_ChildBg]);
    color.w *= 1.0F - std::clamp(numberField(vm, props, "BackgroundTransparency"), 0.0F, 1.0F);
    if (button && booleanField(vm, props, "AutoButtonColor", true) && (hovered || active)) {
        const float change = active ? -0.10F : 0.10F;
        color.x = std::clamp(color.x + change, 0.0F, 1.0F); color.y = std::clamp(color.y + change, 0.0F, 1.0F);
        color.z = std::clamp(color.z + change, 0.0F, 1.0F);
    }
    color.w *= ImGui::GetStyle().Alpha;
    const ImVec2 end{ rect.origin.x + rect.size.x, rect.origin.y + rect.size.y };
    surface(rect,rect,radius,{}, {},color,gradient,budget);
    const float border = std::clamp(numberField(vm, props, "BorderSizePixel")*scale, 0.0F, std::min(rect.size.x, rect.size.y) * 0.5F);
    if (border > 0.0F) {
        auto colorBorder = colorField(vm, props, "BorderColor3", ImGui::GetStyle().Colors[ImGuiCol_Border]);
        colorBorder.w *= ImGui::GetStyle().Alpha;
        ImGui::GetWindowDrawList()->AddRect(rect.origin, end, ImGui::ColorConvertFloat4ToU32(colorBorder), radius, 0, border);
    }
}
void publishBoolean(lua_State* vm, int instance, int props, const char* property, bool value, Budget& budget) {
    instance=absolute(vm,instance); props=absolute(vm,props);
    if (budget.valueWrites<=0 || budget.allocationBytes<512 || lua_getreadonly(vm,props)) return;
    rawField(vm,props,property);
    const bool same=lua_type(vm,-1)==LUA_TBOOLEAN && (lua_toboolean(vm,-1)!=0)==value; lua_pop(vm,1);
    if (same) return;
    --budget.valueWrites;
    budget.allocationBytes=budget.allocationBytes>512 ? budget.allocationBytes-512 : 0;
    lua_pushstring(vm,property); lua_pushboolean(vm,value); lua_rawset(vm,props);
    lua_pushboolean(vm,value); const auto event=std::string("Change.")+property; enqueue(vm,instance,event.c_str(),-1); lua_pop(vm,1);
}
ReactUI::Image resolveImage(std::string_view path, Budget& budget) {
    const std::string key(path);
    if (const auto found=budget.images.find(key); found!=budget.images.end()) return found->second;
    ReactUI::Image image;
    if (budget.images.size()<256 && budget.imageResolver && *budget.imageResolver) {
        try { image=(*budget.imageResolver)(path); } catch (...) { image={}; }
    }
    if (!std::isfinite(image.width) || !std::isfinite(image.height) || image.width<=0 || image.height<=0) image={};
    if (budget.images.size()<256) budget.images.emplace(key,image);
    return image;
}
void imageNode(lua_State* vm, int instance, int props, const Rect& rect, float radius, bool hovered, bool active,
               const Gradient* gradient, Budget& budget, float pixelScale) {
    auto path=stringField(vm,props,"Image");
    const auto alternate=stringField(vm,props,active ? "PressedImage" : hovered ? "HoverImage" : "Image");
    if (!alternate.empty()) path=alternate;
    const auto image=path.empty() ? ReactUI::Image{} : resolveImage(path,budget);
    publishBoolean(vm,instance,props,"IsLoaded",image.textureId!=nullptr,budget);
    if (path.empty()) return;
    auto color=colorField(vm,props,"ImageColor3",ImVec4(1,1,1,1));
    color.w=(1.0F-std::clamp(numberField(vm,props,"ImageTransparency"),0.0F,1.0F))*ImGui::GetStyle().Alpha;
    if (color.w<=0) return;
    if (!image.textureId) {
        surface(rect,rect,radius,{}, {},ImVec4(.24F,.27F,.32F,color.w),gradient,budget);
        auto* draw=ImGui::GetWindowDrawList();
        const ImVec2 margin{std::min(8.0F,rect.size.x*.2F),std::min(8.0F,rect.size.y*.2F)};
        draw->AddLine({rect.origin.x+margin.x,rect.origin.y+margin.y},{rect.origin.x+rect.size.x-margin.x,rect.origin.y+rect.size.y-margin.y},IM_COL32(240,125,80,180),2);
        draw->AddLine({rect.origin.x+margin.x,rect.origin.y+rect.size.y-margin.y},{rect.origin.x+rect.size.x-margin.x,rect.origin.y+margin.y},IM_COL32(240,125,80,180),2);
        return;
    }
    auto offset=vectorField(vm,props,"ImageRectOffset"),sourceSize=vectorField(vm,props,"ImageRectSize");
    offset.x=std::clamp(offset.x,0.0F,image.width); offset.y=std::clamp(offset.y,0.0F,image.height);
    sourceSize.x=sourceSize.x>0 ? std::min(sourceSize.x,image.width-offset.x) : image.width-offset.x;
    sourceSize.y=sourceSize.y>0 ? std::min(sourceSize.y,image.height-offset.y) : image.height-offset.y;
    if (sourceSize.x<=0 || sourceSize.y<=0 || rect.size.x<=0 || rect.size.y<=0) return;
    const auto uv=[&](ImVec2 position) {return ImVec2((offset.x+position.x)/image.width,(offset.y+position.y)/image.height);};
    const auto paint=[&](Rect patch,ImVec2 minimum,ImVec2 maximum) {surface(patch,rect,radius,uv(minimum),uv(maximum),color,gradient,budget,&image);};
    const auto scaleType=enumField(vm,props,"ScaleType","Stretch");
    if (scaleType=="Fit") {
        const float factor=std::min(rect.size.x/sourceSize.x,rect.size.y/sourceSize.y);
        const ImVec2 size{sourceSize.x*factor,sourceSize.y*factor};
        paint({{rect.origin.x+(rect.size.x-size.x)*.5F,rect.origin.y+(rect.size.y-size.y)*.5F},size},{},sourceSize);
    } else if (scaleType=="Crop") {
        const float factor=std::max(rect.size.x/sourceSize.x,rect.size.y/sourceSize.y);
        const ImVec2 size{rect.size.x/factor,rect.size.y/factor},start{(sourceSize.x-size.x)*.5F,(sourceSize.y-size.y)*.5F};
        paint(rect,start,{start.x+size.x,start.y+size.y});
    } else if (scaleType=="Tile") {
        ImVec2 tile{sourceSize.x*pixelScale,sourceSize.y*pixelScale}; dimensions(vm,props,"TileSize",rect.size,tile,pixelScale);
        tile.x=std::max(1.0F,tile.x); tile.y=std::max(1.0F,tile.y);
        int remaining=256;
        for (float y=0; y<rect.size.y && remaining>0; y+=tile.y) for (float x=0; x<rect.size.x && remaining-- >0; x+=tile.x) {
            const ImVec2 size{std::min(tile.x,rect.size.x-x),std::min(tile.y,rect.size.y-y)};
            paint({{rect.origin.x+x,rect.origin.y+y},size},{},{sourceSize.x*size.x/tile.x,sourceSize.y*size.y/tile.y});
        }
    } else if (scaleType=="Slice") {
        rawField(vm,props,"SliceCenter");
        ImVec2 minimum,maximum;
        if (lua_istable(vm,-1)) {minimum=vectorField(vm,-1,"Min"); maximum=vectorField(vm,-1,"Max");}
        lua_pop(vm,1);
        minimum.x=std::clamp(minimum.x,0.0F,sourceSize.x); minimum.y=std::clamp(minimum.y,0.0F,sourceSize.y);
        maximum.x=std::clamp(maximum.x,minimum.x,sourceSize.x); maximum.y=std::clamp(maximum.y,minimum.y,sourceSize.y);
        if (maximum.x<=minimum.x || maximum.y<=minimum.y) {paint(rect,{},sourceSize); return;}
        const float scale=std::clamp(numberField(vm,props,"SliceScale",1)*pixelScale,0.001F,100.0F);
        const float borderX=minimum.x+(sourceSize.x-maximum.x),borderY=minimum.y+(sourceSize.y-maximum.y);
        const float sx=borderX>0 ? std::min(scale,rect.size.x/borderX) : scale,sy=borderY>0 ? std::min(scale,rect.size.y/borderY) : scale;
        const std::array<float,4> xs{{0,minimum.x*sx,rect.size.x-(sourceSize.x-maximum.x)*sx,rect.size.x}};
        const std::array<float,4> ys{{0,minimum.y*sy,rect.size.y-(sourceSize.y-maximum.y)*sy,rect.size.y}};
        const std::array<float,4> us{{0,minimum.x,maximum.x,sourceSize.x}},vs{{0,minimum.y,maximum.y,sourceSize.y}};
        for (int row=0; row<3; ++row) for (int column=0; column<3; ++column)
            paint({{rect.origin.x+xs[column],rect.origin.y+ys[row]},{xs[column+1]-xs[column],ys[row+1]-ys[row]}},
                {us[column],vs[row]},{us[column+1],vs[row+1]});
    } else paint(rect,{},sourceSize);
}
void label(lua_State* vm, int props, const Rect& rect, const std::string& text, bool button, const Gradient* gradient, Budget& budget, float scale,
           const ImVec4* overrideColor=nullptr) {
    auto color = overrideColor ? *overrideColor : colorField(vm, props, "TextColor", ImGui::GetStyle().Colors[ImGuiCol_Text]);
    if (!overrideColor) color = colorField(vm, props, "TextColor3", color);
    color.w *= (1.0F - std::clamp(numberField(vm, props, "TextTransparency"), 0.0F, 1.0F)) * ImGui::GetStyle().Alpha;
    const float fontSize = std::clamp(textSize(vm, props)*scale,1.0F,512.0F);
    const bool wrapped = booleanField(vm, props, "TextWrapped");
    const auto size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, wrapped ? std::max(1.0F, rect.size.x) : 0.0F, text.c_str());
    const float x = alignment(enumField(vm, props, "TextXAlignment", button ? "Center" : "Left"), "Right");
    const float y = alignment(enumField(vm, props, "TextYAlignment", "Center"), "Bottom");
    const ImVec2 position{ rect.origin.x + std::max(0.0F, rect.size.x - size.x) * x, rect.origin.y + std::max(0.0F, rect.size.y - size.y) * y };
    const ImVec4 clip{ rect.origin.x, rect.origin.y, rect.origin.x + rect.size.x, rect.origin.y + rect.size.y };
    auto* draw=ImGui::GetWindowDrawList(); const int first=draw->VtxBuffer.Size;
    int bytes=std::min(static_cast<int>(text.size()),budget.textBytes);
    if (!(draw->Flags & ImDrawListFlags_AllowVtxOffset) && sizeof(ImDrawIdx)==2) bytes=std::min(bytes,std::max(0,60000-draw->VtxBuffer.Size)/4);
    while (bytes>0 && bytes<static_cast<int>(text.size()) && (static_cast<unsigned char>(text[bytes])&0xC0)==0x80) --bytes;
    budget.textBytes-=bytes;
    draw->AddText(ImGui::GetFont(), fontSize, position, ImGui::ColorConvertFloat4ToU32(color), text.c_str(), text.c_str()+bytes,
        wrapped ? std::max(1.0F, rect.size.x) : 0.0F, &clip);
    if (gradient) for (int index=first; index<draw->VtxBuffer.Size && budget.meshVertices-- >0; ++index)
        draw->VtxBuffer[index].col=tinted(color,gradient,draw->VtxBuffer[index].pos,rect);
}
bool wantsEvent(lua_State* vm,int instance,const char* name) {
    instance=absolute(vm,instance);
    rawField(vm,instance,"events");
    bool found=lua_istable(vm,-1) && hasField(vm,-1,name); lua_pop(vm,1);
    if (!found) {rawField(vm,instance,"_signals"); found=lua_istable(vm,-1) && hasField(vm,-1,name); lua_pop(vm,1);}
    return found;
}
bool spend(Budget& budget,size_t bytes) {
    const int units=static_cast<int>((bytes+511)/512);
    if (budget.inputWrites<units || budget.allocationBytes<bytes) return false;
    budget.inputWrites-=units; budget.allocationBytes-=bytes; return true;
}
void mouseEvent(lua_State* vm,int instance,const char* name,Budget& budget) {
    if (!wantsEvent(vm,instance,name) || !spend(budget,512)) return;
    const auto mouse=ImGui::GetIO().MousePos;
    lua_createtable(vm,2,1); lua_pushnumber(vm,mouse.x); lua_rawseti(vm,-2,1); lua_pushnumber(vm,mouse.y); lua_rawseti(vm,-2,2);
    lua_pushinteger(vm,2); lua_setfield(vm,-2,"n"); enqueue(vm,instance,name,0,-1); lua_pop(vm,1);
}
void hoverEvents(lua_State* vm,int instance,const Rect& rect,bool button,Budget& budget) {
    instance=absolute(vm,instance);
    const bool enabled=!(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
    const bool hovered=enabled && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseHoveringRect(rect.origin,{rect.origin.x+rect.size.x,rect.origin.y+rect.size.y});
    if (wantsEvent(vm,instance,"MouseEnter") || wantsEvent(vm,instance,"MouseLeave")) {
        const bool previous=booleanField(vm,instance,"_nativeHovered");
        if (hovered!=previous && !lua_getreadonly(vm,instance) && spend(budget,512)) {
            lua_pushstring(vm,"_nativeHovered"); lua_pushboolean(vm,hovered); lua_rawset(vm,instance);
            mouseEvent(vm,instance,hovered ? "MouseEnter" : "MouseLeave",budget);
        }
    }
    if (enabled && button && ImGui::IsItemActivated()) mouseEvent(vm,instance,"MouseButton1Down",budget);
    if (enabled && button && ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) mouseEvent(vm,instance,"MouseButton1Up",budget);
}
void publishText(lua_State* vm,int instance,int props,std::string_view text,Budget& budget) {
    instance=absolute(vm,instance); props=absolute(vm,props);
    if (lua_getreadonly(vm,props) || !spend(budget,text.size()+1024)) return;
    lua_pushlstring(vm,text.data(),text.size()); const int value=lua_gettop(vm);
    lua_pushstring(vm,"Text"); lua_pushvalue(vm,value); lua_rawset(vm,props);
    enqueue(vm,instance,"Change.Text",value); lua_pop(vm,1);
}
void textBox(lua_State* vm,int instance,int props,const Rect& rect,const Gradient* gradient,Budget& budget,float scale) {
    instance=absolute(vm,instance); props=absolute(vm,props);
    const auto original=stringField(vm,props,"Text"),placeholder=stringField(vm,props,"PlaceholderText");
    std::array<char,32769> buffer{}; std::copy(original.begin(),original.end(),buffer.begin());
    const auto id=ImGui::GetID("##text");
    const bool enabled=!(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
    if (!enabled && ImGui::GetCurrentContext()->ActiveId==id) ImGui::ClearActiveID();
    const bool capture=booleanField(vm,instance,"_focusRequested"),release=hasField(vm,instance,"_releaseFocusRequested");
    const bool releaseEnter=booleanField(vm,instance,"_releaseFocusRequested");
    if (!lua_getreadonly(vm,instance)) {
        if (capture && spend(budget,512)) {lua_pushstring(vm,"_focusRequested"); lua_pushnil(vm); lua_rawset(vm,instance); if (enabled) ImGui::SetKeyboardFocusHere();}
        if (release && spend(budget,512)) {lua_pushstring(vm,"_releaseFocusRequested"); lua_pushnil(vm); lua_rawset(vm,instance);}
    }
    if (auto* state=ImGui::GetInputTextState(id); state && std::string_view(state->TextA.Data,state->TextLen)!=original) state->ReloadUserBufAndKeepSelection();
    auto color=colorField(vm,props,"TextColor3",ImGui::GetStyle().Colors[ImGuiCol_Text]);
    const auto plainColor=color;
    color.w*=1.0F-std::clamp(numberField(vm,props,"TextTransparency"),0.0F,1.0F);
    const bool multiline=booleanField(vm,props,"MultiLine");
    auto* draw=ImGui::GetWindowDrawList();
    const bool boundedText=(!(draw->Flags & ImDrawListFlags_AllowVtxOffset) && sizeof(ImDrawIdx)==2) || original.size()>static_cast<size_t>(budget.textBytes);
    const auto placeholderColor=colorField(vm,props,"PlaceholderColor3",ImVec4(.55F,.55F,.55F,1));
    ImGui::PushStyleColor(ImGuiCol_Text,boundedText ? ImVec4(0,0,0,0) : color);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,boundedText ? ImVec4(0,0,0,0) : placeholderColor);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg,boundedText ? ImVec4(0,0,0,0) : ImGui::GetStyle().Colors[ImGuiCol_TextSelectedBg]);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(0,0,0,0)); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(0,0,0,0)); ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(0,0,0,0));
    const float size=std::clamp(textSize(vm,props)*scale,1.0F,512.0F);
    ImGui::PushFont(nullptr,size); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(0,multiline ? 0 : std::max(0.0F,(rect.size.y-size)*.5F)));
    ImGui::SetCursorScreenPos(rect.origin); ImGui::SetNextItemWidth(std::max(1.0F,rect.size.x));
    ImGuiInputTextFlags flags=enabled && booleanField(vm,props,"TextEditable",true) ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_ReadOnly;
    if (!multiline) flags|=ImGuiInputTextFlags_EnterReturnsTrue;
    const int first=draw->VtxBuffer.Size;
    const bool entered=multiline ? ImGui::InputTextMultiline("##text",buffer.data(),buffer.size(),rect.size,flags) :
        ImGui::InputTextWithHint("##text",placeholder.c_str(),buffer.data(),buffer.size(),flags);
    if (ImGui::IsItemActivated() && booleanField(vm,props,"ClearTextOnFocus",true) && booleanField(vm,props,"TextEditable",true)) {
        buffer[0]=0; if (auto* state=ImGui::GetInputTextState(id)) state->ClearText();
    }
    const bool enterPressed=(entered && !multiline) || (release && releaseEnter);
    if ((enterPressed || release) && ImGui::GetCurrentContext()->ActiveId==id) ImGui::ClearActiveID();
    const bool focused=ImGui::IsItemActive(),wasFocused=booleanField(vm,instance,"_focused");
    if (focused!=wasFocused && !lua_getreadonly(vm,instance) && spend(budget,512)) {
        lua_pushstring(vm,"_focused"); lua_pushboolean(vm,focused); lua_rawset(vm,instance);
        if (wantsEvent(vm,instance,focused ? "Focused" : "FocusLost") && spend(budget,512)) {
            if (focused) enqueue(vm,instance,"Focused");
            else {lua_createtable(vm,1,1); lua_pushboolean(vm,enterPressed); lua_rawseti(vm,-2,1); lua_pushinteger(vm,1); lua_setfield(vm,-2,"n"); enqueue(vm,instance,"FocusLost",0,-1); lua_pop(vm,1);}
        }
    }
    const std::string_view current(buffer.data());
    if (current!=original) publishText(vm,instance,props,current,budget);
    if (!boundedText && multiline && current.empty() && !focused && !placeholder.empty()) {
        auto hint=colorField(vm,props,"PlaceholderColor3",ImVec4(.55F,.55F,.55F,1)); hint.w*=ImGui::GetStyle().Alpha;
        const ImVec4 clip{rect.origin.x,rect.origin.y,rect.origin.x+rect.size.x,rect.origin.y+rect.size.y};
        draw->AddText(ImGui::GetFont(),size,rect.origin,ImGui::ColorConvertFloat4ToU32(hint),placeholder.c_str(),nullptr,rect.size.x,&clip);
    }
    if (gradient) for (int index=first; index<draw->VtxBuffer.Size && budget.meshVertices-- >0; ++index) {
        const auto base=ImGui::ColorConvertU32ToFloat4(draw->VtxBuffer[index].col); draw->VtxBuffer[index].col=tinted(base,gradient,draw->VtxBuffer[index].pos,rect);
    }
    ImGui::PopStyleVar(2); ImGui::PopFont(); ImGui::PopStyleColor(6);
    if (boundedText) {
        // Keep the full editable UTF-8 buffer and clipboard behavior, while the
        // fallback renderer cannot exceed a 16-bit draw list's vertex range.
        const bool hint=current.empty() && !focused;
        label(vm,props,rect,hint ? placeholder : std::string(current),false,gradient,budget,scale,hint ? &placeholderColor : &plainColor);
    } else budget.textBytes=std::max(0,budget.textBytes-static_cast<int>(std::max(current.size(),current.empty() ? placeholder.size() : size_t{0})));
    publishVector(vm,instance,props,"TextBounds",ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,multiline ? rect.size.x : 0,current.data()),budget);
}
ImVec2 contentSize(lua_State* vm,int instance,const Modifiers& layout,ImVec2 basis,Budget& budget,int depth,float scale) {
    if (layout.grid) return gridMetrics(layout,basis,scale).total;
    ImVec2 result; rawField(vm,instance,"children");
    if (lua_istable(vm,-1)) {
        const int children=lua_gettop(vm);
        int count=0;
        for (const auto& child:layout.children) {
            if (budget.measurements--<=0) break;
            lua_rawgeti(vm,children,child.index); rawField(vm,-1,"props");
            const auto size=nodeSize(vm,-2,-1,stringField(vm,-2,"type"),basis,budget,depth+1,scale);
            if (layout.list) {
                if (layout.horizontal) {result.x+=size.x; result.y=std::max(result.y,size.y);}
                else {result.y+=size.y; result.x=std::max(result.x,size.x);}
            } else {
                ImVec2 position; dimensions(vm,-1,"Position",basis,position,scale); const auto anchor=vectorField(vm,-1,"AnchorPoint");
                result.x=std::max(result.x,position.x+size.x*(1-anchor.x)); result.y=std::max(result.y,position.y+size.y*(1-anchor.y));
            }
            ++count; lua_pop(vm,2);
        }
        if (layout.list && count>1) {
            const float gap=std::max(0.0F,layout.gap.resolve(layout.horizontal ? basis.x : basis.y,scale))*(count-1);
            if (layout.horizontal) result.x+=gap; else result.y+=gap;
        }
    }
    lua_pop(vm,1); return result;
}
Rect scrolling(lua_State* vm,int instance,int props,const Rect& window,const Modifiers& layout,Budget& budget,int depth,float scale) {
    const auto direction=enumField(vm,props,"ScrollingDirection","XY"),automatic=enumField(vm,props,"AutomaticCanvasSize","None");
    const bool x=direction=="X" || direction=="XY",y=direction=="Y" || direction=="XY";
    const bool enabled=booleanField(vm,props,"ScrollingEnabled",true) && !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
    ImVec2 canvas; dimensions(vm,props,"CanvasSize",window.size,canvas,scale);
    const auto content=contentSize(vm,instance,layout,window.size,budget,depth,scale);
    if (automatic=="X" || automatic=="XY") canvas.x=std::max(canvas.x,content.x);
    if (automatic=="Y" || automatic=="XY") canvas.y=std::max(canvas.y,content.y);
    canvas.x=std::clamp(std::max(window.size.x,canvas.x),0.0F,1000000.0F); canvas.y=std::clamp(std::max(window.size.y,canvas.y),0.0F,1000000.0F);
    const float thickness=std::clamp(numberField(vm,props,"ScrollBarThickness",12)*scale,0.0F,std::min(window.size.x,window.size.y)*.25F);
    const bool barX=x && canvas.x>window.size.x+.5F,barY=y && canvas.y>window.size.y+.5F;
    Rect viewport=window;
    if (barX) viewport.size.y=std::max(0.0F,viewport.size.y-thickness);
    if (barY) viewport.size.x=std::max(0.0F,viewport.size.x-thickness);
    auto position=vectorField(vm,props,"CanvasPosition");
    ImVec2 maximum{std::max(0.0F,canvas.x-viewport.size.x),std::max(0.0F,canvas.y-viewport.size.y)};
    if (enabled && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(window.origin,{window.origin.x+window.size.x,window.origin.y+window.size.y})) {
        const auto& io=ImGui::GetIO();
        if (x) position.x-=(io.MouseWheelH+(io.KeyShift ? io.MouseWheel : 0))*40.0F*scale;
        if (y && !io.KeyShift) position.y-=io.MouseWheel*40.0F*scale;
    }
    auto color=colorField(vm,props,"ScrollBarImageColor3",ImVec4(.6F,.65F,.7F,1));
    color.w*=1.0F-std::clamp(numberField(vm,props,"ScrollBarImageTransparency"),0.0F,1.0F);
    const auto bar=[&](bool horizontal) {
        const float length=horizontal ? viewport.size.x : viewport.size.y,extent=horizontal ? canvas.x : canvas.y;
        if (thickness<=0 || length<=0 || extent<=0) return;
        const ImVec2 origin=horizontal ? ImVec2(window.origin.x,window.origin.y+viewport.size.y) : ImVec2(window.origin.x+viewport.size.x,window.origin.y);
        const ImVec2 size=horizontal ? ImVec2(length,thickness) : ImVec2(thickness,length);
        const float thumb=std::max(thickness,std::min(length,length*length/extent));
        float& offset=horizontal ? position.x : position.y; const float max=horizontal ? maximum.x : maximum.y;
        ImGui::SetCursorScreenPos(origin); ImGui::BeginDisabled(!enabled);
        ImGui::InvisibleButton(horizontal ? "scroll-x" : "scroll-y",size);
        if (enabled && ImGui::IsItemActive() && max>0 && length>thumb) offset=((horizontal ? ImGui::GetIO().MousePos.x-origin.x : ImGui::GetIO().MousePos.y-origin.y)-thumb*.5F)/(length-thumb)*max;
        ImGui::EndDisabled();
        offset=std::clamp(offset,0.0F,max);
        auto* draw=ImGui::GetWindowDrawList(); draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(50,58,68,130),thickness*.3F);
        const float start=max>0 ? offset/max*(length-thumb) : 0;
        const ImVec2 minimum=horizontal ? ImVec2(origin.x+start,origin.y) : ImVec2(origin.x,origin.y+start);
        draw->AddRectFilled(minimum,{minimum.x+(horizontal ? thumb : thickness),minimum.y+(horizontal ? thickness : thumb)},ImGui::ColorConvertFloat4ToU32(color),thickness*.3F);
    };
    if (barX) bar(true); if (barY) bar(false);
    position.x=std::clamp(position.x,0.0F,maximum.x); position.y=std::clamp(position.y,0.0F,maximum.y);
    publishVector(vm,instance,props,"CanvasPosition",position,budget);
    publishVector(vm,instance,props,"AbsoluteCanvasSize",canvas,budget); publishVector(vm,instance,props,"AbsoluteWindowSize",viewport.size,budget);
    ImGui::PushClipRect(viewport.origin,{viewport.origin.x+viewport.size.x,viewport.origin.y+viewport.size.y},true);
    return {{viewport.origin.x-position.x,viewport.origin.y-position.y},canvas};
}
void changedBoolean(lua_State* vm, int instance, bool value) {
    lua_pushboolean(vm, value); enqueue(vm, instance, "Changed", -1); enqueue(vm, instance, "Change.Checked", -1); lua_pop(vm, 1);
}
void changedNumber(lua_State* vm, int instance, float value) {
    lua_pushnumber(vm, value); enqueue(vm, instance, "Changed", -1); enqueue(vm, instance, "Change.Value", -1); lua_pop(vm, 1);
}
void drawNode(lua_State* vm, int instance, int depth, Budget& budget, const Rect& parent, bool canvas, const Rect* placed, float parentScale) {
    if (depth > 64 || budget.nodes-- <= 0 || !lua_istable(vm, instance)) return;
    const auto* draw=ImGui::GetWindowDrawList();
    if (!(draw->Flags & ImDrawListFlags_AllowVtxOffset) && sizeof(ImDrawIdx)==2 && draw->VtxBuffer.Size>58000) return;
    instance = absolute(vm, instance);
    if (booleanField(vm, instance, "hidden")) return;
    const auto type = stringField(vm, instance, "type");
    rawField(vm, instance, "props");
    if (!lua_istable(vm, -1)) { lua_pop(vm, 1); return; }
    const int props = lua_gettop(vm);
    if (!booleanField(vm, props, "Visible", true) || (type == "ScreenGui" && !booleanField(vm, props, "Enabled", true))) { lua_pop(vm, 1); return; }
    const auto text = stringField(vm, props, "Text", type.c_str());
    const auto children = modifiers(vm, instance, budget);
    const float scale=std::clamp(parentScale*children.scale,0.0F,10000.0F);
    const bool canvasGroup=type=="CanvasGroup";
    const float groupAlpha=1.0F-std::clamp(numberField(vm,props,"GroupTransparency"),0.0F,1.0F);
    const bool canvasAvailable=budget.canvasResolver && *budget.canvasResolver;
    // Reserve a fail-closed readiness publication before descendants spend the
    // event budget. A failed render must suppress events already queued inside.
    const bool canvasCanPublish=canvasGroup && !lua_getreadonly(vm,instance) && budget.allocationBytes>=512;
    if (canvasCanPublish) budget.allocationBytes-=512;
    if (type == "Folder" || type == "PlayerGui") {
        drawChildren(vm, instance, depth+1, budget, parent, canvas, children,parentScale);
        lua_pop(vm, 1); return;
    }
    const bool styled = canvas || placed || type == "ScreenGui" || children.list || children.corner || children.padding ||
        hasField(vm, props, "Size") || hasField(vm, props, "Position") || hasField(vm, props, "AnchorPoint") ||
        hasField(vm, props, "BackgroundColor3") || hasField(vm, props, "BackgroundTransparency") || hasField(vm, props, "BorderSizePixel") ||
        hasField(vm, props, "BorderColor3") || hasField(vm, props, "ClipsDescendants") ||
        hasField(vm, props, "TextSize") || hasField(vm, props, "TextColor3") || hasField(vm, props, "TextTransparency") ||
        hasField(vm, props, "TextXAlignment") || hasField(vm, props, "TextYAlignment") || hasField(vm, props, "TextWrapped") ||
        hasField(vm, props, "AutomaticSize") || children.strokeIndex != 0 || children.gradientIndex != 0 || children.scale != 1.0F ||
        children.sizeConstraint || children.aspect>0 || type=="ImageLabel" || type=="ImageButton" || type=="TextBox" || type=="ScrollingFrame" || canvasGroup;
    ImGui::PushID(static_cast<int>(numberField(vm, instance, "id")));
    const bool gated=!booleanField(vm,props,"Interactable",true) ||
        (canvasGroup && (groupAlpha<=0 || !canvasAvailable || !booleanField(vm,instance,"_canvasReady",true) || !canvasCanPublish));
    // Interaction ancestry is independent of the group's visual opacity.
    if (gated) ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1.0F);
    ImGui::BeginDisabled(booleanField(vm, props, "Disabled") || gated ||
        ((type == "TextButton" || type=="ImageButton") && !booleanField(vm, props, "Active", true)));
    bool childRegion = false, group = false, renderChildren = true, clip = false, restoreCursor = false;
    Rect childRect{ ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail() };
    Rect canvasRect;
    float canvasRadius=0;
    Gradient canvasGradient;
    std::unique_ptr<CanvasCapture> capture;
    int compositeReserve=0;
    ImVec2 after;
    const bool colorPushed = hasField(vm, props, "TextColor") || hasField(vm, props, "TextColor3");
    if (colorPushed) {
        const auto color = colorField(vm, props, "TextColor", ImGui::GetStyle().Colors[ImGuiCol_Text]);
        ImGui::PushStyleColor(ImGuiCol_Text, colorField(vm, props, "TextColor3", color));
    }
    if (styled && type != "SameLine") {
        Rect rect{ canvas ? parent.origin : ImGui::GetCursorScreenPos(), nodeSize(vm, instance, props, type, parent.size, budget, depth,parentScale,&children) };
        ImVec2 position;
        const bool explicitPosition = dimensions(vm, props, "Position", parent.size, position,parentScale);
        if (explicitPosition || hasField(vm, props, "AnchorPoint")) {
            const auto anchor = vectorField(vm, props, "AnchorPoint");
            rect.origin = { parent.origin.x + position.x - anchor.x * rect.size.x, parent.origin.y + position.y - anchor.y * rect.size.y };
        }
        if (placed) {rect=*placed; rect.size=constrainedSize(rect.size,children,scale);}
        publishVector(vm, instance, props, "AbsolutePosition", rect.origin, budget);
        publishVector(vm, instance, props, "AbsoluteSize", rect.size, budget);
        ImGui::SetCursorScreenPos(rect.origin);
        const float radius = children.corner ? std::clamp(children.radius.resolve(std::min(rect.size.x, rect.size.y),scale), 0.0F, std::min(rect.size.x, rect.size.y) * 0.5F) : 0.0F;
        if (canvasGroup) {
            canvasRect=rect; canvasRadius=radius; canvasGradient=readGradient(vm,instance,children.gradientIndex);
            compositeReserve=children.gradientIndex ? 2304 : radius>0 ? 128 : 4;
            if (budget.canvasDepth<8 && budget.canvasGroups>0 && budget.meshVertices>=compositeReserve && rect.size.x>0 && rect.size.y>0) {
                --budget.canvasGroups; ++budget.canvasDepth; budget.meshVertices-=compositeReserve;
                capture=std::make_unique<CanvasCapture>(rect);
            } else compositeReserve=0;
        }
        const bool button = type == "TextButton" || type=="ImageButton";
        bool hovered = false, active = false;
        if (button && rect.size.x > 0.0F && rect.size.y > 0.0F) {
            if (ImGui::InvisibleButton("button", rect.size, ImGuiButtonFlags_EnableNav)) {
                enqueue(vm, instance, "Activated"); enqueue(vm, instance, "MouseButton1Click");
            }
            hovered = ImGui::IsItemHovered(); active = ImGui::IsItemActive();
        } else if (type != "Checkbox" && type != "SliderFloat") ImGui::Dummy(rect.size);
        hoverEvents(vm,instance,rect,button,budget);
        const auto gradient=readGradient(vm,instance,children.gradientIndex);
        const auto* tint=children.gradientIndex && !canvasGroup ? &gradient : nullptr;
        if ((!canvasGroup || capture) && (type != "ScreenGui" || hasField(vm, props, "BackgroundColor3")))
            background(vm, props, rect, canvasGroup ? 0 : radius, type, budget, tint, hovered, active,scale);
        if (type=="ImageLabel" || type=="ImageButton") imageNode(vm,instance,props,rect,radius,hovered,active,tint,budget,scale);
        if (type=="TextBox") {
            const float left=std::max(0.0F,children.left.resolve(rect.size.x,scale)),right=std::max(0.0F,children.right.resolve(rect.size.x,scale));
            const float top=std::max(0.0F,children.top.resolve(rect.size.y,scale)),bottom=std::max(0.0F,children.bottom.resolve(rect.size.y,scale));
            textBox(vm,instance,props,{{rect.origin.x+left,rect.origin.y+top},{std::max(1.0F,rect.size.x-left-right),std::max(1.0F,rect.size.y-top-bottom)}},tint,budget,scale);
        }
        if (children.strokeIndex && (!canvasGroup || capture)) {
            rawField(vm, instance, "children"); lua_rawgeti(vm, -1, children.strokeIndex); rawField(vm, -1, "props");
            const int stroke = lua_gettop(vm);
            auto color = colorField(vm, stroke, "Color", ImVec4(0,0,0,1));
            color.w *= (1.0F-std::clamp(numberField(vm, stroke, "Transparency"), 0.0F, 1.0F)) * ImGui::GetStyle().Alpha;
            const float thickness = std::clamp(numberField(vm, stroke, "Thickness", 1.0F)*scale, 0.0F, 128.0F);
            if (thickness > 0.0F) ImGui::GetWindowDrawList()->AddRect(rect.origin, {rect.origin.x+rect.size.x, rect.origin.y+rect.size.y},
                ImGui::ColorConvertFloat4ToU32(color), canvasGroup ? 0 : radius, 0, thickness);
            lua_pop(vm, 3);
        }
        if (type == "TextLabel" || type=="TextButton") {
            label(vm, props, rect, text, type=="TextButton",tint,budget,scale);
            const auto bounds = ImGui::GetFont()->CalcTextSizeA(std::clamp(textSize(vm, props)*scale,1.0F,512.0F), FLT_MAX,
                booleanField(vm, props, "TextWrapped") ? std::max(1.0F, rect.size.x) : 0.0F, text.c_str());
            publishVector(vm, instance, props, "TextBounds", bounds, budget);
        }
        if (type == "Checkbox") {
            bool checked = booleanField(vm, props, "Checked");
            if (ImGui::Checkbox(text.c_str(), &checked)) changedBoolean(vm, instance, checked);
        } else if (type == "SliderFloat") {
            float value = numberField(vm, props, "Value");
            ImGui::SetNextItemWidth(std::max(1.0F, rect.size.x));
            const float minimum = numberField(vm, props, "Min");
            if (ImGui::SliderFloat(text.c_str(), &value, minimum, std::max(minimum, numberField(vm, props, "Max", 1.0F)))) changedNumber(vm, instance, value);
        } else if (type == "Separator") {
            ImGui::GetWindowDrawList()->AddLine(rect.origin, { rect.origin.x + rect.size.x, rect.origin.y }, ImGui::GetColorU32(ImGuiCol_Separator));
        }
        after = ImGui::GetCursorScreenPos(); restoreCursor = true;
        childRect = rect;
        const float left = std::max(0.0F, children.left.resolve(rect.size.x,scale)), right = std::max(0.0F, children.right.resolve(rect.size.x,scale));
        const float top = std::max(0.0F, children.top.resolve(rect.size.y,scale)), bottom = std::max(0.0F, children.bottom.resolve(rect.size.y,scale));
        childRect.origin = { rect.origin.x + left, rect.origin.y + top };
        childRect.size = { std::max(0.0F, rect.size.x - left - right), std::max(0.0F, rect.size.y - top - bottom) };
        if (type=="ScrollingFrame") {
            childRect=scrolling(vm,instance,props,childRect,children,budget,depth,scale); clip=true;
        } else if (!canvasGroup && booleanField(vm, props, "ClipsDescendants")) {
            ImGui::PushClipRect(rect.origin, { rect.origin.x + rect.size.x, rect.origin.y + rect.size.y }, true);
            clip = true;
        }
        renderChildren = rect.size.x > 0.0F && rect.size.y > 0.0F && (!canvasGroup || capture);
    } else {
        const auto origin = ImGui::GetCursorScreenPos();
        const float width = std::max(0.0F, numberField(vm, props, "Width")), height = std::max(0.0F, numberField(vm, props, "Height"));
        if (type == "TextLabel") ImGui::TextUnformatted(text.c_str());
        else if (type == "TextButton") {
            if (ImGui::Button(text.c_str(), ImVec2(width, height))) { enqueue(vm, instance, "Activated"); enqueue(vm, instance, "MouseButton1Click"); }
            hoverEvents(vm,instance,{ImGui::GetItemRectMin(),ImGui::GetItemRectSize()},true,budget);
        } else if (type == "Checkbox") {
            bool checked = booleanField(vm, props, "Checked");
            if (ImGui::Checkbox(text.c_str(), &checked)) changedBoolean(vm, instance, checked);
        } else if (type == "SliderFloat") {
            float value = numberField(vm, props, "Value");
            const float minimum = numberField(vm, props, "Min");
            if (width > 0.0F) ImGui::SetNextItemWidth(width);
            if (ImGui::SliderFloat(text.c_str(), &value, minimum, std::max(minimum, numberField(vm, props, "Max", 1.0F)))) changedNumber(vm, instance, value);
        } else if (type == "Separator") ImGui::Separator();
        else if (type == "SameLine") ImGui::SameLine();
        else if (type == "Frame" || type == "Window") {
            if (width > 0.0F || height > 0.0F) { renderChildren = ImGui::BeginChild("region", ImVec2(width, height)); childRegion = true; }
            else { ImGui::BeginGroup(); group = true; }
            childRect = { ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail() };
        }
        if (type != "SameLine") {
            publishVector(vm, instance, props, "AbsolutePosition", origin, budget);
            publishVector(vm, instance, props, "AbsoluteSize", type == "Frame" || type == "Window" ? childRect.size : ImGui::GetItemRectSize(), budget);
            if (type == "TextLabel" || type == "TextButton") publishVector(vm, instance, props, "TextBounds", ImGui::CalcTextSize(text.c_str()), budget);
        }
    }
    if (renderChildren) {
        if (styled) ImGui::SetCursorScreenPos(childRect.origin);
        // ScreenGui historically hosted legacy flow widgets too. Only an
        // explicitly laid out descendant switches those children to a canvas.
        drawChildren(vm, instance, depth + 1, budget, childRect, styled && type != "ScreenGui", children,scale);
    }
    if (clip) ImGui::PopClipRect();
    if (canvasGroup) {
        ReactUI::Image image;
        if (capture) {
            ImDrawList output(ImGui::GetDrawListSharedData());
            const bool valid=capture->finish(output,budget);
            --budget.canvasDepth; budget.meshVertices+=compositeReserve;
            if (valid && groupAlpha>0 && canvasAvailable && canvasRect.size.x<=2048 && canvasRect.size.y<=2048) {
                try {image=(*budget.canvasResolver)(lua_topointer(vm,instance),output,canvasRect.origin.x,canvasRect.origin.y,canvasRect.size.x,canvasRect.size.y);}
                catch (...) {image={};}
            }
        }
        const bool ready=image.textureId && std::isfinite(image.width) && std::isfinite(image.height) && image.width>0 && image.height>0;
        if (booleanField(vm,instance,"_canvasReady",!ready)!=ready && canvasCanPublish) {
            lua_pushstring(vm,"_canvasReady"); lua_pushboolean(vm,ready); lua_rawset(vm,instance);
        }
        if (ready) {
            auto color=colorField(vm,props,"GroupColor3",ImVec4(1,1,1,1)); color.w=groupAlpha;
            surface(canvasRect,canvasRect,canvasRadius,{0,0},{1,1},color,children.gradientIndex ? &canvasGradient : nullptr,budget,&image);
        } else if (groupAlpha>0 && canvasRect.size.x>0 && canvasRect.size.y>0 && budget.meshVertices>=128 && budget.textBytes>=32) {
            budget.meshVertices-=128; budget.textBytes-=32;
            auto* list=ImGui::GetWindowDrawList();
            list->PushClipRect(canvasRect.origin,{canvasRect.origin.x+canvasRect.size.x,canvasRect.origin.y+canvasRect.size.y},true);
            list->AddRect(canvasRect.origin,{canvasRect.origin.x+canvasRect.size.x,canvasRect.origin.y+canvasRect.size.y},IM_COL32(255,90,90,255));
            list->AddText({canvasRect.origin.x+4,canvasRect.origin.y+4},IM_COL32(255,160,160,255),"CanvasGroup unavailable");
            list->PopClipRect();
        }
    }
    if (restoreCursor) {
        // Register the restored boundary through a real item. Merely moving the
        // cursor past the last item's extent is an ImGui layout assertion.
        ImGui::SetCursorScreenPos({ after.x, after.y - ImGui::GetStyle().ItemSpacing.y });
        ImGui::Dummy(ImVec2(0.0F, 0.0F));
    }
    if (childRegion) ImGui::EndChild();
    if (group) ImGui::EndGroup();
    if (colorPushed) ImGui::PopStyleColor();
    ImGui::EndDisabled(); if (gated) ImGui::PopStyleVar(); ImGui::PopID(); lua_pop(vm, 1);
}
} // namespace

void ReactUI::draw(ScriptingSystem& scripting, const bool gameOnly, const ImageResolver& images, const CanvasResolver& canvases) {
    // Preserve allocator headroom for raw-field reads, the bounded Lua stack and
    // mouse event records. Lua callbacks only run later in the protected pump.
    const auto limit = scripting.limits().memoryBytes;
    if (scripting.memoryUsed() >= limit || limit - scripting.memoryUsed() < 64U * 1024U) return;
    lua_State* vm = scripting.state();
    if (!vm || !lua_checkstack(vm, 512)) return;
    const int originalTop = lua_gettop(vm);
    rawField(vm, LUA_GLOBALSINDEX, "__turboUIRoots");
    if (lua_istable(vm, -1)) {
        const int roots = lua_gettop(vm), count = lua_objlen(vm, roots);
        Budget budget;
        budget.imageResolver=&images;
        budget.canvasResolver=&canvases;
        // Geometry and event records are allocated on this unprotected native
        // path. Reserve most VM headroom and cap writes; unchanged nodes then
        // let large trees finish publishing geometry across subsequent frames.
        const size_t headroom = scripting.memoryUsed() < limit ? limit-scripting.memoryUsed() : 0;
        budget.allocationBytes=headroom>32768U ? headroom-32768U : 0;
        budget.valueWrites = headroom > 32768U ? static_cast<int>(std::min(size_t{128}, (headroom-32768U)/512U)) : 0;
        for (int index = 1; index <= count && budget.nodes > 0; ++index) {
            --budget.nodes;
            lua_rawgeti(vm, roots, index);
            if (lua_istable(vm, -1) && drawableRoot(vm, -1, 0, budget.rootInspections)) {
                const int container = lua_gettop(vm);
                rawField(vm, container, "props");
                const auto title = lua_istable(vm, -1) ? stringField(vm, -1, "Name", "Script UI") : stringField(vm, container, "Name", "Script UI");
                lua_pop(vm, 1);
                const auto name = title + "##ReactRoot" +
                    std::to_string(static_cast<int>(numberField(vm, container, "id", static_cast<float>(index))));
                ImGuiWindowFlags flags = ImGuiWindowFlags_None;
                if (gameOnly) {
                    const auto* viewport = ImGui::GetMainViewport();
                    ImGui::SetNextWindowPos(viewport->Pos); ImGui::SetNextWindowSize(viewport->Size);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
                    flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
                } else ImGui::SetNextWindowSize(ImVec2(440, 420), ImGuiCond_FirstUseEver);
                if (ImGui::Begin(name.c_str(), nullptr, flags)) {
                    const Rect rect{ ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail() };
                    const auto children = modifiers(vm, container, budget);
                    drawChildren(vm, container, 0, budget, rect, false, children);
                }
                ImGui::End();
                if (gameOnly) ImGui::PopStyleVar();
            }
            lua_pop(vm, 1);
        }
    }
    lua_settop(vm, originalTop);
}
} // namespace engine::scripting

