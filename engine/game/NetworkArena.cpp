#include "engine/game/NetworkArena.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "engine/ecs/components.h"
#include "engine/ecs/render_system.h"
#include "engine/ecs/world.h"
#include "engine/game/NetworkArenaRules.h"
#include "engine/game/StateStack.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/renderer/RenderAdapter.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace engine::game {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using platform::KeyCode;

constexpr float MmToMeters = 0.001F;
constexpr float ArenaHalfWidth = static_cast<float>(ArenaHalfWidthMm) * MmToMeters;
constexpr float ArenaHalfDepth = static_cast<float>(ArenaHalfDepthMm) * MmToMeters;
constexpr float RelayRadius = static_cast<float>(ArenaCaptureRadiusMm) * MmToMeters;
constexpr float PanelMargin = 14.0F;
constexpr glm::vec4 LocalColor{0.27F, 0.85F, 1.0F, 1.0F};
constexpr glm::vec4 RelayColor{0.40F, 0.93F, 0.63F, 1.0F};
constexpr glm::vec4 InactiveRelayColor{0.34F, 0.47F, 0.63F, 1.0F};
constexpr ImVec4 TextSecondary{0.61F, 0.69F, 0.79F, 1.0F};
constexpr ImVec4 TextGood{0.51F, 0.94F, 0.72F, 1.0F};
constexpr ImVec4 TextWarn{1.0F, 0.68F, 0.48F, 1.0F};

constexpr std::array PilotColors{
    glm::vec4{1.0F, 0.55F, 0.35F, 1.0F},
    glm::vec4{0.90F, 0.68F, 0.32F, 1.0F},
    glm::vec4{0.74F, 0.57F, 1.0F, 1.0F},
    glm::vec4{0.95F, 0.48F, 0.72F, 1.0F},
};

std::atomic_flag serverStop{};

const char* phaseName(net::ClientPhase phase) {
    switch (phase) {
        case net::ClientPhase::Disconnected: return "Disconnected";
        case net::ClientPhase::Connecting: return "Connecting";
        case net::ClientPhase::Connected: return "Connected";
        case net::ClientPhase::Rejected: return "Rejected";
        case net::ClientPhase::TimedOut: return "Timed out";
    }
    return "Disconnected";
}

float panelWidth(float width) {
    return std::min(std::clamp(width * 0.27F, 264.0F, 326.0F), width * 0.53F);
}

glm::vec3 relayPosition(const ArenaRelay& relay) {
    return {static_cast<float>(relay.x) * MmToMeters, 0.0F, static_cast<float>(relay.z) * MmToMeters};
}

glm::vec4 pilotColor(const net::VisualEntity& entity, net::EntityId local) {
    return entity.id == local ? LocalColor : PilotColors[entity.flags % PilotColors.size()];
}

float snapshotSavings(const net::NetworkStats& stats) {
    if (stats.fullSnapshotBytes == 0) return 0.0F;
    return 100.0F * (1.0F - static_cast<float>(stats.snapshotBytes) / static_cast<float>(stats.fullSnapshotBytes));
}

class NetworkArena final : public IGameState, public IGameStateUi {
public:
    NetworkArena(StateStack& stack, net::Endpoint server)
        : IGameState(stack), m_endpoint(server), m_endpointLabel(net::formatEndpoint(server)),
          m_client(simulateArenaPlayer) {}

    void onEnter() override;
    void onExit() override;
    void handleEvent(const platform::Event& event) override;
    void update(double dt) override;
    void render(renderer::Renderer& renderer) override;
    void renderUi(renderer::Renderer& renderer) override;

private:
    struct RelayVisual {
        ecs::EntityId pad{};
        ecs::EntityId crystal{};
    };

    struct PilotVisual {
        ecs::EntityId base{};
        ecs::EntityId hull{};
        ecs::EntityId nose{};
        std::uint16_t score{};
        Clock::time_point chargedAt{};
    };

