#include "engine/ecs/world.h"

namespace engine::ecs {

World::StablePhase::StablePhase(const World& world, const bool requireOwner) : m_world(world) {
    if (requireOwner) {
        world.requireOwnerThread();
    }
    if (std::this_thread::get_id() == world.m_ownerThread) {
        world.m_stablePhases.fetch_add(1U, std::memory_order_acq_rel);
        return;
    }
    auto count = world.m_stablePhases.load(std::memory_order_acquire);
    while (count != 0U) {
        if (world.m_stablePhases.compare_exchange_weak(count, count + 1U,
                std::memory_order_acq_rel, std::memory_order_acquire)) return;
    }
    throw std::logic_error("World worker reads require an owner-held stable phase");
}

World::StablePhase::~StablePhase() {
    m_world.m_stablePhases.fetch_sub(1U, std::memory_order_release);
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
    requireOwnerThread();
    if (m_stablePhases.load(std::memory_order_acquire) != 0U) {
        throw std::logic_error("World structure is frozen until iteration/jobs finish");
    }
}

void World::requireReadAccess() const {
    if (std::this_thread::get_id() != m_ownerThread &&
        m_stablePhases.load(std::memory_order_acquire) == 0U) {
        throw std::logic_error("World worker reads require an owner-held stable phase");
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
    m_registry.clear();
}

bool World::isAlive(const EntityId entity) const {
    requireReadAccess();
    return entity != kInvalidEntity && m_registry.valid(toNative(entity));
}

std::size_t World::aliveCount() const {
    requireReadAccess();
    return m_registry.storage<entt::entity>()->free_list();
}

}
