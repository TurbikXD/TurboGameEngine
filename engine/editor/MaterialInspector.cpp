#include "engine/editor/MaterialInspector.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <glm/gtc/type_ptr.hpp>
#include "engine/core/AtomicFile.h"
#include "engine/core/Log.h"
#include "engine/ecs/components.h"
#include "engine/editor/MaterialGraphEditor.h"
#include "engine/resources/material.h"
#include "engine/resources/material_graph.h"

namespace engine::editor {
namespace {
using Json = nlohmann::json;
constexpr char materialGraphWindowName[] = "Material Graph";
constexpr std::size_t maxMaterialSourceBytes = 256U * 1024U;
constexpr std::size_t maxDraftFileBytes = 32U * 1024U * 1024U;
constexpr std::size_t maxDrafts = 256;
constexpr std::size_t maxHistoryEdits = 32;
constexpr std::size_t maxHistoryBytes = 512U * 1024U;

struct MaterialEdit final {
    std::string source;
    MaterialGraphChange change;
};

struct MaterialHistory final {
    std::deque<MaterialEdit> undo, redo;
    std::uint64_t nextNodeId{1};
    ImGuiID activeItem{};
    bool layoutDrag{};
};

struct Draft final {
    Json source = Json::object();
    Json resolvedGraph;
    MaterialGraphView graphView;
    MaterialHistory history;
    std::string original, savedSource, shader, texture, base, destination, status, search, manualPath;
    std::vector<std::string> choices;
    std::weak_ptr<resources::Material> resource;
    glm::vec4 tint{1};
    glm::vec2 uvScale{1};
    int blend{};
    bool depthTest{true}, depthWrite{true}, dirty{}, loaded{}, creating{}, failed{}, persistenceDirty{};
    bool recoverHistory{}, recoveryTooLarge{};
    std::uint64_t revision{};
    std::uint64_t appearanceRevision{};
};

struct GraphPreview final {
    std::weak_ptr<resources::Material> source;
    std::shared_ptr<resources::Material> material;
    std::chrono::steady_clock::time_point nextUpdate{};
    std::string error;
    std::uint64_t appearanceRevision{};
    std::uint64_t assetRevision{};
};

struct GraphWindow final {
    std::filesystem::path root;
    std::string path;
    std::optional<MaterialGraphCanvasState> canvas;
    bool open{};
    bool focus{};
    bool restoreFocus{};
    bool placedInDock{};
    bool fitOnPlacementChange{};
    bool showProperties{};
    ImVec2 floatingPosition{};
    ImVec2 floatingSize{};
    ImGuiContext* focusedContext{};
    GraphPreview preview;
};

GraphWindow graphWindow;
std::map<std::string, Draft> drafts;

struct GraphDraftStore final {
    std::filesystem::path file;
    std::string keyPrefix;
    Json entries = Json::object();
    std::chrono::steady_clock::time_point nextWrite{};
    bool collectChanges{};
    bool needsWrite{};
    bool writeBlocked{};
    std::string writeError;
};

GraphDraftStore draftStore;

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string draftKey(const resources::MaterialLibrary& library, const std::string& path) {
    return utf8(library.root()) + "|" + path;
}

Draft* acquireDraft(const resources::MaterialLibrary& library, const std::string& key) {
    if (const auto found = drafts.find(key); found != drafts.end()) return &found->second;
    if (drafts.size() >= maxDrafts) {
        const auto active = graphWindow.open ? draftKey(library, graphWindow.path) : std::string{};
        for (auto it = drafts.begin(); it != drafts.end();) {
            if (!it->second.dirty && !it->second.persistenceDirty && it->first != active) it = drafts.erase(it); else ++it;
        }
    }
    return drafts.size() < maxDrafts ? &drafts.try_emplace(key).first->second : nullptr;
}

void openGraph(const resources::MaterialLibrary& library, const std::string& path) {
    if (graphWindow.root != library.root()) {
        graphWindow = {};
        graphWindow.root = library.root();
    }
    if (graphWindow.path != path) {
        graphWindow.path = path;
        graphWindow.canvas.reset();
        graphWindow.preview = {};
    }
    graphWindow.open = graphWindow.focus = true;
}

std::optional<MaterialGraphCanvasState> validatedCanvas(MaterialGraphCanvasState canvas) {
    constexpr float maxPan = 2'000'000.0F;
    if (!(std::abs(canvas.pan.x) <= maxPan && std::abs(canvas.pan.y) <= maxPan) ||
        !std::isfinite(canvas.zoom) || canvas.zoom <= 0.0F) return std::nullopt;
    canvas.zoom = std::clamp(canvas.zoom, minMaterialGraphZoom, maxMaterialGraphZoom);
    return canvas;
}

const Json* localGraph(const Draft& draft) {
    const auto graph = draft.source.find("graph");
    return graph != draft.source.end() && graph->is_object() ? &*graph : nullptr;
}

bool hasGraph(const Draft& draft) { return localGraph(draft) || draft.resolvedGraph.is_object(); }

bool hasHistory(const MaterialHistory& history) { return !history.undo.empty() || !history.redo.empty(); }

bool hasRecovery(const Draft& draft) {
    return (draft.dirty && hasGraph(draft)) || (draft.recoverHistory && hasHistory(draft.history));
}

void markDraftPersistence(Draft& draft) {
    if (draftStore.file.empty() || draftStore.writeBlocked) return;
    draft.persistenceDirty = true;
    draftStore.collectChanges = true;
}

void rememberNodeIds(Draft& draft) {
    if (const auto* graph = localGraph(draft)) {
        draft.history.nextNodeId = std::max(draft.history.nextNodeId, nextMaterialGraphNodeId(*graph));
    }
}

void trimHistory(MaterialHistory& history) {
    std::size_t bytes = 0;
    for (const auto& entry : history.undo) bytes += entry.source.size();
    for (const auto& entry : history.redo) bytes += entry.source.size();
    while (history.undo.size() + history.redo.size() > maxHistoryEdits || bytes > maxHistoryBytes) {
        auto& stack = history.undo.empty() ? history.redo : history.undo;
        bytes -= stack.front().source.size();
        stack.pop_front();
    }
}

void pushMaterialEdit(Draft& draft, MaterialEdit edit) {
    auto& history = draft.history;
    history.layoutDrag = edit.change == MaterialGraphChange::Layout;
    history.undo.push_back(std::move(edit));
    history.redo.clear();
    history.activeItem = 0;
    draft.recoverHistory = true;
    trimHistory(history);
}

void recordMaterialEdit(Draft& draft, MaterialGraphChange change = MaterialGraphChange::Appearance, bool conversion = false) {
    auto& history = draft.history;
    if (!conversion && !hasGraph(draft) && !hasHistory(history)) return;
    if (change == MaterialGraphChange::Layout && history.layoutDrag) return;
    rememberNodeIds(draft);
    pushMaterialEdit(draft, {draft.source.dump(), change});
}

void recordMaterialItemEdit(Draft& draft, bool changed) {
    auto& activeItem = draft.history.activeItem;
    const auto item = ImGui::GetItemID();
    if (activeItem == item && ImGui::IsItemActivated()) activeItem = 0;
    if (changed && (item == 0 || activeItem != item)) {
        recordMaterialEdit(draft);
        if (ImGui::IsItemActive()) activeItem = item;
    }
    if (activeItem == item && !ImGui::IsItemActive()) activeItem = 0;
}

void markMaterialChanged(Draft& draft) {
    ++draft.appearanceRevision;
    draft.dirty = true;
    draft.status.clear();
    draft.failed = false;
    markDraftPersistence(draft);
}

Json starterGraph(const std::string& texture) {
    const auto node = texture.empty() ? Json{{"id", 1}, {"type", "color"}, {"position", {40, 90}}} :
        Json{{"id", 1}, {"type", "texture"}, {"texture", texture}, {"position", {40, 90}}};
    return {{"nodes", Json::array({node})}, {"outputs", {{"baseColor", 1}}}, {"outputPosition", {340, 45}}};
}

void useLocalGraph(Draft& draft, Json graph) {
    draft.source["version"] = 2;
    draft.source["graph"] = std::move(graph);
    draft.source.erase("shader");
    draft.source.erase("texture");
    draft.source.erase("graphTextures");
    draft.graphView = {};
    markMaterialChanged(draft);
}
std::string lowercase(std::string text) {
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}
std::string suggestedPath(const resources::MaterialLibrary& library, const std::string& source, const bool instance) {
    const auto path = std::filesystem::u8path(source.empty() ? "materials/new.material.json" : source);
    const auto name = utf8(path.filename());
    const auto suffix = name.ends_with(".material.json") ? std::string{".material.json"} : std::string{".json"};
    const auto stem = name.substr(0, name.size() - std::min(name.size(), suffix.size())) + (instance ? "_instance" : "");
    for (int index = 0; index < 10000; ++index) {
        const auto result = utf8(path.parent_path() / std::filesystem::u8path(stem + (index == 0 ? "" : "_" + std::to_string(index)) + suffix));
        std::error_code error;
        if (!std::filesystem::exists(library.root() / std::filesystem::u8path(result), error) && !error) return result;
    }
    return {};
}
std::string readSource(const resources::MaterialLibrary& library, const std::string& id) {
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(library.root() / std::filesystem::u8path(id), error);
    if (error) return {};
    const auto relative = path.lexically_relative(library.root());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string text(maxMaterialSourceBytes + 1U, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (input.bad() || input.gcount() > static_cast<std::streamsize>(maxMaterialSourceBytes)) return {};
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}
void showDefinition(Draft& draft, const resources::MaterialDefinition& definition) {
    draft.shader = definition.shader;
    draft.texture = definition.texture;
    draft.tint = definition.tint;
    draft.uvScale = definition.uvScale;
    draft.blend = static_cast<int>(definition.blend);
    draft.depthTest = definition.depthTest;
    draft.depthWrite = definition.depthWrite;
    draft.resolvedGraph = definition.graph;
}
void refresh(Draft& draft, const resources::Material& material, const resources::MaterialLibrary& library) {
    const auto view = draft.graphView;
    draft = {};
    draft.graphView = view;
    draft.graphView.dragNode = draft.graphView.linkSource = 0;
    draft.graphView.dragOutput = draft.graphView.linkPressed = false;
    draft.original = readSource(library, material.path);
    const auto parsed = nlohmann::json::parse(draft.original, nullptr, false);
    if (parsed.is_object()) draft.source = parsed;
    draft.savedSource = draft.source.dump();
    showDefinition(draft, material.definition);
    if (draft.source.contains("base") && draft.source["base"].is_string()) draft.base = draft.source["base"].get<std::string>();
    draft.revision = material.revision;
    draft.loaded = true;
}

void resolveDraft(Draft& draft, const resources::MaterialLibrary& library, const std::string& path) {
    resources::MaterialDefinition definition;
    draft.failed = !library.preview(path, draft.source, definition, &draft.status);
    if (!draft.failed) { showDefinition(draft, definition); draft.status.clear(); }
}

Json parseRecoveryJson(std::string_view bytes) {
    bool tooDeep = false;
    auto result = Json::parse(bytes, [&](int depth, Json::parse_event_t, Json&) {
        tooDeep = tooDeep || depth > 64;
        return !tooDeep;
    }, false);
    return tooDeep ? Json(Json::value_t::discarded) : std::move(result);
}

Json materialSourceWithNodeCursor(Json source, std::uint64_t nextNodeId) {
    if (const auto graph = source.find("graph"); graph != source.end() && graph->is_object() &&
        nextNodeId > nextMaterialGraphNodeId(*graph)) {
        (*graph)["nextNodeId"] = nextNodeId;
    }
    return source;
}

Json writeMaterialHistory(const MaterialHistory& history) {
    const auto stackToJson = [](const std::deque<MaterialEdit>& stack) {
        auto entries = Json::array();
        for (const auto& edit : stack) {
            entries.push_back({{"source", edit.source},
                {"change", edit.change == MaterialGraphChange::Layout ? "layout" : "appearance"}});
        }
        return entries;
    };
    return {{"undo", stackToJson(history.undo)}, {"redo", stackToJson(history.redo)}, {"nextNodeId", history.nextNodeId}};
}

std::optional<MaterialHistory> readMaterialHistory(const Json& entry) {
    const auto saved = entry.find("history");
    if (saved == entry.end() || !saved->is_object()) return std::nullopt;
    const auto undo = saved->find("undo");
    const auto redo = saved->find("redo");
    const auto next = saved->find("nextNodeId");
    if (undo == saved->end() || !undo->is_array() || redo == saved->end() || !redo->is_array() ||
        undo->size() + redo->size() > maxHistoryEdits || next == saved->end() || !next->is_number_integer()) {
        return std::nullopt;
    }

    MaterialHistory history;
    history.nextNodeId = next->get<std::uint64_t>();
    constexpr auto exhaustedNodeId = std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1;
    if (history.nextNodeId == 0 || history.nextNodeId > exhaustedNodeId) return std::nullopt;

    std::size_t remainingBytes = maxHistoryBytes;
    const auto readStack = [&](const Json& entries, std::deque<MaterialEdit>& stack) {
        for (const auto& edit : entries) {
            const auto snapshot = edit.find("source");
            const auto kind = edit.find("change");
            if (snapshot == edit.end() || !snapshot->is_string() || kind == edit.end() ||
                (*kind != "layout" && *kind != "appearance")) return false;
            const auto& bytes = snapshot->get_ref<const std::string&>();
            if (bytes.size() > remainingBytes) return false;
            const auto source = parseRecoveryJson(bytes);
            if (!source.is_object()) return false;
            auto canonical = source.dump();
            const auto size = std::max(bytes.size(), canonical.size());
            if (size > remainingBytes) return false;
            remainingBytes -= size;
            if (const auto graph = source.find("graph"); graph != source.end() && graph->is_object()) {
                history.nextNodeId = std::max(history.nextNodeId, nextMaterialGraphNodeId(*graph));
            }
            stack.push_back({std::move(canonical), *kind == "layout" ? MaterialGraphChange::Layout : MaterialGraphChange::Appearance});
        }
        return true;
    };
    if (!readStack(*undo, history.undo) || !readStack(*redo, history.redo)) return std::nullopt;
    return history;
}

void recoverDraft(Draft& draft, const resources::Material& material, const resources::MaterialLibrary& library) {
    if (!material.ready || !material.lastError.empty() || draftStore.file.empty() ||
        draftStore.keyPrefix != draftKey(library, {})) return;
    const auto saved = draftStore.entries.find(material.path);
    if (saved == draftStore.entries.end()) return;
    const auto& source = saved->at("source");
    const auto& original = saved->at("original").get_ref<const std::string&>();
    const bool conflict = original != draft.original;
    const bool matchesDisk = source == draft.source;
    auto history = readMaterialHistory(*saved);
    const auto originalSource = parseRecoveryJson(original);
    const auto cleanHistory = saved->find("historyOnly");
    const bool historyOnly = cleanHistory != saved->end() && *cleanHistory == true;
    const bool stale = conflict && !historyOnly && (matchesDisk ||
        (history && materialSourceWithNodeCursor(source, history->nextNodeId) == draft.source));
    if (stale || ((matchesDisk || historyOnly) && (!history || !hasHistory(*history)))) {
        draftStore.entries.erase(saved);
        draftStore.needsWrite = true;
        return;
    }
    if (history) draft.history = std::move(*history);
    draft.recoverHistory = hasHistory(draft.history);
    if (matchesDisk || historyOnly) {
        if (conflict) {
            draft.original = original;
            draft.failed = true;
            draft.status = "The material changed on disk. The recovered history is still available.";
        }
        rememberNodeIds(draft);
        return;
    }
    draft.original = original;
    draft.savedSource = originalSource.dump();
    draft.source = source;
    draft.base.clear();
    if (const auto base = draft.source.find("base"); base != draft.source.end() && base->is_string()) {
        draft.base = base->get<std::string>();
    }
    draft.dirty = true;
    ++draft.appearanceRevision;
    rememberNodeIds(draft);
    resolveDraft(draft, library, material.path);
    if (conflict) {
        draft.failed = true;
        draft.status = "The material changed on disk. The recovered draft is still unsaved.";
    }
}

void refreshIfNeeded(Draft& draft, const std::shared_ptr<resources::Material>& material, const resources::MaterialLibrary& library) {
    if (draft.loaded && draft.dirty) {
        draft.resource = material;
        return;
    }
    if (!draft.loaded || draft.resource.lock() != material || draft.revision != material->revision) {
        if (draft.loaded && (draft.resource.lock() == material || draft.recoverHistory) &&
            readSource(library, material->path) == draft.original) {
            showDefinition(draft, material->definition);
            draft.revision = material->revision;
            draft.resource = material;
            return;
        }
        refresh(draft, *material, library);
        draft.resource = material;
        recoverDraft(draft, *material, library);
    }
}

void saveDraft(Draft& draft, const std::shared_ptr<resources::Material>& material, resources::MaterialLibrary& library) {
    draft.failed = true;
    if (readSource(library, material->path) != draft.original) {
        draft.status = "The file changed on disk. Reload it before saving.";
        return;
    }
    if (!library.save(material->path, materialSourceWithNodeCursor(draft.source, draft.history.nextNodeId), &draft.status)) return;
    auto history = std::move(draft.history);
    history.activeItem = 0;
    history.layoutDrag = false;
    refresh(draft, *material, library);
    draft.history = std::move(history);
    draft.resource = material;
    draft.status = "Material saved. Objects update on the next frame.";
    markDraftPersistence(draft);
    flushMaterialGraphDrafts(true);
}

void reloadDraft(Draft& draft, const std::shared_ptr<resources::Material>& material, resources::MaterialLibrary& library) {
    library.poll(true);
    refresh(draft, *material, library);
    draft.resource = material;
    draft.graphView.fit = true;
    markDraftPersistence(draft);
    flushMaterialGraphDrafts(true);
}

void showStatus(const Draft& draft) {
    if (draft.status.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, draft.failed ? ImVec4{1, .5F, .45F, 1} : ImVec4{.45F, .8F, .6F, 1});
    ImGui::TextWrapped("%s", draft.status.c_str());
    ImGui::PopStyleColor();
}

void restoreMaterialEdit(Draft& draft, bool redo, const resources::Material& material, const resources::MaterialLibrary& library) {
    auto& history = draft.history;
    auto& from = redo ? history.redo : history.undo;
    auto& to = redo ? history.undo : history.redo;
    if (from.empty()) return;
    rememberNodeIds(draft);
    auto current = draft.source.dump();
    draft.recoverHistory = true;
    markDraftPersistence(draft);
    while (!from.empty() && from.back().source == current) from.pop_back();
    if (from.empty()) return;
    auto edit = std::move(from.back());
    from.pop_back();
    to.push_back({std::move(current), edit.change});
    trimHistory(history);
    history.activeItem = 0;
    history.layoutDrag = false;
    draft.source = Json::parse(edit.source);
    draft.base.clear();
    if (const auto base = draft.source.find("base"); base != draft.source.end() && base->is_string()) {
        draft.base = base->get<std::string>();
    }
    draft.dirty = edit.source != draft.savedSource;
    draft.status.clear();
    draft.failed = false;
    if (!draft.dirty) {
        draft.appearanceRevision = 0;
        graphWindow.preview = {};
        showDefinition(draft, material.definition);
    } else if (edit.change == MaterialGraphChange::Appearance) {
        ++draft.appearanceRevision;
        graphWindow.preview.nextUpdate = {};
        resolveDraft(draft, library, material.path);
    }
    auto& view = draft.graphView;
    view.dragNode = view.linkSource = 0;
    view.dragOutput = view.linkPressed = false;
    auto& graph = localGraph(draft) ? draft.source["graph"] : draft.resolvedGraph;
    if (!findMaterialGraphNode(graph, view.selected)) view.selected = 0;
}

void changed(Draft& draft, const char* key, nlohmann::json value, bool grouped = false) {
    if (!grouped) recordMaterialEdit(draft);
    draft.source[key] = std::move(value);
    markMaterialChanged(draft);
}
bool hasBase(const Draft& draft) {
    const auto found = draft.source.find("base");
    return found != draft.source.end() && found->is_string() && !found->get_ref<const std::string&>().empty();
}
void hint(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}
bool primaryButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{.16F, .34F, .51F, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{.20F, .43F, .63F, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{.12F, .28F, .43F, 1});
    const bool clicked = ImGui::Button(label, {-1, 0});
    ImGui::PopStyleColor(3);
    return clicked;
}
bool property(Draft& draft, const resources::MaterialLibrary& library, const std::string& path,
              const char* key, const char* label, nlohmann::json value) {
    const bool instance = hasBase(draft);
    bool overridden = draft.source.contains(key);
    if (instance) {
        const auto id = std::string(label) + "###Override_" + key;
        if (ImGui::Checkbox(id.c_str(), &overridden)) {
            recordMaterialEdit(draft);
            if (overridden) draft.source[key] = std::move(value); else draft.source.erase(key);
            markMaterialChanged(draft);
            resolveDraft(draft, library, path);
        }
        hint("Check to override this parameter. Uncheck to inherit it from the base material.");
    } else ImGui::TextUnformatted(label);
    if (instance && !overridden) { ImGui::SameLine(); ImGui::TextDisabled("inherited"); }
    ImGui::SetNextItemWidth(-1);
    return !instance || overridden;
}
void gatherChoices(Draft& draft, const resources::MaterialLibrary& library, const char* directory, const bool textures) {
    draft.choices.clear(); draft.search.clear();
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(library.root() / directory,
        std::filesystem::directory_options::skip_permission_denied, error), end;
    for (std::size_t visited = 0; !error && iterator != end && visited < 4096 && draft.choices.size() < 256;
         iterator.increment(error), ++visited) {
        if (iterator.depth() >= 16) iterator.disable_recursion_pending();
        if (!iterator->is_regular_file(error)) continue;
        const auto extension = lowercase(utf8(iterator->path().extension()));
        const bool accepted = textures ? (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
            extension == ".bmp" || extension == ".tga" || extension == ".ppm" || extension == ".pgm" || extension == ".hdr") :
            extension == ".json";
        if (accepted) draft.choices.push_back(utf8(iterator->path().lexically_relative(library.root())));
    }
    std::sort(draft.choices.begin(), draft.choices.end());
}
bool assetChoice(const char* id, const std::string& current, const char* emptyLabel, Draft& draft,
                 const resources::MaterialLibrary& library, const char* directory, const bool textures,
                 std::string& selected) {
    const auto display = current.empty() ? std::string(emptyLabel) : utf8(std::filesystem::u8path(current).filename());
    bool chosen = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(id, display.c_str(), ImGuiComboFlags_HeightLarge)) {
        const bool appearing = ImGui::IsWindowAppearing();
        if (appearing) { gatherChoices(draft, library, directory, textures); draft.manualPath = current; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("###AssetSearch", "Search by name or path...", &draft.search);
        if (ImGui::CollapsingHeader("Enter a Path###CustomAssetPath")) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("###ManualAssetPath", &draft.manualPath);
            if (ImGui::Button("Select File###UseAssetPath")) {
                selected = draft.manualPath; chosen = true; ImGui::CloseCurrentPopup();
            }
        }
        const auto query = lowercase(draft.search);
        if (query.empty() && ImGui::Selectable(emptyLabel, current.empty())) { selected.clear(); chosen = true; }
        int visible = 0;
        for (const auto& path : draft.choices) {
            if (!query.empty() && lowercase(path).find(query) == std::string::npos) continue;
            ++visible;
            const auto label = utf8(std::filesystem::u8path(path).filename()) + "###" + path;
            if (ImGui::Selectable(label.c_str(), current == path)) { selected = path; chosen = true; }
            hint(path.c_str());
        }
        if (!visible) ImGui::TextDisabled("No matching files");
        ImGui::EndCombo();
    }
    hint(current.empty() ? "Select a project asset from the list." : current.c_str());
    return chosen;
}
bool createAsset(Draft& draft, ecs::MeshRenderer& mesh, resources::MaterialLibrary& library, const std::string& base, bool graph = false) {
    if (draft.destination.empty() || draft.destination.size() > 4096 || draft.destination.find('\0') != std::string::npos) {
        draft.failed = true; draft.status = "Enter a valid path for a new file inside the project."; return false;
    }
    std::error_code error;
    auto destination = std::filesystem::u8path(draft.destination);
    if (!destination.is_absolute() && *destination.begin() == "assets") destination = destination.lexically_relative("assets");
    const auto path = std::filesystem::weakly_canonical(library.root() / destination, error);
    const auto relative = path.lexically_relative(library.root());
    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == ".." || relative == ".") {
        draft.failed = true; draft.status = "The new material must be inside the project's assets folder."; return false;
    }
    if (std::filesystem::exists(path, error) || error) {
        draft.failed = true; draft.status = "This file already exists or cannot be accessed. Choose another name."; return false;
    }
    Json data{{"version", graph ? 2 : 1}};
    if (!base.empty()) data["base"] = base;
    else if (graph) data["graph"] = starterGraph({});
    else {
        data["shader"] = "shaders_hlsl/textured.shader.json";
        data["tint"] = {1, 1, 1, 1};
    }
    const auto id = utf8(relative);
    if (!library.save(id, data, &draft.status)) { draft.failed = true; return false; }
    mesh.materialId = library.load(id)->path;
    mesh.material.reset();
    draft.creating = false;
    if (graph) openGraph(library, mesh.materialId);
    return true;
}
}

