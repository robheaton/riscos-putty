#!/bin/sh
# Copy the built tools (and the test client key, if there is one) into the
# emulator's HostFS:  guest path HostFS::HostFS.$.PuttyDev
set -eu
cd "$(dirname "$0")/.."
HOSTFS="${RPCEMU_HOSTFS:-$HOME/RPCEmu/machines/RISC OS 5/hostfs}"
DEST="$HOSTFS/PuttyDev"
mkdir -p "$DEST"
cp build/ro/plink,ff8 "$DEST/"
[ -f build/test/client.ppk ] && cp build/test/client.ppk "$DEST/ClientKey"
echo "deployed to $DEST"
