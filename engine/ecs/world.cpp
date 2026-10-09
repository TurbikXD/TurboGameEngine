#include "engine/ecs/world.h"

namespace engine::ecs {

World::StablePhase::StablePhase(const World& world, const bool requireOwner) : m_world(world) {
    if (requireOwner) {
        world.requireOwnerThread();
    }
    m_ownsPhase = std::this_thread::get_id() == world.m_ownerThread;
    if (m_ownsPhase) {
        ++world.m_stablePhases;
    }
}

World::StablePhase::~StablePhase() {
    if (m_ownsPhase) {
        --m_world.m_stablePhases;
    }
}

World::StablePhase World::stablePhase() const {
    return StablePhase(*this);
}

void World::requireOwnerThread() const {
    if (std::this_thread::get_id() != m_ownerThread) {
        throw std::logic_error("World structural operations and job gathering require its owner thread");
    }
}

void World::requireStructuralChange() const {
    // Check thread ownership first; workers must never read the owner's phase counter.
    requireOwnerThread();
    if (m_stablePhases != 0U) {
        throw std::logic_error("World structure is frozen until iteration/jobs finish");
    }
}

EntityId World::createEntity() {
    requireStructuralChange();
    return fromNative(m_registry.create());
}

void World::destroyEntity(const EntityId entity) {
    requireStructuralChange();
    if (isAlive(entity)) {
        m_registry.destroy(toNative(entity));
    }
}

void World::clear() {
    requireStructuralChange();
    // Keep entity storage so its versions invalidate handles across scene resets.
    m_registry.clear();
}

bool World::isAlive(const EntityId entity) const {
    return entity != kInvalidEntity && m_registry.valid(toNative(entity));
}

std::size_t World::aliveCount() const {
    return m_registry.storage<entt::entity>()->free_list();
}

} // namespace engine::ecs