void renderMaterialInspector(ecs::MeshRenderer& mesh, resources::MaterialLibrary& library) {
    if (!ImGui::CollapsingHeader("Material###MaterialAsset", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const auto material = mesh.materialId.empty() ? nullptr : library.load(mesh.materialId);
    const auto key = draftKey(library, material ? material->path : mesh.materialId);
    auto* entry = acquireDraft(library, key);
    if (!entry) { ImGui::TextWrapped("Save material drafts before opening more assets."); return; }
    auto& draft = *entry;
    ImGui::PushID(key.c_str());
    struct ContentPadding final {
        ContentPadding() { ImGui::Indent(10); }
        ~ContentPadding() { ImGui::Unindent(10); }
    } padding;
    ImGui::TextDisabled("OBJECT MATERIAL");
    std::string selected;
    if (assetChoice("###MaterialChoice", mesh.materialId, "No Material", draft, library, "materials", false, selected)) {
        const auto candidate = selected.empty() ? nullptr : library.load(selected);
        if (candidate && !candidate->ready) { draft.status = candidate->lastError; draft.failed = true; }
        else { mesh.materialId = candidate ? candidate->path : std::string{}; mesh.material.reset(); ImGui::PopID(); return; }
    }
    if (mesh.materialId.empty()) {
        ImGui::Spacing(); ImGui::TextWrapped("Select an existing material above or create one for this object.");
        if (draft.destination.empty()) draft.destination = suggestedPath(library, {}, false);
        ImGui::Spacing(); ImGui::TextUnformatted("New Material File"); ImGui::SetNextItemWidth(-1);
        ImGui::InputText("###Destination", &draft.destination);
        if (primaryButton("Create Material Graph###CreateGraphMaterial")) createAsset(draft, mesh, library, {}, true);
        if (ImGui::Button("Create Legacy Material###CreateMaterial", {-1, 0})) createAsset(draft, mesh, library, {});
        if (!draft.status.empty()) ImGui::TextWrapped("%s", draft.status.c_str());
        ImGui::PopID();
        return;
    }
    refreshIfNeeded(draft, material, library);
    ImGui::TextWrapped("%s", material->path.c_str());
    const bool resourceError = !material->ready || !material->lastError.empty() || !material->runtimeError.empty();
    ImGui::TextColored(resourceError ? ImVec4{1, .5F, .45F, 1} : draft.dirty ? ImVec4{1, .72F, .3F, 1} : ImVec4{.45F, .8F, .6F, 1},
        "%s", !material->ready ? "Material could not be loaded" : draft.dirty ? "Unsaved material changes" :
            resourceError ? "Last Valid Version" : "Saved");
    if (!material->lastError.empty()) ImGui::TextWrapped("Asset error: %s", material->lastError.c_str());
    if (!material->runtimeError.empty()) ImGui::TextWrapped("Render error: %s", material->runtimeError.c_str());
    ImGui::TextWrapped("Shared asset: changes affect every object using this material.");
    ImGui::BeginDisabled(draft.dirty || !material->ready);
    if (ImGui::Button("Create Instance for This Object...###NewInstance", {-1, 0})) {
        draft.destination = suggestedPath(library, material->path, true); draft.creating = true;
    }
    ImGui::EndDisabled();
    hint(draft.dirty ? "Save the shared material changes first." : "Create a separate file that inherits this material and assign it to this object.");
    if (draft.creating) {
        ImGui::TextWrapped("The new instance inherits this material and will be assigned to this object.");
        ImGui::SetNextItemWidth(-1); ImGui::InputText("###Destination", &draft.destination);
        ImGui::BeginDisabled(draft.dirty);
        if (primaryButton("Create and Assign###ConfirmCreate") && createAsset(draft, mesh, library, material->path)) {
            ImGui::EndDisabled(); ImGui::PopID(); return;
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Cancel###CancelCreate")) draft.creating = false;
    }
    if (hasBase(draft)) {
        ImGui::TextWrapped("Base: %s", draft.source["base"].get_ref<const std::string&>().c_str());
        ImGui::TextWrapped("Check a parameter to override it. Unchecked values come from the base.");
    }
    const bool graph = hasGraph(draft);
    if (graph || hasHistory(draft.history)) {
        if (primaryButton("Open Material Graph###OpenMaterialGraph")) openGraph(library, material->path);
    }
    if (!graph && draft.shader == "shaders_hlsl/textured.shader.json") {
        if (ImGui::Button("Convert to Material Graph###ConvertMaterialGraph", {-1, 0})) {
            recordMaterialEdit(draft, MaterialGraphChange::Appearance, true);
            useLocalGraph(draft, starterGraph(draft.texture));
            openGraph(library, material->path);
        }
        hint("Create a graph from this material's texture. Save to apply the conversion.");
    }
    ImGui::Spacing(); ImGui::SeparatorText("Appearance");
    bool editable = property(draft, library, material->path, "tint", "Color and Opacity", {draft.tint.r, draft.tint.g, draft.tint.b, draft.tint.a});
    ImGui::BeginDisabled(!editable);
    if (ImGui::ColorButton("###TintColor", {draft.tint.r, draft.tint.g, draft.tint.b, draft.tint.a}, ImGuiColorEditFlags_AlphaPreviewHalf,
                           {ImGui::GetContentRegionAvail().x, 26})) ImGui::OpenPopup("MaterialColor");
    hint("Click the swatch to choose a color.");
    if (ImGui::BeginPopup("MaterialColor")) {
        ImGui::SetNextItemWidth(230);
        const bool edited = ImGui::ColorPicker4("###TintPicker", glm::value_ptr(draft.tint), ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_DisplayRGB);
        recordMaterialItemEdit(draft, edited);
        if (edited) changed(draft, "tint", {draft.tint.r, draft.tint.g, draft.tint.b, draft.tint.a}, true);
        ImGui::EndPopup();
    }
    float opacity = draft.tint.a * 100;
    ImGui::SetNextItemWidth(-1);
    const bool opacityEdited = ImGui::SliderFloat("###Opacity", &opacity, 0, 100, "Opacity: %.0f%%");
    recordMaterialItemEdit(draft, opacityEdited);
    if (opacityEdited) {
        draft.tint.a = opacity / 100;
        changed(draft, "tint", {draft.tint.r, draft.tint.g, draft.tint.b, draft.tint.a}, true);
    }
    ImGui::EndDisabled();
    if (!graph) {
        editable = property(draft, library, material->path, "texture", "Texture", draft.texture);
        ImGui::BeginDisabled(!editable);
        if (assetChoice("###Texture", draft.texture, "No Texture", draft, library, "textures", true, selected)) {
            draft.texture = selected; changed(draft, "texture", selected);
        }
        ImGui::EndDisabled();
    }
    editable = property(draft, library, material->path, "uvScale", "Texture Tiling (U / V)", {draft.uvScale.x, draft.uvScale.y});
    ImGui::BeginDisabled(!editable);
    const bool tilingEdited = ImGui::DragFloat2("###UVScale", glm::value_ptr(draft.uvScale), .05F, -1000, 1000, "%.2f");
    recordMaterialItemEdit(draft, tilingEdited);
    if (tilingEdited) changed(draft, "uvScale", {draft.uvScale.x, draft.uvScale.y}, true);
    hint("Texture repetitions along U and V. Ctrl+click for exact input.");
    ImGui::EndDisabled();
    editable = property(draft, library, material->path, "blend", "Blend Mode", std::array{"opaque", "alpha", "additive"}[static_cast<std::size_t>(draft.blend)]);
    ImGui::BeginDisabled(!editable);
    constexpr std::array labels{"Opaque", "Alpha Blend", "Additive"};
    constexpr std::array values{"opaque", "alpha", "additive"};
    if (ImGui::Combo("###Blend", &draft.blend, labels.data(), static_cast<int>(labels.size()))) {
        recordMaterialEdit(draft);
        draft.source["blend"] = values[static_cast<std::size_t>(draft.blend)];
        draft.depthWrite = draft.blend == 0;
        draft.source["depthWrite"] = draft.depthWrite;
        markMaterialChanged(draft);
    }
    ImGui::EndDisabled();
    if (draft.blend == 0 && draft.tint.a < 1) ImGui::TextWrapped("Opaque mode ignores alpha. Select Alpha Blend to show transparency.");
    if (draft.blend == 1 && draft.tint.a >= 1) ImGui::TextWrapped("Reduce color opacity to make the material transparent.");
    if (ImGui::CollapsingHeader("Resources and Depth###AdvancedMaterial")) {
        ImGui::TextUnformatted("Base Material");
        hint("Leave empty to keep the current appearance without inheritance. Enter applies the base to this draft.");
        ImGui::SetNextItemWidth(-1);
        const bool applyBase = ImGui::InputText("###Base", &draft.base, ImGuiInputTextFlags_EnterReturnsTrue);
        if (applyBase || ImGui::IsItemDeactivatedAfterEdit()) {
            recordMaterialEdit(draft);
            if (draft.base.empty()) {
                draft.source.erase("base");
                if (hasGraph(draft)) useLocalGraph(draft, localGraph(draft) ? *localGraph(draft) : draft.resolvedGraph);
                else { draft.source["shader"] = draft.shader; draft.source["texture"] = draft.texture; }
                draft.source["tint"] = {draft.tint.r, draft.tint.g, draft.tint.b, draft.tint.a};
                draft.source["uvScale"] = {draft.uvScale.x, draft.uvScale.y};
                draft.source["blend"] = values[static_cast<std::size_t>(draft.blend)];
                draft.source["depthTest"] = draft.depthTest; draft.source["depthWrite"] = draft.depthWrite;
            } else draft.source["base"] = draft.base;
            markMaterialChanged(draft);
            resolveDraft(draft, library, material->path);
        }
        if (!hasGraph(draft)) {
            editable = property(draft, library, material->path, "shader", "Shader", draft.shader);
            ImGui::BeginDisabled(!editable);
            const bool shaderEdited = ImGui::InputText("###Shader", &draft.shader);
            recordMaterialItemEdit(draft, shaderEdited);
            if (shaderEdited) changed(draft, "shader", draft.shader, true);
            ImGui::EndDisabled();
            editable = !hasBase(draft) || draft.source.contains("texture");
            ImGui::TextUnformatted("Texture Path"); ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(!editable);
            const bool textureEdited = ImGui::InputText("###TexturePath", &draft.texture);
            recordMaterialItemEdit(draft, textureEdited);
            if (textureEdited) changed(draft, "texture", draft.texture, true);
            ImGui::EndDisabled();
        }
        editable = property(draft, library, material->path, "depthTest", "Depth Test", draft.depthTest);
        ImGui::BeginDisabled(!editable);
        if (ImGui::Checkbox("Enabled###DepthTest", &draft.depthTest)) changed(draft, "depthTest", draft.depthTest);
        ImGui::EndDisabled();
        editable = property(draft, library, material->path, "depthWrite", "Depth Write", draft.depthWrite);
        ImGui::BeginDisabled(!editable);
        if (ImGui::Checkbox("Enabled###DepthWrite", &draft.depthWrite)) changed(draft, "depthWrite", draft.depthWrite);
        ImGui::EndDisabled();
        ImGui::TextWrapped("Transparent modes disable depth writes by default. These controls allow a manual override.");
        if (hasBase(draft) && ImGui::Button("Reset All Overrides###ResetOverrides", {-1, 0})) {
            recordMaterialEdit(draft);
            for (const auto* field : {"shader", "texture", "tint", "uvScale", "blend", "depthTest", "depthWrite", "graph", "graphTextures"}) draft.source.erase(field);
            markMaterialChanged(draft);
            resolveDraft(draft, library, material->path);
        }
    }
    ImGui::Spacing(); ImGui::Separator();
    ImGui::BeginDisabled(!draft.dirty);
    if (primaryButton("Save Material###SaveMaterial")) saveDraft(draft, material, library);
    ImGui::EndDisabled();
    if (ImGui::Button(draft.dirty ? "Discard Changes / Reload###DiscardMaterial" : "Reload from Disk###DiscardMaterial", {-1, 0})) {
        reloadDraft(draft, material, library);
    }
    showStatus(draft);
    ImGui::TextWrapped("Saving a material changes its asset file. Save the scene separately to keep object assignments.");
    ImGui::PopID();
}

namespace {
const Json& graphField(const Json& node, const char* key) {
    static const Json missing;
    const auto found = node.find(key);
    return found != node.end() ? *found : missing;
}

float graphNumber(const Json& value, float fallback) {
    if (!value.is_number()) return fallback;
    const float number = value.get<float>();
    return std::abs(number) <= 1'000'000.0F ? number : fallback;
}

void renderTextureNode(Draft& draft, Json& node, bool local, const resources::MaterialLibrary& library, const std::string& path) {
    const auto id = std::to_string(draft.graphView.selected);
    const auto& texture = graphField(node, "texture");
    auto current = texture.is_string() ? texture.get<std::string>() : std::string{};
    const auto& overrides = graphField(draft.source, "graphTextures");
    const auto entry = overrides.find(id);
    bool overridden = entry != overrides.end() && entry->is_string();
    if (overridden) current = entry->get<std::string>();
    if (!local && ImGui::Checkbox("Override Texture###OverrideGraphTexture", &overridden)) {
        recordMaterialEdit(draft);
        if (overridden) {
            if (!overrides.is_object()) draft.source["graphTextures"] = Json::object();
            draft.source["graphTextures"][id] = current;
        } else {
            draft.source["graphTextures"].erase(id);
        }
        markMaterialChanged(draft);
        resolveDraft(draft, library, path);
    }
    std::string selected;
    ImGui::BeginDisabled(!local && !overridden);
    if (assetChoice("###GraphTexture", current, "White", draft, library, "textures", true, selected) && selected != current) {
        recordMaterialEdit(draft);
        if (local) {
            node["texture"] = selected;
            if (overrides.is_object()) draft.source["graphTextures"].erase(id);
            markMaterialChanged(draft);
        } else {
            draft.source["graphTextures"][id] = selected;
            markMaterialChanged(draft);
            resolveDraft(draft, library, path);
        }
    }
    ImGui::EndDisabled();
}

void renderGraphNodeDetails(Draft& draft, Json& graph, bool local, const resources::MaterialLibrary& library, const std::string& path) {
    auto* node = findMaterialGraphNode(graph, draft.graphView.selected);
    const auto* type = node ? materialGraphNodeType(*node) : nullptr;
    if (!type) {
        ImGui::TextDisabled("Select a node to inspect it.");
        return;
    }
    const auto id = std::to_string(draft.graphView.selected);
    ImGui::PushID(id.c_str());
    ImGui::SeparatorText(type->label);
    using Kind = resources::MaterialGraphNodeKind;
    if (type->kind == Kind::Texture) {
        renderTextureNode(draft, *node, local, library, path);
    } else {
        const auto& value = graphField(*node, "value");
        ImGui::BeginDisabled(!local);
        ImGui::SetNextItemWidth(-1);
        switch (type->kind) {
            case Kind::Scalar: {
                float scalar = graphNumber(value, 0.0F);
                const bool edited = ImGui::DragFloat("###GraphScalar", &scalar, 0.01F, -1000, 1000, "%.3f");
                recordMaterialItemEdit(draft, edited);
                if (edited) {
                    (*node)["value"] = scalar;
                    markMaterialChanged(draft);
                }
                break;
            }
            case Kind::Color: {
                glm::vec4 color{1.0F};
                if (value.is_array() && value.size() == 4) {
                    for (int index = 0; index < 4; ++index) color[index] = graphNumber(value[static_cast<std::size_t>(index)], 1.0F);
                }
                const bool edited = ImGui::ColorEdit4("###GraphColor", glm::value_ptr(color), ImGuiColorEditFlags_AlphaBar);
                recordMaterialItemEdit(draft, edited);
                if (edited) {
                    (*node)["value"] = {color.r, color.g, color.b, color.a};
                    markMaterialChanged(draft);
                }
                break;
            }
            case Kind::Vector2: {
                glm::vec2 vector{1.0F};
                if (value.is_array() && value.size() == 2) vector = {graphNumber(value[0], 1), graphNumber(value[1], 1)};
                const bool edited = ImGui::DragFloat2("###GraphVector2", glm::value_ptr(vector), 0.01F, -1000, 1000, "%.3f");
                recordMaterialItemEdit(draft, edited);
                if (edited) {
                    (*node)["value"] = {vector.x, vector.y};
                    markMaterialChanged(draft);
                }
                break;
            }
            case Kind::Channel: {
                constexpr std::array names{"Red / X", "Green / Y", "Blue / Z", "Alpha / W"};
                constexpr std::array keys{"r", "g", "b", "a"};
                int channel = 0;
                for (std::size_t index = 0; index < keys.size(); ++index) {
                    if (value == keys[index]) channel = static_cast<int>(index);
                }
                if (ImGui::Combo("###GraphChannel", &channel, names.data(), static_cast<int>(names.size()))) {
                    recordMaterialEdit(draft);
                    (*node)["value"] = keys[static_cast<std::size_t>(channel)];
                    markMaterialChanged(draft);
                }
                break;
            }
            default: break;
        }
        ImGui::EndDisabled();
    }
    if (local && ImGui::Button("Delete Node###DeleteGraphNode", {-1, 0})) {
        recordMaterialEdit(draft);
        removeMaterialGraphNode(graph, draft.graphView);
        if (auto overrides = draft.source.find("graphTextures"); overrides != draft.source.end() && overrides->is_object()) overrides->erase(id);
        markMaterialChanged(draft);
    }
    ImGui::PopID();
}

void sameLineFor(const char* label) {
    const auto& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0F;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right) ImGui::SameLine();
}

void renderGraphPlacement(const MaterialGraphDock* dock, bool available, bool inlineControl) {
    if (!dock) return;
    const char* label = dock->docked ? "Float###FloatMaterialGraph" : "Dock beside Viewport###DockMaterialGraph";
    if (inlineControl) sameLineFor(label);
    ImGui::BeginDisabled(!available && !dock->docked);
    if (ImGui::Button(label)) {
        dock->docked = !dock->docked;
        graphWindow.fitOnPlacementChange = available;
    }
    ImGui::EndDisabled();
    if (!available && !dock->docked) hint("The Viewport needs space for a docked graph.");
}

void renderGraphActions(Draft& draft, const std::shared_ptr<resources::Material>& material, resources::MaterialLibrary& library,
                        const MaterialGraphDock* dock, bool dockAvailable, bool compact) {
    const char* save = compact ? "Save###SaveGraphMaterial" : "Save Material###SaveGraphMaterial";
    const char* validate = "Validate###ValidateGraphMaterial";
    const char* reload = draft.dirty ? (compact ? "Discard###ReloadGraphMaterial" : "Discard / Reload###ReloadGraphMaterial")
                                    : "Reload###ReloadGraphMaterial";
    const char* fit = compact ? "Fit###FitMaterialGraph" : "Fit Graph###FitMaterialGraph";
    const char* undo = "Undo###UndoGraphMaterial";
    const char* redo = "Redo###RedoGraphMaterial";
    const bool saveShortcut = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteFocused);
    ImGui::BeginDisabled(!draft.dirty);
    if (ImGui::Button(save) || (saveShortcut && draft.dirty)) saveDraft(draft, material, library);
    ImGui::EndDisabled();
    sameLineFor(validate);
    if (ImGui::Button(validate)) {
        resolveDraft(draft, library, material->path);
        if (!draft.failed) draft.status = "Material graph is valid.";
        if (draft.appearanceRevision != 0) ++draft.appearanceRevision;
    }
    sameLineFor(reload);
    if (ImGui::Button(reload)) reloadDraft(draft, material, library);
    sameLineFor(fit);
    if (ImGui::Button(fit)) draft.graphView.fit = true;
    hint("Fit all nodes. Middle drag to pan, scroll to zoom.");
    sameLineFor(undo);
    ImGui::BeginDisabled(draft.history.undo.empty());
    if (ImGui::Button(undo)) restoreMaterialEdit(draft, false, *material, library);
    ImGui::EndDisabled();
    hint("Undo (Ctrl+Z)");
    sameLineFor(redo);
    ImGui::BeginDisabled(draft.history.redo.empty());
    if (ImGui::Button(redo)) restoreMaterialEdit(draft, true, *material, library);
    ImGui::EndDisabled();
    hint("Redo (Ctrl+Y)");
    renderGraphPlacement(dock, dockAvailable, true);
}

void showGraphPreview(const Draft& draft, bool previewSelected, bool compact) {
    const auto& preview = graphWindow.preview;
    if (!previewSelected) {
        ImGui::TextDisabled("%s", compact ? "Preview unavailable" : "Select an object using this material in the Viewport to preview.");
        if (compact) hint("Select an object using this material in the Viewport to preview.");
        return;
    }
    if (draft.appearanceRevision == 0) {
        ImGui::TextDisabled("%s", compact ? "Preview ready" : "Material edits preview on the selected object.");
        if (compact) hint("Material edits preview on the selected object.");
        return;
    }
    const std::string* error = &preview.error;
    if (error->empty() && preview.material) error = &preview.material->runtimeError;
    if (!error->empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, .5F, .45F, 1});
        if (compact) {
            ImGui::TextUnformatted("Preview paused");
            hint(error->c_str());
        } else {
            ImGui::TextWrapped("Live preview kept the previous material.");
            if (*error != draft.status) ImGui::TextWrapped("%s", error->c_str());
        }
        ImGui::PopStyleColor();
    } else if (preview.appearanceRevision != draft.appearanceRevision) {
        ImGui::TextDisabled("Updating live preview...");
    } else {
        ImGui::TextDisabled("%s", compact ? "Live preview" : "Live preview on the selected object.");
    }
}

