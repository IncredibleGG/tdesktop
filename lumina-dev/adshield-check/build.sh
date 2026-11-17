#!/bin/sh
# Build and run the AdShield conformance checker against the engine sources in
# Telegram/SourceFiles/lumina/. Not part of the app build.
#
#   sh lumina-dev/adshield-check/build.sh            g++ (or $CXX), -Werror, then ASan+UBSan
#   CXX=clang++ sh lumina-dev/adshield-check/build.sh
#
# Output binaries go to $OUT (default: a temporary directory).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="$REPO/Telegram/SourceFiles"
CXX="${CXX:-g++}"
OUT="${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/adshield-check.XXXXXX")}"
FILES="$HERE/check.cpp $SRC/lumina/lumina_ad_shield.cpp $SRC/lumina/lumina_ad_shield_store.cpp $SRC/lumina/lumina_ad_shield_account.cpp"
WARN="-Wall -Wextra -Werror"

echo "== $CXX -O1 $WARN"
"$CXX" -std=c++20 -O1 $WARN -I "$SRC" -I "$HERE" $FILES -o "$OUT/adshield-check"
"$OUT/adshield-check" "$HERE"

echo "== $CXX -O1 -fsanitize=address,undefined"
"$CXX" -std=c++20 -O1 -g $WARN -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -I "$SRC" -I "$HERE" $FILES -o "$OUT/adshield-check-asan"
"$OUT/adshield-check-asan" "$HERE"
