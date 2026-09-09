#pragma once
#include "../common/ipc_protocol.hpp"
namespace k2vr::game32 {
// Called outside DllMain, only after exact supported game-build validation.
bool InstallBinkMovieHooks(ipc::SessionNonce nonce) noexcept;
}