void showGraphStatus(const Draft& draft, const resources::Material& material, bool previewSelected, bool compact) {
    showGraphPreview(draft, previewSelected, compact);
    const char* recoveryError = draft.recoveryTooLarge ? "The draft exceeds the project recovery size limit." :
        draftStore.writeError.empty() ? nullptr : draftStore.writeError.c_str();
    if (recoveryError) {
        const char* label = "Draft recovery could not be written";
        if (compact) sameLineFor(label);
        ImGui::TextColored({1, .5F, .45F, 1}, "%s", label);
        hint(recoveryError);
    }
    if (!compact) {
        showStatus(draft);
        if (!material.lastError.empty()) ImGui::TextWrapped("Asset error: %s", material.lastError.c_str());
        if (!material.runtimeError.empty()) ImGui::TextWrapped("Render error: %s", material.runtimeError.c_str());
        return;
    }
    if (!draft.status.empty()) {
        const char* label = draft.failed ? "Draft error" : "Draft status";
        sameLineFor(label);
        ImGui::TextColored(draft.failed ? ImVec4{1, .5F, .45F, 1} : ImVec4{.45F, .8F, .6F, 1}, "%s", label);
        hint(draft.status.c_str());
    }
    for (const auto& [label, error] : {std::pair{"Asset error", &material.lastError}, std::pair{"Render error", &material.runtimeError}}) {
        if (error->empty()) continue;
        sameLineFor(label);
        ImGui::TextColored({1, .5F, .45F, 1}, "%s", label);
        hint(error->c_str());
    }
}

