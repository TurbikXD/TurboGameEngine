#include "engine/ecs/world.h"

namespace engine::ecs {

EntityId World::createEntity() {
    if (!m_freeList.empty()) {
        const EntityId recycled = m_freeList.back();
        m_freeList.pop_back();
        if (isValidEntityIndex(recycled)) {
            auto& record = m_entities[recycled - 1];
            record.alive = true;
            return recycled;
        }
    }

    m_entities.push_back(EntityRecord{});
    m_entities.back().alive = true;
    return static_cast<EntityId>(m_entities.size());
}

void World::destroyEntity(EntityId entity) {
    if (!isAlive(entity)) {
        return;
    }

    auto& record = m_entities[entity - 1];
    record.alive = false;
    ++record.generation;
    m_freeList.push_back(entity);

    for (auto& [type, storage] : m_componentStores) {
        (void)type;
        storage->remove(entity);
    }
}

void World::clear() {
    for (auto& [type, storage] : m_componentStores) {
        (void)type;
        storage->clear();
    }
    m_entities.clear();
    m_freeList.clear();
}

bool World::isAlive(EntityId entity) const {
    if (!isValidEntityIndex(entity)) {
        return false;
    }
    return m_entities[entity - 1].alive;
}

bool World::isValidEntityIndex(EntityId entity) const {
    return entity > 0 && static_cast<std::size_t>(entity - 1) < m_entities.size();
}

} // namespace engine::ecs
