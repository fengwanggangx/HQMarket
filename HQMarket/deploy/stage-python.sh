#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME="${ROOT}/runtime/python"
PYTHON_BIN="${PYTHON_BIN:-python3.12}"

"${PYTHON_BIN}" -c 'import sys; assert sys.version_info[:2] == (3, 12)'
mkdir -p "${RUNTIME}"
cp -a "$("${PYTHON_BIN}" -c 'import sys; print(sys.prefix)')/." "${RUNTIME}/"
"${RUNTIME}/bin/python3.12" -m pip install --disable-pip-version-check --no-cache-dir -r "${ROOT}/python/requirements.lock"
"${RUNTIME}/bin/python3.12" -m pip check
"${RUNTIME}/bin/python3.12" -c 'import akshare, mootdx, pandas; print("embedded providers ready")'