void placeGraphWindow(const MaterialGraphDock* dock, bool docked) {
    if (docked) {
        ImGui::SetNextWindowPos({dock->position.x, dock->position.y}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({dock->size.x, dock->size.y}, ImGuiCond_Always);
        return;
    }
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 maximum{std::max(1.0F, viewport->WorkSize.x), std::max(1.0F, viewport->WorkSize.y)};
    const ImVec2 minimum{std::min(480.0F, maximum.x), std::min(320.0F, maximum.y)};
    const ImVec2 preferred{std::min(880.0F, maximum.x), std::min(550.0F, maximum.y)};
    ImGui::SetNextWindowSizeConstraints(minimum, maximum);
    if (!graphWindow.placedInDock) {
        ImGui::SetNextWindowSize(preferred, ImGuiCond_FirstUseEver);
        return;
    }
    auto position = graphWindow.floatingPosition;
    auto size = graphWindow.floatingSize;
    if (size.x <= 0.0F || size.y <= 0.0F) {
        size = preferred;
        position = {viewport->WorkPos.x + (maximum.x - size.x) * 0.5F,
                    viewport->WorkPos.y + (maximum.y - size.y) * 0.5F};
    }
    size = {std::clamp(size.x, minimum.x, maximum.x), std::clamp(size.y, minimum.y, maximum.y)};
    position = {std::clamp(position.x, viewport->WorkPos.x, viewport->WorkPos.x + (maximum.x - size.x)),
                std::clamp(position.y, viewport->WorkPos.y, viewport->WorkPos.y + (maximum.y - size.y))};
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
}

bool graphPreviewSelected(const resources::Material& material, ecs::EntityId selected, const ecs::MeshRenderer* mesh) {
    return selected != ecs::kInvalidEntity && mesh &&
        (mesh->materialId == graphWindow.path || mesh->materialId == material.path ||
         (mesh->materialCacheKey == mesh->materialId && mesh->material.get() == &material));
}

std::shared_ptr<resources::Material> updateGraphPreview(const Draft* draft,
    const std::shared_ptr<resources::Material>& material, const resources::MaterialLibrary& library,
    bool previewSelected) {
    auto& preview = graphWindow.preview;
    if (!graphWindow.open || !draft || !previewSelected || draft->appearanceRevision == 0) {
        preview = {};
        return {};
    }
    if (preview.source.lock() != material) {
        preview = {};
        preview.source = material;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < preview.nextUpdate || (preview.appearanceRevision == draft->appearanceRevision &&
                                     preview.assetRevision == material->revision)) {
        return preview.material;
    }
    preview.nextUpdate = now + std::chrono::milliseconds(80);
    preview.appearanceRevision = draft->appearanceRevision;
    preview.assetRevision = material->revision;
    resources::MaterialDefinition definition;
    if (library.preview(material->path, draft->source, definition, &preview.error)) {
        if (!preview.material) {
            preview.material = std::make_shared<resources::Material>();
            preview.material->path = material->path;
            preview.material->ready = true;
        }
        preview.material->definition = std::move(definition);
        ++preview.material->revision;
        preview.material->runtimeError.clear();
    }
    return preview.material;
}
}

