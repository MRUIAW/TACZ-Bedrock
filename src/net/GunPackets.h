#pragma once

#include <cstdint>

#include "ll/api/network/packet/Packet.h"

#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"

// Gun action packets, mirroring TACZ's ClientMessagePlayerShoot / ClientMessagePlayerFireSelect.
namespace tacz::net {

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

} // namespace tacz::net
