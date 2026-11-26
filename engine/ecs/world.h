#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/core/JobSystem.h"
#include "engine/ecs/entity.h"

namespace engine::ecs {

class World final {
public:
    World() = default;
    ~World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    class StablePhase final {
    public:
        ~StablePhase();
        StablePhase(const StablePhase&) = delete;
        StablePhase& operator=(const StablePhase&) = delete;
        StablePhase(StablePhase&&) = delete;
        StablePhase& operator=(StablePhase&&) = delete;

    private:
        friend class World;
        explicit StablePhase(const World& world, bool requireOwner = true);
        const World& m_world;
    };

    [[nodiscard]] StablePhase stablePhase() const;

    EntityId createEntity();
    void destroyEntity(EntityId entity);
    void clear();

    [[nodiscard]] bool isAlive(EntityId entity) const;
    [[nodiscard]] std::size_t aliveCount() const;

    template <class T, class... Args>
    T& addComponent(EntityId entity, Args&&... args);
    template <class T>
    [[nodiscard]] bool hasComponent(EntityId entity) const;
    template <class T>
    void removeComponent(EntityId entity);
    template <class T>
    T* getComponent(EntityId entity);
    template <class T>
    const T* getComponent(EntityId entity) const;

    template <class... Components, class Func>
    void forEach(Func&& func);
    template <class... Components, class Func>
    void forEach(Func&& func) const;
    template <class Func>
    void forEachEntity(Func&& func);
    template <class Func>
    void forEachEntity(Func&& func) const;

    template <class... Components, class Func>
    void forEachParallel(core::JobSystem* jobs, std::size_t minimumItemsPerJob, Func&& func);
    template <class... Components, class Func>
    void forEachParallel(core::JobSystem* jobs, std::size_t minimumItemsPerJob, Func&& func) const;

private:
    static entt::entity toNative(EntityId entity) {
        return entity == kInvalidEntity ? entt::null : static_cast<entt::entity>(entity - 1U);
    }
    static EntityId fromNative(entt::entity entity) {
        return entt::to_integral(entity) + 1U;
    }
    void requireOwnerThread() const;
    void requireReadAccess() const;
    void requireStructuralChange() const;

    template <class... Components, class Self, class Func>
    static void parallelEach(Self& world, core::JobSystem* jobs, std::size_t grain, Func&& func);

    entt::registry m_registry;
    const std::thread::id m_ownerThread{std::this_thread::get_id()};
    mutable std::atomic_size_t m_stablePhases{0U};
};

template <class T, class... Args>
T& World::addComponent(const EntityId entity, Args&&... args) {
    requireStructuralChange();
    if (!isAlive(entity)) {
        throw std::invalid_argument("Cannot add a component to a dead or invalid entity");
    }
    return m_registry.emplace_or_replace<T>(toNative(entity), std::forward<Args>(args)...);
}

template <class T>
bool World::hasComponent(const EntityId entity) const {
    return getComponent<T>(entity) != nullptr;
}

template <class T>
void World::removeComponent(const EntityId entity) {
    requireStructuralChange();
    if (isAlive(entity)) {
        if (const auto* storage = std::as_const(m_registry).storage<T>(); storage != nullptr) {
            const_cast<entt::registry::storage_for_type<T>*>(storage)->remove(toNative(entity));
        }
    }
}

template <class T>
T* World::getComponent(const EntityId entity) {
    return isAlive(entity) ? m_registry.try_get<T>(toNative(entity)) : nullptr;
}

template <class T>
const T* World::getComponent(const EntityId entity) const {
    return isAlive(entity) ? m_registry.try_get<T>(toNative(entity)) : nullptr;
}

template <class... Components, class Func>
void World::forEach(Func&& func) {
    static_assert(sizeof...(Components) > 0, "forEach requires component types");
    const auto phase = stablePhase();
    entt::basic_view<entt::get_t<entt::registry::storage_for_type<Components>...>, entt::exclude_t<>> view;
    [&view](const auto*... storage) {
        ((storage ? view.storage(*const_cast<entt::registry::storage_for_type<Components>*>(storage)) : void()), ...);
    }(std::as_const(m_registry).storage<std::remove_const_t<Components>>()...);
    for (const auto entity : view) {
        std::invoke(func, fromNative(entity), view.template get<Components>(entity)...);
    }
}

template <class... Components, class Func>
void World::forEach(Func&& func) const {
    static_assert(sizeof...(Components) > 0, "forEach requires component types");
    const StablePhase phase(*this, false);
    const auto view = m_registry.view<Components...>();
    for (const auto entity : view) {
        std::invoke(func, fromNative(entity), view.template get<Components>(entity)...);
    }
}

template <class Func>
void World::forEachEntity(Func&& func) {
    const auto phase = stablePhase();
    std::as_const(*this).forEachEntity(std::forward<Func>(func));
}

template <class Func>
void World::forEachEntity(Func&& func) const {
    const StablePhase phase(*this, false);
    for (const auto [entity] : m_registry.storage<entt::entity>()->each()) {
        std::invoke(func, fromNative(entity));
    }
}

template <class... Components, class Self, class Func>
void World::parallelEach(Self& world, core::JobSystem* const jobs, const std::size_t grain, Func&& func) {
    static_assert(sizeof...(Components) > 0, "forEachParallel requires component types");
    const auto phase = world.stablePhase();
    using Item = std::tuple<EntityId, std::conditional_t<std::is_const_v<Self>, const Components*, Components*>...>;
    std::vector<Item> items;
    items.reserve(world.aliveCount());
    world.template forEach<Components...>([&](EntityId entity, auto&... components) {
        items.emplace_back(entity, std::addressof(components)...);
    });
    const auto range = [&](const std::size_t begin, const std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            std::apply([&](EntityId entity, auto*... components) {
                std::invoke(func, entity, *components...);
            }, items[index]);
        }
    };
    if (jobs != nullptr) {
        jobs->parallelFor(items.size(), grain, range);
    } else {
        range(0U, items.size());
    }
}

template <class... Components, class Func>
void World::forEachParallel(core::JobSystem* jobs, std::size_t minimumItemsPerJob, Func&& func) {
    parallelEach<Components...>(*this, jobs, minimumItemsPerJob, std::forward<Func>(func));
}

template <class... Components, class Func>
void World::forEachParallel(core::JobSystem* jobs, std::size_t minimumItemsPerJob, Func&& func) const {
    parallelEach<Components...>(*this, jobs, minimumItemsPerJob, std::forward<Func>(func));
}

}