void loadMaterialGraphDrafts(resources::MaterialLibrary& library, const std::filesystem::path& path) {
    closeMaterialGraphDrafts();
    draftStore.file = path;
    draftStore.keyPrefix = draftKey(library, {});
    if (path.empty()) return;
    for (auto& [key, draft] : drafts) {
        if (key.starts_with(draftStore.keyPrefix) && hasRecovery(draft)) markDraftPersistence(draft);
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (!exists && !error) return;
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::streamoff>(input.tellg());
        if (size < 0 || size > static_cast<std::streamoff>(maxDraftFileBytes)) {
            throw std::runtime_error("Cannot read the file or it exceeds the recovery size limit.");
        }
        input.seekg(0);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) || input.peek() != std::char_traits<char>::eof()) {
            throw std::runtime_error("The file changed while it was being read.");
        }
        auto document = parseRecoveryJson(bytes);
        const auto version = document.find("version");
        auto entries = document.find("drafts");
        if (!document.is_object() || document.size() != 2 || version == document.end() ||
            !version->is_number_integer() || *version != 1 || entries == document.end() || !entries->is_object() ||
            entries->size() > maxDrafts) {
            throw std::runtime_error("Invalid material draft format.");
        }
        for (auto it = entries->begin(); it != entries->end();) {
            const auto original = it->find("original");
            const auto source = it->find("source");
            if (it.key().empty() || it.key().size() > 4096 || it.key().find('\0') != std::string::npos ||
                original == it->end() || !original->is_string() ||
                original->get_ref<const std::string&>().size() > maxMaterialSourceBytes ||
                !parseRecoveryJson(original->get_ref<const std::string&>()).is_object() ||
                source == it->end() || !source->is_object() || source->dump().size() > maxMaterialSourceBytes) {
                it = entries->erase(it);
            } else {
                ++it;
            }
        }
        draftStore.entries = std::move(*entries);
    } catch (const std::exception& failure) {
        draftStore.writeBlocked = true;
        draftStore.writeError = "The existing recovery file could not be read. It has been left intact.";
        ENGINE_LOG_WARN("Cannot recover material graph drafts from '{}': {}", utf8(path), failure.what());
    }
}

