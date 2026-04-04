#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TELEPATH_ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

HOST="${TELEPATH_WEB_HOST:-127.0.0.1}"
PORT="${TELEPATH_WEB_PORT:-8080}"

cd "${TELEPATH_ROOT_DIR}"
exec python3 web/backend/server.py --host "${HOST}" --port "${PORT}"
