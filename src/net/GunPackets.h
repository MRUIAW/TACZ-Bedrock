#pragma once

#include <cstdint>

#include "gun/SyncedShooterData.h"

#include "ll/api/network/packet/Packet.h"

#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"

// Gun action packets, mirroring TACZ's ClientMessagePlayerShoot / ClientMessagePlayerFireSelect.
namespace tacz::net {

// ClientMessagePlayerAim: state only, never client-supplied progress/duration/inaccuracy.
class AimPacket final : public ll::network::PacketBase<AimPacket> {
public:
    bool aiming = false;
    AimPacket() = default;
    explicit AimPacket(bool value) : aiming(value) {}
    void write(BinaryStream& stream) const override { stream.writeBool(aiming, nullptr, nullptr); }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto value = stream.getBool();
        if (!value) return nonstd::make_unexpected(value.error());
        aiming = *value;
        return {};
    }
};

// Bedrock transport counterpart of Java's vanilla STOP_SPRINTING command.
// Only held ShootKey.stopSprint intent, never sprint time, shot permission or gun data.
class ShootSprintPacket final : public ll::network::PacketBase<ShootSprintPacket> {
public:
    bool held = false;
    ShootSprintPacket() = default;
    explicit ShootSprintPacket(bool value) : held(value) {}
    void write(BinaryStream& stream) const override { stream.writeBool(held, nullptr, nullptr); }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto value = stream.getBool();
        if (!value) return nonstd::make_unexpected(value.error());
        held = *value;
        return {};
    }
};

// Selection notification only. The server reads its own selected ItemStack and never trusts
// a client-supplied gun id, duration or clock. Tick/action observations are the fallback.
class DrawPacket final : public ll::network::PacketBase<DrawPacket> {
public:
    void write(BinaryStream&) const override {}
    Bedrock::Result<void> read(ReadOnlyBinaryStream&) override { return {}; }
};

// Client -> server: the trigger was pulled. `timestamp` is the client's own clock in ms; the
// server compares it with the previous shot for the rate limit, as TACZ does.
class ShootPacket final : public ll::network::PacketBase<ShootPacket> {
public:
    int64_t timestamp = 0;

    ShootPacket() = default;
    explicit ShootPacket(int64_t ts) : timestamp(ts) {}

    void write(BinaryStream& stream) const override { stream.writeSignedInt64(timestamp, nullptr, nullptr); }

    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto ts = stream.getSignedInt64();
        if (!ts) return nonstd::make_unexpected(ts.error());
        timestamp = *ts;
        return {};
    }
};

// Client -> server: switch to the gun's next fire mode.
class FireSelectPacket final : public ll::network::PacketBase<FireSelectPacket> {
public:
    void                  write(BinaryStream&) const override {}
    Bedrock::Result<void> read(ReadOnlyBinaryStream&) override { return {}; }
};

// Client -> server: bolt intent only (ClientMessagePlayerBoltGun).
class BoltPacket final : public ll::network::PacketBase<BoltPacket> {
public:
    void write(BinaryStream&) const override {}
    Bedrock::Result<void> read(ReadOnlyBinaryStream&) override { return {}; }
};

// Client -> server: start reloading (ClientMessagePlayerReloadGun).
class ReloadPacket final : public ll::network::PacketBase<ReloadPacket> {
public:
    void                  write(BinaryStream&) const override {}
    Bedrock::Result<void> read(ReadOnlyBinaryStream&) override { return {}; }
};

// Client -> server: interrupt the reload (ClientMessagePlayerCancelReload).
class CancelReloadPacket final : public ll::network::PacketBase<CancelReloadPacket> {
public:
    void                  write(BinaryStream&) const override {}
    Bedrock::Result<void> read(ReadOnlyBinaryStream&) override { return {}; }
};

// Server -> entity owner and trackers. Pure codec is shared by both variants/tests.
// Bounded raw payloads reach the handler for pure-codec validation; malformed
// payloads are dropped there, never exposed as partial entity state.
class SyncedShooterDataPacket final : public ll::network::PacketBase<SyncedShooterDataPacket> {
public:
    std::string payload;
    gun::sync::DecodeError transportError = gun::sync::DecodeError::None;
    SyncedShooterDataPacket() = default;
    explicit SyncedShooterDataPacket(gun::sync::Message const& message) : payload(gun::sync::encode(message)) {}
    void write(BinaryStream& stream) const override { stream.write(payload.data(), payload.size()); }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        payload.clear();
        transportError = gun::sync::DecodeError::None;
        if (stream.mHasOverflowed || stream.mReadPointer > stream.mView.size()) {
            transportError = gun::sync::DecodeError::Truncated;
        } else {
            auto remaining = stream.mView.substr(static_cast<size_t>(stream.mReadPointer));
            if (remaining.size() > gun::sync::MaxPayloadSize) transportError = gun::sync::DecodeError::Oversize;
            else payload.assign(remaining);
        }
        stream.mReadPointer = stream.mView.size();
        return {};
    }
};

} // namespace tacz::net
