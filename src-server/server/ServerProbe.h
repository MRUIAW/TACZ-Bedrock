#pragma once

// Technical-validation probe, server side: custom packet receive/reply,
// item NBT write/read, and the /tacz debug command. Runs on BDS and on the
// client-hosted server of single-player worlds.
namespace tacz::server::probe {

void enable();
void disable();

} // namespace tacz::server::probe
