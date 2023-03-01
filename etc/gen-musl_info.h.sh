#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"

HEADER_FILE="$PROJECT/src/musl_info.h"

_co_targets

echo "generating $(_relpath "$HEADER_FILE")"

cat << END > "$HEADER_FILE"
// info about bundled musl
// Do not edit! Generated by ${SCRIPT_FILE##$PROJECT/}
// SPDX-License-Identifier: Apache-2.0
END

_err "not implemented"
