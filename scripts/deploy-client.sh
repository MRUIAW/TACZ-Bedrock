#!/usr/bin/env bash
# Builds the client variant and copies it into a LeviLauncher game version's mods folder.
# Refuses to deploy unless the build succeeded and the packed manifest says "client".
#
# Usage: scripts/deploy-client.sh [mods/tacz directory]
set -euo pipefail

cd "$(dirname "$0")/.."
DEST="${1:-/f/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz}"

xmake f -a x64 -m release -p windows --target_type=client -y >/dev/null
if ! xmake build tacz; then
    echo "deploy: client build failed, nothing copied" >&2
    exit 1
fi
if ! grep -q '"platform": "client"' bin/tacz/manifest.json; then
    echo "deploy: bin/tacz is not a client build, nothing copied" >&2
    exit 1
fi
if tasklist 2>/dev/null | grep -qi "Minecraft.Windows"; then
    echo "deploy: the game is running, close it first" >&2
    exit 1
fi
mkdir -p "$DEST"
cp bin/tacz/manifest.json bin/tacz/tacz.dll bin/tacz/tacz.pdb "$DEST/"
cmp -s bin/tacz/tacz.dll "$DEST/tacz.dll"
# LeviLamina loads <mod>/resource_packs and <mod>/behavior_packs at game start (ll/core/main_win.cpp).
# Replace our own packs wholesale so files deleted from the repo do not linger.
for kind in resource_packs behavior_packs; do
    for pack in packs/"$kind"/*/; do
        name=$(basename "$pack")
        rm -rf "${DEST:?}/$kind/$name"
        mkdir -p "$DEST/$kind"
        cp -r "$pack" "$DEST/$kind/$name"
    done
done
echo "deploy: $(grep '"version"' bin/tacz/manifest.json | tr -d ' ,') client deployed to $DEST"
