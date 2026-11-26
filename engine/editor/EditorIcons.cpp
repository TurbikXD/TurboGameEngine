#include "engine/editor/EditorIcons.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace engine::editor::EditorIcons {
namespace {
using Id = EditorCommandId;
constexpr float kPi = 3.14159265F;

bool image(const std::string_view path, const float x, const float y, const float size, const Resolver& resolve, const bool enabled) {
    if (!resolve || path.empty()) { return false; }
    const Image value = resolve(path);
    if (!value.textureId || !std::isfinite(value.width) || !std::isfinite(value.height) || value.width <= 0.0F || value.height <= 0.0F) { return false; }
    const float scale = size / std::max(value.width, value.height);
    const float width = value.width * scale, height = value.height * scale;
    const ImVec2 minimum(x + (size - width) * 0.5F, y + (size - height) * 0.5F);
    const ImU32 tint = ImGui::GetColorU32(ImVec4(1.0F, 1.0F, 1.0F, enabled ? 1.0F : 0.35F));
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(value.textureId)),
        minimum, ImVec2(minimum.x + width, minimum.y + height), ImVec2(0, 0), ImVec2(1, 1), tint);
    return true;
}

void fallback(const Id command, const float x, const float y, const float size, const bool enabled) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImVec4(0.79F, 0.81F, 0.86F, enabled ? 1.0F : 0.35F));
    const float unit = size / 24.0F, thickness = std::max(1.0F, unit * 1.5F);
    const auto point = [=](const float px, const float py) { return ImVec2(x + px * unit, y + py * unit); };
    const auto line = [&](const float ax, const float ay, const float bx, const float by) { draw->AddLine(point(ax, ay), point(bx, by), color, thickness); };
    if (command == Id::Play || command == Id::PlayWindow) {
        const ImU32 playColor = ImGui::GetColorU32(ImVec4(0.29F, 0.43F, 1.0F, enabled ? 1.0F : 0.35F));
        draw->AddTriangleFilled(point(7, 4), point(7, 20), point(20, 12), playColor);
    } else if (command == Id::Pause) {
        draw->AddRectFilled(point(7, 5), point(10, 19), color); draw->AddRectFilled(point(14, 5), point(17, 19), color);
    } else if (command == Id::StartNetworkTest) {
        draw->AddRect(point(8, 3), point(16, 9), color, 1.0F * unit, 0, thickness);
        line(12, 9, 12, 13); line(6, 13, 18, 13); line(6, 13, 6, 16); line(18, 13, 18, 16);
        draw->AddRect(point(2, 16), point(10, 22), color, 1.0F * unit, 0, thickness);
        draw->AddRect(point(14, 16), point(22, 22), color, 1.0F * unit, 0, thickness);
    } else if (command == Id::Stop || command == Id::StopWindow || command == Id::StopNetworkTest) {
        draw->AddRectFilled(point(6, 6), point(18, 18), ImGui::GetColorU32(ImVec4(0.75F, 0.23F, 0.24F, enabled ? 1.0F : 0.35F)));
    } else if (command == Id::ToolSelect) {
        const ImVec2 points[]{point(4, 3), point(9, 21), point(13, 14), point(21, 10)};
        draw->AddPolyline(points, 4, color, ImDrawFlags_Closed, thickness);
    } else if (command == Id::ToolMove) {
        line(12, 2, 12, 22); line(2, 12, 22, 12);
        line(9, 5, 12, 2); line(15, 5, 12, 2); line(9, 19, 12, 22); line(15, 19, 12, 22);
        line(5, 9, 2, 12); line(5, 15, 2, 12); line(19, 9, 22, 12); line(19, 15, 22, 12);
    } else if (command == Id::ToolRotate || command == Id::Undo || command == Id::Redo) {
        draw->PathArcTo(point(12, 12), 8.0F * unit, -kPi * 0.25F, kPi * 1.35F, 18); draw->PathStroke(color, 0, thickness);
        draw->AddTriangleFilled(point(3, 4), point(3, 12), point(10, 6), color);
    } else if (command == Id::ToolScale) {
        draw->AddRect(point(4, 10), point(14, 20), color, 0, 0, thickness);
        line(11, 13, 21, 3); line(15, 3, 21, 3); line(21, 3, 21, 9);
    } else if (command == Id::CreatePhysicsSphere) {
        draw->AddCircle(point(12, 12), 9 * unit, color, 20, thickness);
        draw->AddEllipse(point(12, 12), ImVec2(4 * unit, 9 * unit), color, 0, 20, thickness);
        line(3, 12, 21, 12);
    } else if (command == Id::CreatePhysicsCube || command == Id::CreateRenderCube || command == Id::CreateEmpty || command == Id::AddComponent) {
        const ImVec2 outline[]{point(12, 2), point(22, 7), point(22, 17), point(12, 22), point(2, 17), point(2, 7)};
        draw->AddPolyline(outline, 6, color, ImDrawFlags_Closed, thickness);
        line(2, 7, 12, 12); line(22, 7, 12, 12); line(12, 12, 12, 22);
    } else if (command == Id::ShowHierarchy) {
        line(5, 4, 5, 20); line(5, 7, 11, 7); line(5, 17, 11, 17);
        draw->AddRect(point(11, 3), point(21, 10), color); draw->AddRect(point(11, 14), point(21, 21), color);
    } else if (command == Id::ShowContent || command == Id::ProjectSwitch) {
        const ImVec2 outline[]{point(2, 5), point(10, 5), point(12, 8), point(22, 8), point(22, 20), point(2, 20)};
        draw->AddPolyline(outline, 6, color, ImDrawFlags_Closed, thickness);
    } else if (command == Id::SaveScene || command == Id::SaveLayout || command == Id::ScriptSave || command == Id::ScriptSaveAll) {
        draw->AddRect(point(3, 3), point(21, 21), color, 1.0F * unit, 0, thickness);
        draw->AddRect(point(7, 3), point(17, 10), color, 0, 0, thickness);
        draw->AddRect(point(7, 14), point(17, 21), color, 0, 0, thickness);
    } else {
        draw->AddRect(point(4, 3), point(20, 21), color, 1.0F * unit, 0, thickness);
        line(8, 8, 16, 8); line(8, 12, 16, 12); line(8, 16, 14, 16);
    }
}
}

