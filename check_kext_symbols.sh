#!/usr/bin/env bash
# check_kext_symbols.sh
#
# Loads the rtl8188ee kext and prints a clean, deduplicated list of
# undefined ("failed to bind") symbols instead of kmutil's raw output,
# which repeats every symbol once per call site (can be hundreds of
# lines for a single missing symbol).
#
# Usage: ./check_kext_symbols.sh [path-to-kext]
# Defaults to build/out/rtl8188ee.kext relative to the project root.

set -euo pipefail

KEXT_PATH="${1:-build/out/rtl8188ee.kext}"

if [ ! -d "$KEXT_PATH" ]; then
    echo "error: kext not found at $KEXT_PATH" >&2
    echo "usage: $0 [path-to-kext]" >&2
    exit 1
fi

echo "== Attempting to load: $KEXT_PATH =="
echo

# kmutil's exit code is non-zero on failure to bind -- capture output
# regardless so we can still parse/report it, don't let set -e kill us.
RAW_OUTPUT="$(sudo kextutil -t "$KEXT_PATH" 2>&1 || true)"

SYMBOLS="$(echo "$RAW_OUTPUT" | grep -oE "bind \(_[a-zA-Z_0-9]+\)" | sed -E 's/bind \((_[a-zA-Z_0-9]+)\)/\1/' | sort -u || true)"

if [ -z "$SYMBOLS" ]; then
    if echo "$RAW_OUTPUT" | grep -qi "error\|failed"; then
        echo "Load failed, but no 'bind (...)' symbols were found in the output."
        echo "Full raw output below (something other than a missing symbol):"
        echo
        echo "$RAW_OUTPUT"
    else
        echo "No undefined symbols found. Kext load likely succeeded."
        echo
        echo "Raw output:"
        echo "$RAW_OUTPUT"
    fi
    exit 0
fi

COUNT="$(echo "$SYMBOLS" | wc -l | tr -d ' ')"

echo "== $COUNT unique undefined symbol(s) =="
echo "$SYMBOLS"
