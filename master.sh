#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: ./master.sh <reference.wav> <input.wav> [mastered_cli options]" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="$ROOT/build/mastered_cli"

if [ ! -x "$CLI" ]; then
    "$ROOT/build.sh"
fi

REFERENCE="$1"
INPUT="$2"
shift 2

exec "$CLI" "$REFERENCE" "$INPUT" "$@"
