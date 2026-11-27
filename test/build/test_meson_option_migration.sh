#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

mkdir "$WORK_DIR/source"
printf "project('option-migration')\n" > "$WORK_DIR/source/meson.build"
meson setup "$WORK_DIR/build" "$WORK_DIR/source"

cat > "$WORK_DIR/source/meson_options.txt" <<'EOF'
option('build_tui', type: 'boolean', value: false)
EOF

"$PROJECT_ROOT/utils/meson_register_tui_option.sh" "$WORK_DIR/build" meson
meson setup "$WORK_DIR/build" --reconfigure -Dbuild_tui=true
meson introspect --buildoptions "$WORK_DIR/build" | python3 -c '
import json
import sys

options = {option["name"]: option["value"] for option in json.load(sys.stdin)}
assert options["build_tui"] is True
'
"$PROJECT_ROOT/utils/meson_register_tui_option.sh" "$WORK_DIR/build" meson
meson setup "$WORK_DIR/build" --reconfigure -Dbuild_tui=false
meson introspect --buildoptions "$WORK_DIR/build" | python3 -c '
import json
import sys

options = {option["name"]: option["value"] for option in json.load(sys.stdin)}
assert options["build_tui"] is False
'