void flushMaterialGraphDrafts(bool force) {
    if (draftStore.file.empty() || draftStore.writeBlocked || (!draftStore.collectChanges && !draftStore.needsWrite)) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < draftStore.nextWrite) return;
    draftStore.nextWrite = now + std::chrono::milliseconds(250);
    try {
        if (draftStore.collectChanges) {
            for (auto& [key, draft] : drafts) {
                if (!draft.persistenceDirty || !key.starts_with(draftStore.keyPrefix)) continue;
                const auto path = key.substr(draftStore.keyPrefix.size());
                if (hasRecovery(draft) && !path.empty() && !draft.original.empty()) {
                    const bool exceedsLimit = draft.source.dump().size() > maxMaterialSourceBytes;
                    if (exceedsLimit && !draft.recoveryTooLarge) {
                        ENGINE_LOG_WARN("Material graph draft '{}' exceeds the recovery size limit", path);
                    }
                    draft.recoveryTooLarge = exceedsLimit;
                    if (!exceedsLimit) {
                        auto entry = Json{{"original", draft.original}, {"source", draft.source},
                            {"history", writeMaterialHistory(draft.history)}};
                        if (!draft.dirty) entry["historyOnly"] = true;
                        draftStore.entries[path] = std::move(entry);
                        draftStore.needsWrite = true;
                    }
                } else {
                    draft.recoveryTooLarge = false;
                    if (draftStore.entries.erase(path) != 0) draftStore.needsWrite = true;
                }
                draft.persistenceDirty = false;
            }
            draftStore.collectChanges = false;
        }
        if (!draftStore.needsWrite) return;
        const auto bytes = Json{{"version", 1}, {"drafts", draftStore.entries}}.dump();
        if (draftStore.entries.size() > maxDrafts || bytes.size() > maxDraftFileBytes) {
            throw std::runtime_error("The drafts exceed the project recovery size limit.");
        }
        if (draftStore.file.has_parent_path()) std::filesystem::create_directories(draftStore.file.parent_path());
        std::string error;
        if (!core::writeFileAtomically(draftStore.file, bytes, error)) throw std::runtime_error(error);
        draftStore.needsWrite = false;
        draftStore.writeError.clear();
    } catch (const std::exception& failure) {
        draftStore.nextWrite = now + std::chrono::seconds(2);
        if (draftStore.writeError != failure.what()) {
            draftStore.writeError = failure.what();
            ENGINE_LOG_WARN("Cannot save material graph drafts to '{}': {}", utf8(draftStore.file), draftStore.writeError);
        }
    }
}