std::string_view assetPath(const Id command) noexcept {
    switch (command) {
    case Id::ToolSelect: return "editor/icons/RibbonSelect.png";
    case Id::ToolMove: return "editor/icons/RibbonMove.png";
    case Id::ToolScale: return "editor/icons/RibbonScale.png";
    case Id::ToolRotate: return "editor/icons/RibbonRotate.png";
    case Id::ToggleLocalSpace: return "editor/icons/RibbonGeometric.png";
    case Id::ToggleSnap: return "editor/icons/RibbonUIGridLayout.png";
    case Id::CreateEmpty: case Id::CreateRenderCube: case Id::CreatePhysicsCube: return "editor/icons/RibbonPart.png";
    case Id::CreatePhysicsSphere: return "editor/icons/RibbonPart_Sphere.png";
    case Id::CreatePhysicsPyramid: return "editor/icons/RibbonPart_Wedge.png";
    case Id::CreateCamera: return "editor/icons/RibbonCamera.png";
    case Id::RandomizeTint: return "editor/icons/RibbonColor.png";
    case Id::ToggleVisible: return "editor/icons/RibbonVisible.png";
    case Id::ToggleStatic: return "editor/icons/RibbonAnchor.png";
    case Id::ToggleGravity: case Id::ZeroVelocity: return "editor/icons/RibbonVectorForce.png";
    case Id::ToggleCollider: case Id::ToggleColliderDebug: return "editor/icons/RibbonPhysical.png";
    case Id::AddComponent: return "editor/icons/RibbonAddTools.png";
    case Id::ShowHierarchy: return "editor/icons/RibbonExplorer.png";
    case Id::ShowInspector: return "editor/icons/RibbonProperties.png";
    case Id::ProjectSwitch: case Id::ShowContent: return "editor/icons/RibbonAssetManager.png";
    case Id::ShowStats: return "editor/icons/RibbonPerformance.png";
    case Id::ShowAssistant: return "editor/icons/RibbonAI.png";
    case Id::ShowScriptWorkspace: case Id::ScriptNew: return "editor/icons/RibbonScript.png";
    case Id::Play: return "editor/icons/RibbonPlaySmall.png";
    case Id::PlayWindow: return "editor/icons/RibbonClientSmall.png";
    case Id::Pause: return "editor/icons/RibbonPauseSmall.png";
    case Id::Stop: case Id::StopWindow: case Id::StopNetworkTest: return "editor/icons/RibbonStopActiveSmall.png";
    case Id::StepTick: return "editor/icons/RibbonStepForwardSmall.png";
    case Id::Undo: return "editor/icons/RibbonBack.png";
    case Id::Redo: return "editor/icons/RibbonFwd.png";
    case Id::Duplicate: return "editor/icons/RibbonDuplicate.png";
    case Id::Delete: return "editor/icons/RibbonDeleteSmall.png";
    case Id::FocusSelected: case Id::FrameScene: return "editor/icons/RibbonFrame.png";
    case Id::ResetTransform: return "editor/icons/RibbonResetPivot.png";
    case Id::DropToGround: return "editor/icons/RibbonAlignPosition.png";
    case Id::ScriptDemo: return "editor/icons/RibbonReactDevTools.png";
    case Id::ShowViewport: return "editor/icons/RibbonViewportFrame.png";
    case Id::ShowStatus: case Id::ScriptOutput: return "editor/icons/RibbonOutput.png";
    case Id::ToggleHoverOutline: return "editor/icons/RibbonEffect_Highlight.png";
    case Id::ToggleLayoutLock: return "editor/icons/RibbonLock.png";
    case Id::ResetLayout: return "editor/icons/RibbonResetPivot.png";
    case Id::SaveLayout: case Id::SaveScene: case Id::ScriptSave: case Id::ScriptSaveAll: return {};
    case Id::LoadScene: case Id::ScriptOpen: return "editor/icons/RibbonAssetManager.png";
    case Id::BookmarkStore1: case Id::BookmarkRecall1: return "editor/icons/RibbonViewportFrame.png";
    case Id::ScriptSaveReload: case Id::ScriptReloadDisk: return "editor/icons/RibbonUpdateSmall.png";
    case Id::ScriptCheck: return "editor/icons/RibbonAnalysis.png";
    case Id::ScriptFind: return "editor/icons/RibbonFind.png";
    case Id::ScriptGoToLine: return "editor/icons/RibbonGoToLine.png";
    case Id::ScriptComment: return "editor/icons/RibbonCodeSnippet.png";
    case Id::ScriptFormat: return "editor/icons/RibbonFormatDocument.png";
    case Id::ScriptCommand: return "editor/icons/RibbonCommand.png";
    case Id::ScriptState: return "editor/icons/RibbonWatch.png";
    case Id::ScriptErrors: return "editor/icons/RibbonCallStack.png";
    default: return {};
    }
}

