#pragma once

#include <cstdint>
#include <memory>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/ecs/entity.h"

namespace engine::ecs {

class World final {
public:
    World() = default;
    ~World() = default;

    EntityId createEntity();
    void destroyEntity(EntityId entity);
    void clear();

    [[nodiscard]] bool isAlive(EntityId entity) const;

    template <class T, class... Args>
    T& addComponent(EntityId entity, Args&&... args);

    template <class T>
    [[nodiscard]] bool hasComponent(EntityId entity) const;

    template <class T>
    T* getComponent(EntityId entity);

    template <class T>
    const T* getComponent(EntityId entity) const;

    template <class... Components, class Func>
    void forEach(Func&& func);

private:
    struct EntityRecord final {
        std::uint32_t generation{1};
        bool alive{false};
    };

    struct IComponentStorage {
        virtual ~IComponentStorage() = default;
        virtual void remove(EntityId entity) = 0;
        virtual void clear() = 0;
    };

    template <class T>
    struct ComponentStorage final : IComponentStorage {
        std::unordered_map<EntityId, T> components;

        void remove(EntityId entity) override {
            components.erase(entity);
        }

        void clear() override {
            components.clear();
        }
    };

    template <class T>
    ComponentStorage<T>* ensureStorage();

    template <class T>
    ComponentStorage<T>* getStorage();

    template <class T>
    const ComponentStorage<T>* getStorage() const;

    [[nodiscard]] bool isValidEntityIndex(EntityId entity) const;

    std::vector<EntityRecord> m_entities;
    std::vector<EntityId> m_freeList;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentStorage>> m_componentStores;
};

template <class T, class... Args>
T& World::addComponent(EntityId entity, Args&&... args) {
    auto* storage = ensureStorage<T>();
    auto [it, inserted] = storage->components.try_emplace(entity, std::forward<Args>(args)...);
    if (!inserted) {
        it->second = T(std::forward<Args>(args)...);
    }
    return it->second;
}

template <class T>
bool World::hasComponent(EntityId entity) const {
    if (!isAlive(entity)) {
        return false;
    }

    const auto* storage = getStorage<T>();
    if (storage == nullptr) {
        return false;
    }
    return storage->components.find(entity) != storage->components.end();
}

template <class T>
T* World::getComponent(EntityId entity) {
    auto* storage = getStorage<T>();
    if (storage == nullptr) {
        return nullptr;
    }

    const auto it = storage->components.find(entity);
    if (it == storage->components.end()) {
        return nullptr;
    }
    return &it->second;
}

template <class T>
const T* World::getComponent(EntityId entity) const {
    const auto* storage = getStorage<T>();
    if (storage == nullptr) {
        return nullptr;
    }

    const auto it = storage->components.find(entity);
    if (it == storage->components.end()) {
        return nullptr;
    }
    return &it->second;
}

template <class... Components, class Func>
void World::forEach(Func&& func) {
    static_assert(sizeof...(Components) > 0, "forEach requires at least one component type");

    using FirstComponent = std::tuple_element_t<0, std::tuple<Components...>>;
    auto* primaryStorage = getStorage<FirstComponent>();
    if (primaryStorage == nullptr) {
        return;
    }

    for (auto& [entity, ignored] : primaryStorage->components) {
        (void)ignored;
        if (!isAlive(entity)) {
            continue;
        }
        if (!(hasComponent<Components>(entity) && ...)) {
            continue;
        }
        func(entity, *getComponent<Components>(entity)...);
    }
}

template <class T>
World::ComponentStorage<T>* World::ensureStorage() {
    const std::type_index key(typeid(T));
    const auto it = m_componentStores.find(key);
    if (it != m_componentStores.end()) {
        return static_cast<ComponentStorage<T>*>(it->second.get());
    }

    auto storage = std::make_unique<ComponentStorage<T>>();
    auto* rawStorage = storage.get();
    m_componentStores.emplace(key, std::move(storage));
    return rawStorage;
}

template <class T>
World::ComponentStorage<T>* World::getStorage() {
    const std::type_index key(typeid(T));
    const auto it = m_componentStores.find(key);
    if (it == m_componentStores.end()) {
        return nullptr;
    }
    return static_cast<ComponentStorage<T>*>(it->second.get());
}

template <class T>
const World::ComponentStorage<T>* World::getStorage() const {
    const std::type_index key(typeid(T));
    const auto it = m_componentStores.find(key);
    if (it == m_componentStores.end()) {
        return nullptr;
    }
    return static_cast<const ComponentStorage<T>*>(it->second.get());
}

} // namespace engine::ecs
