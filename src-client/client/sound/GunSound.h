#pragma once

#include <string>

#include "pack/ResourceId.h"

// Plays TACZ gun pack sounds (assets/<ns>/tacz_sounds/<path>.ogg) through the game's sound
// engine straight from memory; no resource pack involved. Call on the client thread.
namespace tacz::client::sound {

// Plays a sound id such as tacz:ak47/ak47_shoot, non-positional. False if the file is missing.
bool play(pack::ResourceId const& soundId, float volume = 1.0f, float pitch = 1.0f, bool animation = false);

// Plays the gun display's sound for `key` ("shoot", "reload_tactical", ...).
bool playGunSound(pack::ResourceId const& gunId, std::string const& key, float volume = 1.0f, float pitch = 1.0f, bool animation = false);

// Only our tracked draw/reload/inspect/keyframe sounds, never shots or unrelated game audio.
void stopAnimation();

// Drops cached sound data (gun pack reload, mod disable).
void clear();

} // namespace tacz::client::sound
