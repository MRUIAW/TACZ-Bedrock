#pragma once
class Player;
namespace tacz::nbt {
// Server thread only, immediately after our mutation, before native pickup.
// Own InventoryMenu is explicitly suppressed by ServerPlayer::slotChanged.
// Pair a single-slot packet with the same-slot baseline commit, not a full resend.
void sendTrackedInventorySlot(Player& player,int slot,int before,char const* reason);
// For writeSelectedGun's user-verified explicit packet, after sendNetworkPacket.
void recordSentInventorySlot(Player& player,int slot,int before,char const* reason);
void enableInventorySyncDiagnostics();
void disableInventorySyncDiagnostics();
}