void closeMaterialGraphDrafts() {
    if (!draftStore.file.empty()) {
        for (auto it = drafts.begin(); it != drafts.end();) {
            if (it->first.starts_with(draftStore.keyPrefix) &&
                (hasGraph(it->second) || hasHistory(it->second.history) ||
                    draftStore.entries.contains(it->first.substr(draftStore.keyPrefix.size()))) &&
                !(hasRecovery(it->second) && (it->second.persistenceDirty || it->second.recoveryTooLarge ||
                    draftStore.needsWrite || draftStore.writeBlocked))) {
                it = drafts.erase(it);
            } else {
                ++it;
            }
        }
    }
    draftStore = {};
}

bool materialGraphEditorOpen(const resources::MaterialLibrary& library) {
    return graphWindow.open && graphWindow.root == library.root();
}

std::optional<MaterialGraphSession> materialGraphSession(const resources::MaterialLibrary& library) {
    if (!materialGraphEditorOpen(library)) return std::nullopt;
    return MaterialGraphSession{graphWindow.path, graphWindow.canvas};
}

bool restoreMaterialGraphSession(resources::MaterialLibrary& library, const MaterialGraphSession& session) {
    const auto material = library.load(session.materialId);
    if (!material->ready || !material->lastError.empty()) return false;
    auto* draft = acquireDraft(library, draftKey(library, material->path));
    if (!draft) return false;
    refreshIfNeeded(*draft, material, library);
    if (!hasGraph(*draft) && !hasHistory(draft->history)) return false;

    graphWindow = {};
    graphWindow.root = library.root();
    graphWindow.path = material->path;
    graphWindow.open = true;
    graphWindow.restoreFocus = graphWindow.focus = hasHistory(draft->history);
    if (session.canvas) graphWindow.canvas = validatedCanvas(*session.canvas);

    draft->graphView = {};
    draft->history.activeItem = 0;
    draft->history.layoutDrag = false;
    if (graphWindow.canvas) {
        draft->graphView.pan = graphWindow.canvas->pan;
        draft->graphView.zoom = graphWindow.canvas->zoom;
        draft->graphView.fit = false;
    }
    return true;
}