    struct TrafficRates {
        double txPackets{};
        double rxPackets{};
        double txBytes{};
        double rxBytes{};
    };

    ecs::EntityId mesh(const char* model, glm::vec3 position, glm::vec3 scale, glm::vec4 tint);
    void buildArena();
    void syncPilots();
    void reconnect();
    void updateEmulation();
    void submitCapture(bool invalid);
    void sampleTraffic(const net::TransportStats& stats);
    const net::VisualEntity* localPilot() const;
    void drawLabels(ImDrawList& drawList, ImVec2 viewportPosition) const;

    net::Endpoint m_endpoint;
    std::string m_endpointLabel;
    net::NetworkClient m_client;
    ecs::World m_world;
    ecs::RenderSystem m_renderSystem;
    std::array<RelayVisual, ArenaRelays.size()> m_relays{};
    std::map<net::EntityId, PilotVisual> m_pilots;
    std::vector<net::VisualEntity> m_presented;
    std::bitset<static_cast<std::size_t>(KeyCode::Count)> m_movePresses;
    net::EntityId m_titleId{};
    int m_latencyMs{};
    int m_lossPercent{};
    bool m_linkCut{};
    bool m_prediction{true};
    bool m_interpolation{true};
    std::string m_setupError;
    std::string m_rpcFeedback;
    ImVec4 m_rpcColor{TextSecondary};
    TrafficRates m_rates;
    net::TransportStats m_previousTraffic;
    Clock::time_point m_sampleTime{};
    Clock::time_point m_lastUpdate{};
    float m_arenaTime{};
    glm::mat4 m_viewProjection{1.0F};
    glm::vec2 m_scenePosition{};
    glm::vec2 m_sceneSize{};
};

ecs::EntityId NetworkArena::mesh(const char* model, glm::vec3 position, glm::vec3 scale, glm::vec4 tint) {
    const auto entity = m_world.createEntity();
    ecs::Transform transform{};
    transform.position = position;
    transform.scale = scale;
    m_world.addComponent<ecs::Transform>(entity, transform);
    ecs::MeshRenderer renderable{};
    renderable.meshId = model;
    renderable.shaderId = "assets/shaders_hlsl/textured.shader.json";
    renderable.tint = tint;
    m_world.addComponent<ecs::MeshRenderer>(entity, std::move(renderable));
    return entity;
}

void NetworkArena::buildArena() {
    const glm::vec4 foundation{0.13F, 0.20F, 0.31F, 1.0F};
    const glm::vec4 grid{0.19F, 0.28F, 0.39F, 1.0F};
    const glm::vec4 border{0.28F, 0.44F, 0.59F, 1.0F};
    const float floorWidth = (ArenaHalfWidth + 0.65F) * 2.0F;
    const float floorDepth = (ArenaHalfDepth + 0.65F) * 2.0F;
    mesh("builtin:cube", {0, -0.18F, 0}, {floorWidth, 0.34F, floorDepth}, foundation);
    for (int x = -14; x <= 14; x += 2) {
        mesh("builtin:cube", {static_cast<float>(x), 0.012F, 0}, {0.025F, 0.025F, ArenaHalfDepth * 2.0F}, grid);
    }
    for (int z = -6; z <= 6; z += 2) {
        mesh("builtin:cube", {0, 0.012F, static_cast<float>(z)}, {ArenaHalfWidth * 2.0F, 0.025F, 0.025F}, grid);
    }
    for (float side : {-1.0F, 1.0F}) {
        mesh("builtin:cube", {0, 0.10F, side * (ArenaHalfDepth + 0.30F)}, {floorWidth, 0.24F, 0.26F}, border);
        mesh("builtin:cube", {side * (ArenaHalfWidth + 0.30F), 0.10F, 0}, {0.26F, 0.24F, floorDepth}, border);
    }
    for (std::size_t i = 0; i < ArenaRelays.size(); ++i) {
        const auto position = relayPosition(ArenaRelays[i]);
        m_relays[i].pad = mesh("builtin:sphere", position + glm::vec3{0, 0.05F, 0},
            {RelayRadius * 2.0F, 0.09F, RelayRadius * 2.0F}, grid);
        mesh("builtin:cube", position + glm::vec3{0, 0.18F, 0}, {0.98F, 0.35F, 0.98F}, border);
        m_relays[i].crystal = mesh("builtin:cube", position + glm::vec3{0, 0.79F, 0},
            {0.58F, 0.74F, 0.58F}, InactiveRelayColor);
    }
}

