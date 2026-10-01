#!/bin/sh
# Start a throw-away SSH server for testing the RISC OS build: PuTTY's own
# "Uppity" (NOT secure - it exists for testing PuTTY), built natively from the
# same upstream tree.  It listens on 127.0.0.1:2222 and accepts only the
# generated test key.  From inside RPCEmu the host is 10.10.10.2.
#
#   cmake -S upstream/putty-0.85 -B build/host -DPUTTY_GTK_VERSION=0 && make -C build/host
#   tools/testserver.sh            (leave running; Ctrl-C to stop)
#   tools/deploy.sh                (in another terminal)
set -eu
cd "$(dirname "$0")/.."
H=build/host
T=build/test
[ -x $H/uppity ] || { echo "build the host tools first (see the top of this file)"; exit 1; }
mkdir -p $T
[ -f $T/hostkey.ppk ] || $H/puttygen -t ed25519 -o $T/hostkey.ppk --new-passphrase /dev/null
if [ ! -f $T/client.ppk ]; then
    $H/puttygen -t ed25519 -o $T/client.ppk --new-passphrase /dev/null
    $H/puttygen -O public-openssh $T/client.ppk -o $T/client.pub
fi
FP=$($H/puttygen -l $T/hostkey.ppk | awk '{print $3}')
echo "host key fingerprint: $FP"
echo "in RPCEmu:  Run HostFS::HostFS.\$.PuttyDev.plink -batch -v -i HostFS::HostFS.\$.PuttyDev.ClientKey -l tester -P 2222 -hostkey $FP 10.10.10.2 echo hello"
exec $H/uppity --listen 2222 --hostkey $T/hostkey.ppk --userkey $T/client.pub --verbose
