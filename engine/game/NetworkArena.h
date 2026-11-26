#pragma once

#include <memory>

#include "engine/network/NetworkSession.h"

namespace engine::game {

class IGameState;
class StateStack;

std::unique_ptr<IGameState> makeNetworkArena(StateStack& stack, net::Endpoint server);
int runNetworkArenaServer(net::Endpoint bind);

}