void NetworkArena::onEnter() {
    m_renderSystem.setJobSystem(stack().jobs());
    buildArena();
    reconnect();
}

void NetworkArena::onExit() {
    m_client.disconnect();
}

void NetworkArena::reconnect() {
    m_setupError.clear();
    m_rpcFeedback.clear();
    m_linkCut = false;
    updateEmulation();
    m_sampleTime = {};
    m_rates = {};
    m_client.connect(m_endpoint, m_setupError);
}

void NetworkArena::updateEmulation() {
    m_client.setEmulation({static_cast<std::uint16_t>(m_latencyMs),
        static_cast<std::uint8_t>(m_linkCut ? 100 : m_lossPercent)});
}

const net::VisualEntity* NetworkArena::localPilot() const {
    const auto id = m_client.localEntity();
    const auto found = std::find_if(m_presented.begin(), m_presented.end(),
        [id](const auto& entity) { return entity.id == id; });
    return found == m_presented.end() ? nullptr : &*found;
}

void NetworkArena::submitCapture(bool invalid) {
    const auto* local = localPilot();
    if (!local || m_client.phase() != net::ClientPhase::Connected) return;
    const auto relayIndex = static_cast<std::int32_t>(nextArenaRelay(local->flags, local->value));
    const net::RpcCommand command{invalid ? ArenaUnknownAction : ArenaCaptureAction, local->id, relayIndex};
    const auto request = m_client.call(command);
    m_rpcFeedback = request == 0 ? "RPC queue full or connection unavailable" :
        "RPC #" + std::to_string(request) + " pending";
    m_rpcColor = request == 0 ? TextWarn : TextSecondary;
}

void NetworkArena::handleEvent(const platform::Event& event) {
    if (event.type != platform::EventType::KeyPressed || event.repeat) return;
    switch (event.key) {
        case KeyCode::W:
        case KeyCode::A:
        case KeyCode::S:
        case KeyCode::D:
            m_movePresses.set(static_cast<std::size_t>(event.key));
            break;
        case KeyCode::E: submitCapture(false); break;
        case KeyCode::X: submitCapture(true); break;
        default: break;
    }
}

void NetworkArena::update(double dt) {
    const auto down = [this](KeyCode key) {
        return m_movePresses.test(static_cast<std::size_t>(key)) || platform::Input::isKeyDown(key);
    };
    const auto axis = [&down](KeyCode positive, KeyCode negative) {
        const int value = static_cast<int>(down(positive)) - static_cast<int>(down(negative));
        return static_cast<std::int8_t>(value * 127);
    };
    m_client.tick({axis(KeyCode::D, KeyCode::A), axis(KeyCode::S, KeyCode::W)});
    m_movePresses.reset();
    m_lastUpdate = Clock::now();
    m_arenaTime = std::fmod(m_arenaTime + static_cast<float>(dt), 4.0F * std::numbers::pi_v<float>);
    net::RpcResult result{};
    while (m_client.takeResult(result)) {
        const std::string status(net::rpcStatusName(result.status));
        m_rpcFeedback = "RPC #" + std::to_string(result.request) + "  " + status;
        m_rpcColor = result.status == net::RpcStatus::Accepted ? TextGood : TextWarn;
        if (result.status == net::RpcStatus::Accepted) {
            m_rpcFeedback += "  |  Charge " + std::to_string(result.value);
        }
    }
    const auto local = m_client.localEntity();
    if (m_titleId != local) {
        m_titleId = local;
        stack().window()->setTitle(local == 0 ? "TurboGameEngine - Relay Arena" :
            "Relay Arena - Pilot " + std::to_string(local));
    }
}