std::string_view assetPath(const Icon icon) noexcept {
    switch (icon) {
    case Icon::ChevronDown: return "editor/icons/RibbonArrowDownAngleSmall.png";
    case Icon::More: return "editor/icons/RibbonMoreSmall.png";
    case Icon::MoveSnap: return "editor/icons/RibbonMoveSpinboxSmall.png";
    case Icon::RotateSnap: return "editor/icons/RibbonRotateSpinboxSmall.png";
    case Icon::Coordinates: return "editor/icons/Geometric.png";
    }
    return {};
}

void draw(const Id command, const float x, const float y, const float size, const Resolver& resolve, const bool enabled) {
    if (!image(assetPath(command), x, y, size, resolve, enabled)) { fallback(command, x, y, size, enabled); }
}

void draw(const Icon icon, const float x, const float y, const float size, const Resolver& resolve, const bool enabled) {
    if (image(assetPath(icon), x, y, size, resolve, enabled)) { return; }
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImVec4(0.79F, 0.81F, 0.86F, enabled ? 1.0F : 0.35F));
    if (icon == Icon::ChevronDown) {
        draw->AddLine(ImVec2(x + size * .25F, y + size * .4F), ImVec2(x + size * .5F, y + size * .65F), color, 1.3F);
        draw->AddLine(ImVec2(x + size * .5F, y + size * .65F), ImVec2(x + size * .75F, y + size * .4F), color, 1.3F);
    } else if (icon == Icon::More) {
        for (int i = 0; i < 3; ++i) { draw->AddCircleFilled(ImVec2(x + size * (.25F + .25F * i), y + size * .5F), size * .065F, color); }
    } else { fallback(icon == Icon::RotateSnap ? Id::ToolRotate : icon == Icon::MoveSnap ? Id::ToolMove : Id::ToggleLocalSpace, x, y, size, enabled); }
}

}
