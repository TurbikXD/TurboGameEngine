#pragma once

#include <cstdint>

namespace engine::net {

inline constexpr std::uint32_t MaxReplicaKey = 0x7fffffffU;

struct NetworkReplica final {
    std::uint32_t key{1};
};

}