void NetworkArena::syncPilots() {
    const auto local = m_client.localEntity();
    const auto now = Clock::now();
    const float fraction = std::clamp(std::chrono::duration<float>(now - m_lastUpdate).count() *
        static_cast<float>(net::TickRate), 0.0F, 1.0F);
    m_presented = m_client.presentation(fraction);
    for (const auto& pilot : m_presented) {
        const glm::vec3 position{pilot.x, pilot.y, pilot.z};
        const auto color = pilotColor(pilot, local);
        auto [entry, inserted] = m_pilots.try_emplace(pilot.id);
        auto& visual = entry->second;
        if (inserted) {
            visual.base = mesh("builtin:sphere", position, {1.45F, 0.14F, 1.45F}, color * 0.57F);
            visual.hull = mesh("builtin:sphere", position, {1.10F, 0.73F, 1.04F}, color);
            visual.nose = mesh("builtin:cube", position, {0.28F, 0.19F, 0.46F}, {0.90F, 0.95F, 1.0F, 1.0F});
            visual.score = pilot.value;
        } else if (visual.score != pilot.value) {
            visual.score = pilot.value;
            visual.chargedAt = now;
        }
        m_world.getComponent<ecs::Transform>(visual.base)->position = position + glm::vec3{0, 0.08F, 0};
        auto& hull = *m_world.getComponent<ecs::Transform>(visual.hull);
        hull.position = position + glm::vec3{0, 0.43F, 0};
        hull.rotationEulerRadians.y = pilot.yaw;
        auto& nose = *m_world.getComponent<ecs::Transform>(visual.nose);
        nose.position = position + glm::vec3{std::sin(pilot.yaw) * 0.49F, 0.52F, std::cos(pilot.yaw) * 0.49F};
        nose.rotationEulerRadians.y = pilot.yaw;
        auto& base = *m_world.getComponent<ecs::MeshRenderer>(visual.base);
        base.tint = now - visual.chargedAt < 600ms ? RelayColor : glm::vec4(glm::vec3(color) * 0.57F, 1.0F);
    }
    std::erase_if(m_pilots, [this](const auto& entry) {
        const auto id = entry.first;
        if (std::any_of(m_presented.begin(), m_presented.end(), [id](const auto& pilot) { return pilot.id == id; })) {
            return false;
        }
        m_world.destroyEntity(entry.second.base);
        m_world.destroyEntity(entry.second.hull);
        m_world.destroyEntity(entry.second.nose);
        return true;
    });
    const auto* pilot = localPilot();
    const auto target = pilot ? nextArenaRelay(pilot->flags, pilot->value) : ArenaRelays.size();
    for (std::size_t i = 0; i < m_relays.size(); ++i) {
        const auto& relay = m_relays[i];
        const bool active = i == target;
        auto& crystal = *m_world.getComponent<ecs::Transform>(relay.crystal);
        crystal.rotationEulerRadians = {0.29F, m_arenaTime, 0.22F};
        crystal.position.y = 0.87F + 0.10F * std::sin(m_arenaTime * 1.5F);
        m_world.getComponent<ecs::MeshRenderer>(relay.crystal)->tint = active ? RelayColor : InactiveRelayColor;
        m_world.getComponent<ecs::MeshRenderer>(relay.pad)->tint = active ?
            glm::vec4{0.26F, 0.56F, 0.42F, 1.0F} : glm::vec4{0.23F, 0.31F, 0.43F, 1.0F};
    }
}

