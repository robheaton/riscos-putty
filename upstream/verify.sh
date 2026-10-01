#!/bin/sh
# Re-check the PuTTY source tarball: SHA-256 first, then the PGP signature.
# A throw-away GnuPG home is used, so the caller's own keyring is never touched.
set -eu
cd "$(dirname "$0")"

VER=0.85
TARBALL=putty-$VER.tar.gz
# Release Key (2026) and Master Key (2026).  Both fingerprints are printed on
# https://www.chiark.greenend.org.uk/~sgtatham/putty/keys.html, and the master
# one is also compiled into the source (PGP_MASTER_KEY_FP in putty.h).
RELEASE_FP=1A07DAC0F220CF12248EA7195C67D0F88BC3E5B5
MASTER_FP=371C662B855F318FF63AD091D732471F094D5FB5
MASTER_ID=D732471F094D5FB5

fail() { echo "verify.sh: FAILED - $*" >&2; exit 1; }

want=$(grep " $TARBALL\$" sha256sums | cut -d' ' -f1)
got=$(sha256sum "$TARBALL" | cut -d' ' -f1)
[ -n "$want" ] && [ "$want" = "$got" ] || fail "SHA-256 of $TARBALL does not match sha256sums"
echo "SHA-256 ok:    $got"

G=$(mktemp -d)
trap 'rm -rf "$G"' EXIT
chmod 700 "$G"
gpg --homedir "$G" --batch --quiet --import keys/release-2026.asc keys/master-2026.asc 2>/dev/null
fps=$(gpg --homedir "$G" --batch --with-colons --list-keys | awk -F: '$1=="fpr"{print $10}')
echo "$fps" | grep -qx "$RELEASE_FP" || fail "release key fingerprint is not $RELEASE_FP"
echo "$fps" | grep -qx "$MASTER_FP"  || fail "master key fingerprint is not $MASTER_FP"

gpg --homedir "$G" --batch --with-colons --check-sigs "$RELEASE_FP" 2>/dev/null |
    awk -F: -v id="$MASTER_ID" '$1=="sig" && $2=="!" && $5==id {ok=1} END{exit !ok}' ||
    fail "the master key has not certified the release key"
echo "Key chain ok:  master $MASTER_FP certifies release $RELEASE_FP"

st=$(gpg --homedir "$G" --batch --status-fd 1 --verify "$TARBALL.gpg" "$TARBALL" 2>/dev/null) ||
    fail "gpg --verify rejected the signature"
echo "$st" | grep -q "^\[GNUPG:\] VALIDSIG $RELEASE_FP " || fail "tarball is not signed by $RELEASE_FP"
echo "Signature ok:  $TARBALL signed by the PuTTY Releases key"
