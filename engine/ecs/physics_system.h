#pragma once

#include <cstddef>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/core/EventBus.h"
#include "engine/ecs/collision_utils.h"

namespace engine::ecs {

class World;

struct CollisionEnterEvent final {
    CollisionContact contact;
};

struct CollisionStayEvent final {
    CollisionContact contact;
};

struct CollisionExitEvent final {
    CollisionContact contact;
};

class PhysicsSystem final {
public:
    struct Settings final {
        glm::vec3 gravity{0.0F, -9.81F, 0.0F};
        float maxLinearSpeed{80.0F};
        float maxAngularSpeed{18.0F};
        float broadphaseCellSize{2.6F};
        float sleepLinearSpeedThreshold{0.12F};
        float sleepAngularSpeedThreshold{0.18F};
        float sleepTimeThreshold{0.35F};
        std::size_t solverIterations{4};
    };

    PhysicsSystem() = default;
    explicit PhysicsSystem(Settings settings);

    void setSettings(const Settings& settings);
    [[nodiscard]] const Settings& settings() const;
    [[nodiscard]] std::size_t activeCollisionCount() const;
    [[nodiscard]] std::size_t bodyCount() const;
    [[nodiscard]] std::size_t broadphasePairCount() const;
    [[nodiscard]] std::size_t sleepingBodyCount() const;
    void clear();

    void update(World& world, double dt, core::EventBus& eventBus);
    struct CollisionPair final {
        EntityId first{kInvalidEntity};
        EntityId second{kInvalidEntity};

        [[nodiscard]] bool operator==(const CollisionPair& other) const {
            return first == other.first && second == other.second;
        }
    };

    struct CollisionPairHash final {
        std::size_t operator()(const CollisionPair& pair) const;
    };

private:
    Settings m_settings{};
    std::unordered_map<CollisionPair, CollisionContact, CollisionPairHash> m_activeContacts;
    std::size_t m_lastBodyCount{0};
    std::size_t m_lastBroadphasePairCount{0};
    std::size_t m_lastSleepingBodyCount{0};
};

} // namespace engine::ecs