void NetworkArena::render(renderer::Renderer& renderer) {
    syncPilots();
    const auto extent = renderer.frameExtent();
    const float width = static_cast<float>(extent.width);
    const float height = static_cast<float>(extent.height);
    const float left = renderer.isImGuiEnabled() ? panelWidth(width) + PanelMargin * 2.0F : 0.0F;
    m_scenePosition = {left, 40.0F};
    m_sceneSize = {width - left - PanelMargin, height - 68.0F};
    if (m_sceneSize.x <= 0.0F || m_sceneSize.y <= 0.0F) return;
    const float aspect = m_sceneSize.x / m_sceneSize.y;
    const float halfHeight = std::max(9.0F, 18.2F / aspect);
    const glm::vec3 camera{0, 22, 13};
    m_viewProjection = glm::orthoRH_ZO(-halfHeight * aspect, halfHeight * aspect, -halfHeight, halfHeight, 0.1F, 80.0F) *
        glm::lookAtRH(camera, glm::vec3(0), glm::vec3{0, 1, 0});
    renderer.setSceneViewport(m_scenePosition, m_sceneSize);
    renderer::RenderAdapter adapter(renderer, camera);
    m_renderSystem.render(m_world, adapter, m_viewProjection);
    renderer.resetSceneViewport();
}

void NetworkArena::sampleTraffic(const net::TransportStats& stats) {
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - m_sampleTime).count();
    if (m_sampleTime == Clock::time_point{} || stats.txPackets < m_previousTraffic.txPackets ||
        stats.rxPackets < m_previousTraffic.rxPackets) {
        m_sampleTime = now;
        m_previousTraffic = stats;
        m_rates = {};
    } else if (seconds >= 0.75) {
        m_rates = {
            static_cast<double>(stats.txPackets - m_previousTraffic.txPackets) / seconds,
            static_cast<double>(stats.rxPackets - m_previousTraffic.rxPackets) / seconds,
            static_cast<double>(stats.txBytes - m_previousTraffic.txBytes) / seconds,
            static_cast<double>(stats.rxBytes - m_previousTraffic.rxBytes) / seconds
        };
        m_sampleTime = now;
        m_previousTraffic = stats;
    }
}

void NetworkArena::drawLabels(ImDrawList& drawList, ImVec2 viewportPosition) const {
    if (m_sceneSize.x <= 0.0F || m_sceneSize.y <= 0.0F) return;
    const ImVec2 origin{viewportPosition.x + m_scenePosition.x, viewportPosition.y + m_scenePosition.y};
    drawList.PushClipRect(origin, ImVec2(origin.x + m_sceneSize.x, origin.y + m_sceneSize.y), true);
    const auto label = [&](glm::vec3 position, std::string_view text, ImU32 color) {
        const auto clip = m_viewProjection * glm::vec4(position, 1.0F);
        if (clip.w <= 0.0F) return;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
        const ImVec2 min{
            origin.x + (ndc.x + 1.0F) * 0.5F * m_sceneSize.x - size.x * 0.5F,
            origin.y + (1.0F - ndc.y) * 0.5F * m_sceneSize.y - size.y * 0.5F
        };
        drawList.AddRectFilled({min.x - 5, min.y - 2}, {min.x + size.x + 5, min.y + size.y + 2}, IM_COL32(10, 17, 27, 232), 4.0F);
        drawList.AddText(min, color, text.data(), text.data() + text.size());
    };
    const auto* local = localPilot();
    const auto target = local ? nextArenaRelay(local->flags, local->value) : ArenaRelays.size();
    for (std::size_t i = 0; i < ArenaRelays.size(); ++i) {
        label(relayPosition(ArenaRelays[i]) + glm::vec3{0, 0.2F, 1.7F}, ArenaRelays[i].name,
            i == target ? IM_COL32(135, 248, 183, 255) : IM_COL32(173, 187, 204, 255));
    }
    for (const auto& pilot : m_presented) {
        const auto tint = pilotColor(pilot, m_client.localEntity());
        const auto title = (pilot.id == m_client.localEntity() ? "You  " : "Pilot " + std::to_string(pilot.id) + "  ") +
            std::to_string(pilot.value);
        label({pilot.x, pilot.y + 1.37F, pilot.z}, title,
            ImGui::ColorConvertFloat4ToU32({tint.r, tint.g, tint.b, 1.0F}));
    }
    drawList.PopClipRect();
}

