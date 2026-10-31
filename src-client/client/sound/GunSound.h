#pragma once

#include <string>

#include "pack/ResourceId.h"
#include "sound/SoundConcurrency.h"

// Plays TACZ gun pack sounds (assets/<ns>/tacz_sounds/<path>.ogg) through the game's sound
// engine straight from memory; no resource pack involved. Call on the client thread.
namespace tacz::client::sound {

using Category = tacz::sound::Category;

// Plays a sound id such as tacz:ak47/ak47_shoot, non-positional. False if the file is missing.
// temporaryBase is only reload/inspect/bolt/draw/put-away, never keyframes.
bool play(pack::ResourceId const& soundId, float volume = 1.0f, float pitch = 1.0f, Category category = Category::Default, bool temporaryBase = false);

// Plays the gun display's sound for `key` ("shoot", "reload_tactical", ...).
bool playGunSound(pack::ResourceId const& gunId, std::string const& key, float volume = 1.0f, float pitch = 1.0f, bool temporaryBase = false);

// Java tmpSoundInstance; optional filter matches the logical sound id, not an engine alias.
void stopTemporary();
void stopTemporary(pack::ResourceId const& expectedSound);
// World leave/disable: stop every voice we track, including shots and keyframes.
void stopAllTracked();

// Drops cached sound data (gun pack reload, mod disable).
void clear();

// END-of-client-level-tick cleanup hook, separate from render/frame playback.
void enable();
void disable();
bool endTickAvailable(); // startup capability only; safe without a world

} // namespace tacz::client::sound