void focusRestoredMaterialGraph() {
    if (!graphWindow.restoreFocus || ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
    graphWindow.restoreFocus = false;
    if (!graphWindow.open || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    if (auto* window = ImGui::FindWindowByName(materialGraphWindowName)) {
        ImGui::FocusWindow(window, ImGuiFocusRequestFlags_UnlessBelowModal);
        if (ImGui::GetCurrentContext()->NavWindow == window) ImGui::NavInitWindow(window, true);
    }
}

void closeMaterialGraphEditor() {
    graphWindow = {};
}

std::shared_ptr<resources::Material> materialGraphPreview(resources::MaterialLibrary& library,
    ecs::EntityId selected, const ecs::MeshRenderer* mesh) {
    if (!materialGraphEditorOpen(library)) return {};
    const auto found = drafts.find(draftKey(library, graphWindow.path));
    if (found == drafts.end() || !found->second.dirty || !mesh || selected == ecs::kInvalidEntity) return {};
    const auto material = found->second.resource.lock();
    if (!material) return {};
    const bool matches = graphPreviewSelected(*material, selected, mesh) ||
        (!mesh->materialId.empty() && mesh->materialCacheKey != mesh->materialId && library.load(mesh->materialId) == material);
    return updateGraphPreview(&found->second, material, library, matches);
}

std::shared_ptr<resources::Material> renderMaterialGraphEditor(resources::MaterialLibrary& library,
    ecs::EntityId selected, const ecs::MeshRenderer* mesh, const MaterialGraphDock* dock) {
    graphWindow.focusedContext = nullptr;
    if (graphWindow.root != library.root()) graphWindow = {};
    if (!graphWindow.open) {
        graphWindow.preview = {};
        return {};
    }
    const auto material = library.load(graphWindow.path);
    auto* entry = acquireDraft(library, draftKey(library, graphWindow.path));
    if (entry) refreshIfNeeded(*entry, material, library);
    const bool previewSelected = graphPreviewSelected(*material, selected, mesh);
    const bool dockAvailable = dock && dock->size.x > 0.0F && dock->size.y > 0.0F;
    const bool docked = dockAvailable && dock->docked;
    placeGraphWindow(dock, docked);
    if (graphWindow.fitOnPlacementChange) {
        if (entry && docked != graphWindow.placedInDock) entry->graphView.fit = true;
        graphWindow.fitOnPlacementChange = false;
    }
    graphWindow.placedInDock = docked;
    if (graphWindow.focus) {
        ImGui::SetNextWindowFocus();
        graphWindow.focus = false;
    }
    const ImGuiWindowFlags dockFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (docked) ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{1.0F, 1.0F});
    const bool visible = ImGui::Begin(materialGraphWindowName, &graphWindow.open, docked ? dockFlags : 0);
    if (docked) {
        ImGui::PopStyleVar();
    } else {
        graphWindow.floatingPosition = ImGui::GetWindowPos();
        if (visible) graphWindow.floatingSize = ImGui::GetWindowSize();
    }
    if (entry && graphWindow.open) {
        const bool undo = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteFocused);
        const bool redo = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteFocused);
        if ((undo || redo) && !ImGui::GetIO().WantTextInput) restoreMaterialEdit(*entry, redo, *material, library);
    }
    if (visible && graphWindow.open && entry) {
        auto& draft = *entry;
        const auto content = ImGui::GetContentRegionAvail();
        const bool sideProperties = content.x >= 630.0F;
        const bool compact = !sideProperties || content.y < 300.0F;
        if (compact) {
            const auto separator = material->path.find_last_of("/\\");
            const auto* name = material->path.c_str() + (separator == std::string::npos ? 0 : separator + 1);
            ImGui::Text("%s%s", name, draft.dirty ? "  *" : "");
            hint(material->path.c_str());
        } else {
            ImGui::TextWrapped("%s%s", material->path.c_str(), draft.dirty ? "  *" : "");
        }
        renderGraphActions(draft, material, library, dock, dockAvailable, compact);
        if (hasGraph(draft)) {
            if (!localGraph(draft)) {
                ImGui::TextDisabled("Inherited graph");
                ImGui::SameLine();
                if (ImGui::Button("Make Local Copy###CopyInheritedGraph")) {
                    recordMaterialEdit(draft);
                    useLocalGraph(draft, draft.resolvedGraph);
                }
            }
            const bool local = localGraph(draft) != nullptr;
            auto& graph = local ? draft.source["graph"] : draft.resolvedGraph;
            if (local) {
                const auto& nodes = graphField(graph, "nodes");
                ImGui::BeginDisabled(!nodes.is_array() || nodes.size() >= resources::maxMaterialGraphNodes);
                ImGui::SetNextItemWidth(compact ? 112.0F : 200.0F);
                if (ImGui::BeginCombo("###AddGraphNode", "Add Node")) {
                    for (const auto& type : resources::materialGraphNodeTypes()) {
                        if (ImGui::Selectable(type.label)) {
                            auto previous = draft.source.dump();
                            if (addMaterialGraphNode(graph, draft.graphView, type, draft.history.nextNodeId)) {
                                rememberNodeIds(draft);
                                pushMaterialEdit(draft, {std::move(previous), MaterialGraphChange::Appearance});
                                markMaterialChanged(draft);
                            } else { draft.failed = true; draft.status = "This graph has no unused node IDs."; }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
            }
            if (!sideProperties) {
                if (local) ImGui::SameLine();
                ImGui::Checkbox("Properties###MaterialGraphProperties", &graphWindow.showProperties);
                hint("Select a node to edit its properties. Drag from an output to connect. Right-click an input to disconnect.");
            } else if (!compact) {
                if (local) ImGui::SameLine();
                ImGui::TextDisabled("Middle drag to pan  |  Scroll to zoom");
            }
            showGraphStatus(draft, *material, previewSelected, compact);
            const float detailsWidth = sideProperties ? 235.0F : 0.0F;
            const float detailsHeight = !sideProperties && graphWindow.showProperties ?
                std::clamp(ImGui::GetContentRegionAvail().y * 0.42F, 1.0F, 155.0F) : 0.0F;
            const float spacing = sideProperties ? ImGui::GetStyle().ItemSpacing.x : ImGui::GetStyle().ItemSpacing.y;
            const ImVec2 canvasSize{sideProperties ? -detailsWidth - spacing : 0.0F,
                                    detailsHeight > 0.0F ? -detailsHeight - spacing : 0.0F};
            if (ImGui::BeginChild("Graph Canvas", canvasSize, ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                std::string linkError;
                const auto change = renderMaterialGraphCanvas(graph, draft.graphView, local, linkError,
                    [&](MaterialGraphChange edit) { recordMaterialEdit(draft, edit); });
                graphWindow.canvas = validatedCanvas({draft.graphView.pan, draft.graphView.zoom});
                if (!draft.graphView.draggingLayout()) draft.history.layoutDrag = false;
                if (change == MaterialGraphChange::Appearance) markMaterialChanged(draft);
                if (change == MaterialGraphChange::Layout) {
                    draft.dirty = true;
                    markDraftPersistence(draft);
                }
                if (!linkError.empty()) { draft.failed = true; draft.status = std::move(linkError); }
            }
            ImGui::EndChild();
            if (sideProperties || graphWindow.showProperties) {
                if (sideProperties) ImGui::SameLine();
                if (ImGui::BeginChild("Node Properties", {detailsWidth, detailsHeight}, ImGuiChildFlags_Borders)) {
                    if (local && sideProperties && !compact) ImGui::TextWrapped("Drag from an output pin to an input. Right-click an input to disconnect.");
                    renderGraphNodeDetails(draft, graph, local, library, material->path);
                }
                ImGui::EndChild();
            }
        } else {
            ImGui::TextWrapped("This material has no graph. Convert it in the Inspector to start editing.");
            showGraphStatus(draft, *material, previewSelected, compact);
        }
    } else if (visible && graphWindow.open) {
        renderGraphPlacement(dock, dockAvailable, false);
        ImGui::TextWrapped("Save material drafts before opening more assets.");
    }
    if (graphWindow.open && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        graphWindow.focusedContext = ImGui::GetCurrentContext();
    ImGui::End();
    return updateGraphPreview(entry, material, library, previewSelected);
}

bool materialGraphEditorFocused() {
    const auto* context = ImGui::GetCurrentContext();
    return graphWindow.open && context && (graphWindow.focus || graphWindow.focusedContext == context);
}
}