void NetworkArena::renderUi(renderer::Renderer&) {
    const auto* viewport = ImGui::GetMainViewport();
    drawLabels(*ImGui::GetBackgroundDrawList(), viewport->Pos);
    const auto phase = m_client.phase();
    const bool connected = phase == net::ClientPhase::Connected;
    const bool connecting = phase == net::ClientPhase::Connecting;
    const auto stats = m_client.stats();
    sampleTraffic(stats.transport);
    ImGui::SetNextWindowPos({viewport->Pos.x + PanelMargin, viewport->Pos.y + PanelMargin});
    ImGui::SetNextWindowSize({panelWidth(viewport->Size.x), std::max(1.0F, viewport->Size.y - PanelMargin * 2.0F)});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14, 13});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.058F, 0.084F, 0.125F, 0.98F});
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs;
    if (ImGui::Begin("Relay Arena controls", nullptr, flags)) {
        ImGui::TextUnformatted("Relay Arena");
        ImGui::TextColored(connected ? TextGood : TextWarn, "%s", phaseName(phase));
        ImGui::SameLine();
        ImGui::TextColored(TextSecondary, "%s", m_endpointLabel.c_str());
        const auto diagnostic = m_client.diagnostic();
        if (!m_setupError.empty()) ImGui::TextWrapped("%s", m_setupError.c_str());
        else if (!diagnostic.empty() && diagnostic != phaseName(phase)) {
            ImGui::TextWrapped("%.*s", static_cast<int>(diagnostic.size()), diagnostic.data());
        }
        if (connected || connecting) {
            if (ImGui::Button("Disconnect")) {
                m_client.disconnect();
                m_linkCut = false;
                updateEmulation();
            }
            ImGui::SameLine();
            if (ImGui::Button(m_linkCut ? "Restore link" : "Cut link")) {
                m_linkCut = !m_linkCut;
                updateEmulation();
            }
        } else if (ImGui::Button("Connect")) {
            reconnect();
        }
        ImGui::Separator();
        if (const auto* local = localPilot(); local && connected) {
            const auto& relay = ArenaRelays[nextArenaRelay(local->flags, local->value)];
            const auto position = relayPosition(relay);
            const float distance = std::hypot(local->x - position.x, local->z - position.z);
            ImGui::Text("Pilot %u  |  Charge %u", local->id, static_cast<unsigned>(local->value));
            ImGui::TextColored(distance <= RelayRadius ? TextGood : TextSecondary, "%s  %.1f m", relay.name, distance);
            if (ImGui::Button("Charge (E)")) submitCapture(false);
            ImGui::SameLine();
            if (ImGui::Button("Invalid RPC (X)")) submitCapture(true);
        } else {
            ImGui::TextUnformatted("Waiting for a pilot");
        }
        ImGui::TextColored(TextSecondary, "WASD  Move     Esc  Close");
        if (!m_rpcFeedback.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, m_rpcColor);
            ImGui::TextWrapped("%s", m_rpcFeedback.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        ImGui::Text("RTT %u ms  |  Pilots in range %zu", stats.rttMs, m_presented.size());
        ImGui::Text("Send  %5.1f pkt/s  %6.0f B/s", m_rates.txPackets, m_rates.txBytes);
        ImGui::Text("Recv  %5.1f pkt/s  %6.0f B/s", m_rates.rxPackets, m_rates.rxBytes);
        ImGui::Text("Snapshots %llu  |  Delta %+.0f%%",
            static_cast<unsigned long long>(stats.snapshotsReceived), snapshotSavings(stats));
        ImGui::Text("Corrections %u  |  Last %.0f mm", stats.corrections, stats.lastCorrectionMm);
        ImGui::Text("Simulated drops %llu", static_cast<unsigned long long>(stats.transport.emulatedDrops));
        const auto queueDrops = stats.transport.sendQueueDrops + stats.transport.receiveQueueDrops;
        if (queueDrops || stats.invalidMessages || stats.transport.socketErrors) {
            ImGui::TextWrapped("Queue drops %llu  |  Invalid %u  |  Socket errors %llu",
                static_cast<unsigned long long>(queueDrops), stats.invalidMessages,
                static_cast<unsigned long long>(stats.transport.socketErrors));
        }
        ImGui::Separator();
        if (ImGui::Checkbox("Local prediction", &m_prediction)) m_client.setPrediction(m_prediction);
        if (ImGui::Checkbox("Remote interpolation", &m_interpolation)) m_client.setInterpolation(m_interpolation);
        ImGui::BeginDisabled(m_linkCut);
        ImGui::SetNextItemWidth(std::max(80.0F, ImGui::GetContentRegionAvail().x - 120.0F));
        if (ImGui::SliderInt("Outgoing delay", &m_latencyMs, 0, 450, "%d ms")) updateEmulation();
        ImGui::SetNextItemWidth(std::max(80.0F, ImGui::GetContentRegionAvail().x - 120.0F));
        if (ImGui::SliderInt("Outgoing loss", &m_lossPercent, 0, 75, "%d%%")) updateEmulation();
        ImGui::EndDisabled();
        if (m_linkCut) ImGui::TextColored(TextWarn, "Link cut  |  100%% outgoing loss");
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

}

std::unique_ptr<IGameState> makeNetworkArena(StateStack& stack, net::Endpoint server) {
    return std::make_unique<NetworkArena>(stack, server);
}

int runNetworkArenaServer(net::Endpoint bind) {
    NetworkArenaRules rules;
    net::NetworkServer server(rules, simulateArenaPlayer);
    net::ServerConfig config{};
    config.bind = bind;
    config.relevanceRadiusMm = ArenaRelevanceMm;
    std::string error;
    if (!server.start(config, error)) {
        std::cerr << "Relay Arena server failed: " << error << '\n';
        return 1;
    }
    serverStop.clear();
    const auto stop = [](int) { serverStop.test_and_set(std::memory_order_relaxed); };
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::cout << "Relay Arena listening on " << net::formatEndpoint(server.localEndpoint()) << " (UDP, Ctrl+C to stop)" << std::endl;
    const auto tickInterval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / net::TickRate));
    auto nextTick = Clock::now();
    auto nextReport = nextTick + 2s;
    std::size_t lastPeers{};
    while (!serverStop.test(std::memory_order_relaxed)) {
        server.tick();
        const auto now = Clock::now();
        const auto peers = server.peerCount();
        if (peers != lastPeers || (peers != 0 && now >= nextReport)) {
            const auto stats = server.stats();
            std::cout << "Pilots " << peers << " | Snapshots " << stats.snapshotsSent
                      << " | UDP sent " << stats.transport.txBytes << " B"
                      << " | UDP received " << stats.transport.rxBytes << " B"
                      << " | Invalid messages " << stats.invalidMessages << std::endl;
            lastPeers = peers;
            nextReport = now + 2s;
        }
        nextTick += tickInterval;
        if (nextTick + tickInterval * 3 < now) nextTick = now + tickInterval;
        std::this_thread::sleep_until(nextTick);
    }
    server.stop();
    std::cout << "Relay Arena server stopped." << std::endl;
    return 0;
}

}
