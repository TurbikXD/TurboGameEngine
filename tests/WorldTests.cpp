#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/JobSystem.h"
#include "engine/ecs/world.h"

namespace {

using engine::ecs::EntityId;
using engine::ecs::World;

struct Position final { int index{}; int value{}; };
struct Velocity final { int value{}; };
struct Marker final {};
struct Missing final {};

static_assert(!std::is_copy_constructible_v<World> && !std::is_copy_assignable_v<World>);
static_assert(!std::is_move_constructible_v<World> && !std::is_move_assignable_v<World>);
static_assert(std::is_same_v<EntityId, std::uint32_t> && engine::ecs::kInvalidEntity == 0U);

void require(const bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <class Exception, class Func>
void requireThrows(Func&& function, const char* message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

auto nativeSlot(const EntityId entity) {
    return entt::to_entity(static_cast<entt::entity>(entity - 1U));
}

void testLifecycleAndComponents() {
    World world;
    require(world.aliveCount() == 0U && !world.isAlive(engine::ecs::kInvalidEntity), "New world is not empty");
    const auto first = world.createEntity();
    const auto second = world.createEntity();
    require(first != engine::ecs::kInvalidEntity && first != second && world.aliveCount() == 2U,
            "Entity creation returned an invalid or duplicate handle");
    auto& initial = world.addComponent<Position>(first, Position{0, 12});
    require(initial.value == 12 && world.hasComponent<Position>(first), "Component insertion failed");
    auto& replacement = world.addComponent<Position>(first, Position{0, 31});
    require(replacement.value == 31 && world.getComponent<Position>(first)->value == 31,
            "Adding an existing component did not replace its value");
    std::size_t visits = 0;
    world.forEach<Position>([&](EntityId entity, Position&) {
        require(entity == first, "Component storage contains the wrong entity");
        ++visits;
    });
    require(visits == 1U, "Component replacement inserted a duplicate");
    world.removeComponent<Position>(first);
    world.removeComponent<Position>(first);
    require(!world.hasComponent<Position>(first) && world.getComponent<Position>(first) == nullptr,
            "Component removal failed");
    world.addComponent<Position>(first, Position{0, 19});
    world.destroyEntity(first);
    require(!world.isAlive(first) && world.aliveCount() == 1U && !world.hasComponent<Position>(first) &&
            world.getComponent<Position>(first) == nullptr, "Destroyed entity or its component survived");
    const auto recycled = world.createEntity();
    require(recycled != first && nativeSlot(recycled) == nativeSlot(first),
            "Recycled entity did not reuse its slot with a new generation");
    require(world.getComponent<Position>(recycled) == nullptr, "Recycled entity inherited an old component");
    world.addComponent<Position>(recycled, Position{2, 77});
    world.destroyEntity(first);
    world.removeComponent<Position>(first);
    requireThrows<std::invalid_argument>([&]() { world.addComponent<Position>(first); },
                                        "Adding a component to a stale handle was accepted");
    require(world.isAlive(recycled) && world.getComponent<Position>(recycled)->value == 77,
            "A stale handle changed the replacement entity");

    for (const auto invalid : {engine::ecs::kInvalidEntity, std::numeric_limits<EntityId>::max()}) {
        require(!world.isAlive(invalid) && !world.hasComponent<Position>(invalid) &&
                world.getComponent<Position>(invalid) == nullptr, "Invalid handle resolved to an entity");
        requireThrows<std::invalid_argument>([&]() { world.addComponent<Position>(invalid); },
                                            "Adding a component to an invalid handle was accepted");
        world.removeComponent<Position>(invalid);
        world.destroyEntity(invalid);
    }
    require(world.aliveCount() == 2U, "Invalid removals or destruction changed live entities");

    world.clear();
    require(world.aliveCount() == 0U && !world.isAlive(second) && !world.isAlive(recycled),
            "clear() kept live handles");
    const auto afterClear = world.createEntity();
    const auto anotherAfterClear = world.createEntity();
    require(afterClear != second && afterClear != recycled && anotherAfterClear != second &&
            anotherAfterClear != recycled && !world.isAlive(second) && !world.isAlive(recycled),
            "clear() allowed a stale handle to become valid again");
    world.destroyEntity(second);
    world.destroyEntity(recycled);
    require(world.isAlive(afterClear) && world.isAlive(anotherAfterClear),
            "Pre-clear handles destroyed new entities");
}

void testViewsAndEnumeration() {
    World world;
    const auto both = world.createEntity();
    const auto positionOnly = world.createEntity();
    const auto velocityOnly = world.createEntity();
    const auto empty = world.createEntity();
    const auto removed = world.createEntity();
    world.addComponent<Position>(both, Position{0, 10});
    world.addComponent<Velocity>(both, Velocity{4});
    world.addComponent<Marker>(both);
    world.addComponent<Position>(positionOnly, Position{1, 20});
    world.addComponent<Velocity>(velocityOnly, Velocity{8});
    world.addComponent<Position>(removed, Position{2, 30});
    world.destroyEntity(removed);

    std::size_t intersections = 0;
    world.forEach<Position, Velocity>([&](EntityId entity, Position& position, Velocity& velocity) {
        require(entity == both, "View intersection included an incomplete entity");
        position.value += velocity.value;
        ++intersections;
    });
    require(intersections == 1U && world.getComponent<Position>(both)->value == 14,
            "Mutable view did not update its component");
    world.forEach<Marker>([&](EntityId entity, Marker&) {
        require(entity == both, "Mutable empty-marker view yielded the wrong entity");
    });
    const World& readWorld = world;
    intersections = 0;
    readWorld.forEach<Position, Velocity, Marker>([&](EntityId entity, auto& position, auto& velocity, auto& marker) {
        static_assert(std::is_const_v<std::remove_reference_t<decltype(position)>>);
        static_assert(std::is_const_v<std::remove_reference_t<decltype(velocity)>>);
        static_assert(std::is_const_v<std::remove_reference_t<decltype(marker)>>);
        require(entity == both && position.value == 14 && velocity.value == 4,
                "Const view or empty marker references are incorrect");
        ++intersections;
    });
    require(intersections == 1U && readWorld.getComponent<Marker>(both) != nullptr,
            "Empty component was not retained in storage");

    {
        const auto phase = readWorld.stablePhase();
        require(world.getComponent<Missing>(both) == nullptr && readWorld.getComponent<Missing>(both) == nullptr &&
                !world.hasComponent<Missing>(both), "Reading a missing component returned a value");
        world.forEach<Missing>([](EntityId, Missing&) { throw std::runtime_error("Missing pool view visited an entity"); });
        readWorld.forEach<Position, Missing>([](EntityId, const Position&, const Missing&) {
            throw std::runtime_error("Missing intersection view visited an entity");
        });
    }
    world.addComponent<Missing>(both);
    require(world.hasComponent<Missing>(both), "Missing-pool reads left the world structurally locked");

    std::vector<EntityId> mutableIds;
    std::vector<EntityId> constIds;
    world.forEachEntity([&](EntityId entity) { mutableIds.push_back(entity); });
    readWorld.forEachEntity([&](EntityId entity) { constIds.push_back(entity); });
    std::vector<EntityId> expected{both, positionOnly, velocityOnly, empty};
    std::sort(expected.begin(), expected.end());
    std::sort(mutableIds.begin(), mutableIds.end());
    std::sort(constIds.begin(), constIds.end());
    require(mutableIds == expected && constIds == expected && world.aliveCount() == expected.size(),
            "Entity enumeration omitted live empty entities or included a destroyed handle");
}

void testStructuralGuards() {
    World world;
    const auto entity = world.createEntity();
    world.addComponent<Position>(entity, Position{0, 17});
    const auto checkBlocked = [&]() {
        requireThrows<std::logic_error>([&]() { world.createEntity(); }, "stablePhase allowed entity creation");
        requireThrows<std::logic_error>([&]() { world.destroyEntity(entity); }, "stablePhase allowed entity destruction");
        requireThrows<std::logic_error>([&]() { world.clear(); }, "stablePhase allowed clear");
        requireThrows<std::logic_error>([&]() { world.addComponent<Marker>(entity); }, "stablePhase allowed component insertion");
        requireThrows<std::logic_error>([&]() { world.addComponent<Position>(entity, Position{0, 99}); },
                                       "stablePhase allowed component replacement");
        requireThrows<std::logic_error>([&]() { world.removeComponent<Position>(entity); },
                                       "stablePhase allowed component removal");
    };
    {
        const auto outer = world.stablePhase();
        {
            const auto inner = world.stablePhase();
            checkBlocked();
        }
        checkBlocked();
        require(world.isAlive(entity) && world.getComponent<Position>(entity)->value == 17 &&
                !world.hasComponent<Marker>(entity), "Rejected structural operation changed the world");
    }
    requireThrows<std::runtime_error>([&]() {
        const auto phase = world.stablePhase();
        throw std::runtime_error("phase callback failure");
    }, "Fixture did not unwind a stable phase");
    world.addComponent<Marker>(entity);
    world.addComponent<Position>(entity, Position{0, 23});
    require(world.getComponent<Position>(entity)->value == 23, "Guard unwinding left structural changes blocked");

    std::array<bool, 9> rejected{};
    std::thread external([&]() {
        const auto check = [&](const std::size_t index, auto&& operation) {
            try { operation(); } catch (const std::logic_error&) { rejected[index] = true; }
        };
        check(0U, [&]() { world.createEntity(); });
        check(1U, [&]() { world.destroyEntity(entity); });
        check(2U, [&]() { world.clear(); });
        check(3U, [&]() { world.addComponent<Velocity>(entity); });
        check(4U, [&]() { world.addComponent<Position>(entity, Position{0, 99}); });
        check(5U, [&]() { world.removeComponent<Position>(entity); });
        check(6U, [&]() { const auto phase = world.stablePhase(); });
        check(7U, [&]() {
            world.forEachParallel<Position>(nullptr, 1U, [](EntityId, Position&) {});
        });
        check(8U, [&]() {
            std::as_const(world).forEachParallel<Position>(nullptr, 1U, [](EntityId, const Position&) {});
        });
    });
    external.join();
    require(std::all_of(rejected.begin(), rejected.end(), [](bool value) { return value; }),
            "External thread performed a structural operation or gathered a parallel view");
    require(world.isAlive(entity) && world.aliveCount() == 1U && world.getComponent<Position>(entity)->value == 23,
            "Rejected external operation changed world state");
    world.removeComponent<Marker>(entity);
    world.forEach<Position>([&](EntityId current, Position&) {
        requireThrows<std::logic_error>([&]() { world.removeComponent<Position>(current); },
                                       "Serial component iteration allowed storage removal");
    });
    world.forEachEntity([&](EntityId current) {
        requireThrows<std::logic_error>([&]() { world.destroyEntity(current); },
                                       "Entity iteration allowed structural destruction");
    });
    const World& readWorld = world;
    readWorld.forEach<Position>([&](EntityId current, const Position&) {
        requireThrows<std::logic_error>([&]() { world.addComponent<Position>(current); },
                                       "Const component iteration allowed storage replacement");
    });
    readWorld.forEachEntity([&](EntityId) {
        requireThrows<std::logic_error>([&]() { world.clear(); },
                                       "Const entity iteration allowed structural clear");
    });
    requireThrows<std::runtime_error>([&]() {
        world.forEach<Position>([](EntityId, Position&) { throw std::runtime_error("serial callback failure"); });
    }, "Serial callback exception was lost");
    world.addComponent<Marker>(entity);
}

void testParallelViews(engine::core::JobSystem* jobs) {
    constexpr std::size_t count = 513U;
    World emptyWorld;
    emptyWorld.forEachParallel<Position>(jobs, 0U, [](EntityId, Position&) {
        throw std::runtime_error("Empty world parallel view visited an entity");
    });
    World serialWorld;
    World parallelWorld;
    std::array<std::atomic_uint, count> visits{};
    std::vector<EntityId> serialIds;
    std::vector<EntityId> parallelIds;
    for (std::size_t index = 0; index < count; ++index) {
        const auto serial = serialWorld.createEntity();
        const auto parallel = parallelWorld.createEntity();
        const Position position{static_cast<int>(index), static_cast<int>(index) * 2};
        serialWorld.addComponent<Position>(serial, position);
        parallelWorld.addComponent<Position>(parallel, position);
        if (index % 3U != 0U) {
            const Velocity velocity{static_cast<int>(index) + 5};
            serialWorld.addComponent<Velocity>(serial, velocity);
            parallelWorld.addComponent<Velocity>(parallel, velocity);
        }
        if (index % 2U == 0U) { parallelWorld.addComponent<Marker>(parallel); }
        serialIds.push_back(serial);
        parallelIds.push_back(parallel);
    }
    serialWorld.forEach<Position, Velocity>([](EntityId, Position& position, Velocity& velocity) {
        position.value += velocity.value;
        velocity.value *= 2;
    });
    const auto submittedBefore = jobs != nullptr ? jobs->stats().submitted : 0U;
    parallelWorld.forEachParallel<Position, Velocity>(jobs, 7U,
        [&](EntityId entity, Position& position, Velocity& velocity) {
            require(parallelWorld.isAlive(entity), "Parallel view yielded a dead handle");
            require(parallelWorld.getComponent<Missing>(entity) == nullptr && !parallelWorld.hasComponent<Missing>(entity),
                    "Concurrent missing-pool reads returned a component");
            visits[static_cast<std::size_t>(position.index)].fetch_add(1U, std::memory_order_relaxed);
            position.value += velocity.value;
            velocity.value *= 2;
        });
    for (std::size_t index = 0; index < count; ++index) {
        const bool matches = index % 3U != 0U;
        require(visits[index].load(std::memory_order_relaxed) == (matches ? 1U : 0U),
                "Parallel intersection did not visit each matching entity exactly once");
        require(serialWorld.getComponent<Position>(serialIds[index])->value ==
                parallelWorld.getComponent<Position>(parallelIds[index])->value,
                "Parallel component writes differ from serial values");
        if (matches) {
            require(serialWorld.getComponent<Velocity>(serialIds[index])->value ==
                    parallelWorld.getComponent<Velocity>(parallelIds[index])->value,
                    "Parallel secondary component writes differ from serial values");
        }
    }
    if (jobs != nullptr && jobs->enabled()) {
        require(jobs->stats().submitted > submittedBefore, "Enabled parallel view did not submit any jobs");
    }
    const World& readWorld = parallelWorld;
    std::atomic_uint markerVisits{};
    readWorld.forEachParallel<Position, Marker>(jobs, 7U, [&](EntityId entity, auto& position, auto& marker) {
        static_assert(std::is_const_v<std::remove_reference_t<decltype(position)>>);
        static_assert(std::is_const_v<std::remove_reference_t<decltype(marker)>>);
        require(entity == parallelIds[static_cast<std::size_t>(position.index)] && position.index % 2 == 0,
                "Const parallel marker view yielded the wrong entity");
        markerVisits.fetch_add(1U, std::memory_order_relaxed);
    });
    require(markerVisits == (count + 1U) / 2U, "Const parallel view omitted empty markers");
    readWorld.forEachParallel<Missing>(jobs, 1U, [](EntityId, const Missing&) {
        throw std::runtime_error("Missing parallel pool visited an entity");
    });

    std::atomic_uint rejected{};
    parallelWorld.forEachParallel<Position>(jobs, 7U, [&](EntityId entity, Position&) {
        try { parallelWorld.addComponent<Position>(entity); }
        catch (const std::logic_error&) { rejected.fetch_add(1U, std::memory_order_relaxed); }
    });
    require(rejected == count, "Parallel callback was allowed to replace storage");
    parallelWorld.addComponent<Missing>(parallelIds.front());
    require(parallelWorld.hasComponent<Missing>(parallelIds.front()), "Synchronous join did not release the phase guard");

    std::atomic_uint inFlight{};
    requireThrows<std::runtime_error>([&]() {
        parallelWorld.forEachParallel<Position>(jobs, 7U, [&](EntityId, Position& position) {
            struct InFlight final {
                std::atomic_uint& count;
                explicit InFlight(std::atomic_uint& value) : count(value) { ++count; }
                ~InFlight() { --count; }
            } running(inFlight);
            if (position.index == 0) { throw std::runtime_error("parallel callback failure"); }
        });
    }, "Parallel callback exception was lost");
    require(inFlight == 0U, "Exception returned while callbacks were still active");
    if (jobs != nullptr) {
        require(jobs->stats().pending == 0U && jobs->stats().running == 0U,
                "Parallel exception escaped before all submitted jobs joined");
    }
    parallelWorld.removeComponent<Missing>(parallelIds.front());
    const auto afterFailure = parallelWorld.createEntity();
    require(parallelWorld.isAlive(afterFailure), "Parallel exception left structural changes blocked");
}

} // namespace

int main() {
    try {
        testLifecycleAndComponents();
        testViewsAndEnumeration();
        testStructuralGuards();
        testParallelViews(nullptr);
        engine::core::JobSystem disabled(false);
        testParallelViews(&disabled);
        engine::core::JobSystem enabled(true, 3U);
        testParallelViews(&enabled);
    } catch (const std::exception& error) {
        std::cerr << "World test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "World: generational handles, components, views, stable phases and joined parallel iteration passed\n";
    return EXIT_SUCCESS;
}
