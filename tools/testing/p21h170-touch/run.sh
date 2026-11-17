#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$TEST_DIR/../../.." && pwd)"
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/p21h170-touch.XXXXXXXX")"
trap 'rm -rf -- "$TEST_TMP"' EXIT
"${HOSTCC:-cc}" -std=c99 -O2 -Wall -Wextra -Werror \
    -I"$TEST_DIR/include" -I"$SRC_DIR/drivers/input/touchscreen/nt36xxx" \
    "$TEST_DIR/protocol_test.c" -o "$TEST_TMP/protocol_test"
"$TEST_TMP/protocol_test"
