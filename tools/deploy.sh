#!/bin/sh
# Copy the built tools (and the test client key, if there is one) into the
# emulator's HostFS:  guest path HostFS::HostFS.$.PuttyDev
set -eu
cd "$(dirname "$0")/.."
HOSTFS="${RPCEMU_HOSTFS:-$HOME/RPCEmu/machines/RISC OS 5/hostfs}"
DEST="$HOSTFS/PuttyDev"
mkdir -p "$DEST"
cp build/ro/plink,ff8 "$DEST/"
[ -f build/ro/putty,ff8 ] && cp build/ro/putty,ff8 "$DEST/"
# the diagnostics (make tests), if built
for f in build/ro/*,ff8; do cp "$f" "$DEST/"; done
[ -f build/test/client.ppk ] && cp build/test/client.ppk "$DEST/ClientKey"
# the application directory (make app), to be run with Filer_Run
if [ -d 'build/app/!PuTTY' ]; then
    rm -rf "$DEST/!PuTTY" "$DEST/!PuTTYgen"
    cp -r 'build/app/!PuTTY' "$DEST/"
    cp -r 'build/app/!PuTTYgen' "$DEST/"
fi
echo "deployed to $DEST"
