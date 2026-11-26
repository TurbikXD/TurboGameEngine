#pragma once

namespace engine::core { struct PlayLaunch; }
namespace engine::net { class SteamInvites; }

namespace engine::game {

int runNetworkSceneServer(const core::PlayLaunch& launch, net::SteamInvites* steam = nullptr);

}
