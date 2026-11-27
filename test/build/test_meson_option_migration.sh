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

# An empty directory must be accepted by the migration helper.
mkdir "$WORK_DIR/empty"
"$PROJECT_ROOT/utils/meson_register_tui_option.sh" "$WORK_DIR/empty" meson
meson setup "$WORK_DIR/empty" "$WORK_DIR/source" --reconfigure -Dbuild_tui=true

# A failed initial setup must be recoverable without deleting its directory.
printf "project('option-migration')\nerror('Intentional setup failure')\n" > "$WORK_DIR/source/meson.build"
if meson setup "$WORK_DIR/incomplete" "$WORK_DIR/source"; then
  echo "Expected initial Meson setup to fail" >&2
  exit 1
fi
printf "project('option-migration')\n" > "$WORK_DIR/source/meson.build"
"$PROJECT_ROOT/utils/meson_register_tui_option.sh" "$WORK_DIR/incomplete" meson
meson setup "$WORK_DIR/incomplete" "$WORK_DIR/source" --reconfigure -Dbuild_tui=true
