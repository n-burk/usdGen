#!/usr/bin/env bash
set -euo pipefail
_schema_check_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$_schema_check_root/bin/_env.sh"
exec "$PY" "$_schema_check_root/tests/checks/check_expression_schema.py"
