#pragma once

#include <cstdint>

#include "ll/api/network/packet/Packet.h"

#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"

// Gun action packets, mirroring TACZ's ClientMessagePlayerShoot / ClientMessagePlayerFireSelect.
namespace tacz::net {

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

// Server -> client: the shooter's reload state (TACZ syncs it as entity data, RELOAD_STATE_KEY).
// Sent on every state change, and in reply to a refused reload request.
class ReloadStatePacket final : public ll::network::PacketBase<ReloadStatePacket> {
public:
    int     state     = 0; // ReloadStateType ordinal
    int64_t countDown = -1;

    ReloadStatePacket() = default;
    ReloadStatePacket(int s, int64_t c) : state(s), countDown(c) {}

    void write(BinaryStream& stream) const override {
        stream.writeVarInt(state, nullptr, nullptr);
        stream.writeSignedInt64(countDown, nullptr, nullptr);
    }

    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto s = stream.getVarInt();
        if (!s) return nonstd::make_unexpected(s.error());
        auto c = stream.getSignedInt64();
        if (!c) return nonstd::make_unexpected(c.error());
        state     = *s;
        countDown = *c;
        return {};
    }
};

} // namespace tacz::net
