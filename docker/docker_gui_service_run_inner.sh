#!/usr/bin/env bash
set -e

# Docker GUI run helper that starts the app and transaction service on a shared
# session bus for manual transaction testing inside the container.

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck disable=SC1091
source "$PROJECT_ROOT/utils/transaction_service_paths.conf"

export DNFUI_MESON_BUILD_ROOT="/tmp/dnfui-build"
BUILD_DIR="$("$PROJECT_ROOT/utils/meson_build.sh" build-dir)"
SERVICE_BIN="$BUILD_DIR/src/service/$TRANSACTION_SERVICE_BIN_NAME"
APP_BIN="$BUILD_DIR/src/dnfui"

echo "*** Building app and transaction service ***"
"$PROJECT_ROOT/utils/meson_build.sh" all
meson compile -C "$BUILD_DIR" dnf-ui-gmo

# Normal packages install translation catalogs into the system locale directory.
# The Docker GUI target runs the build tree binary, so copy built
# catalogs into the container locale directory before starting GTK.
find "$BUILD_DIR/po" -path "*/LC_MESSAGES/dnf-ui.mo" -print0 | while IFS= read -r -d '' mo_file; do
  locale_dir="$(basename "$(dirname "$(dirname "$mo_file")")")"
  install -Dm0644 "$mo_file" "/usr/share/locale/$locale_dir/LC_MESSAGES/dnf-ui.mo"
done

for path in "$SERVICE_BIN" "$APP_BIN"; do
  if [ ! -x "$path" ]; then
    echo "*** Missing runtime file: $path ***" >&2
    exit 1
  fi
done

echo "*** Starting session bus transaction service for Docker GUI testing ***"
echo "*** System bus Polkit tests remain available through the dockerservicesystem targets ***"

export DNFUI_TRANSACTION_BUS=session
export SERVICE_BIN
export TRANSACTION_SERVICE_NAME
export APP_BIN

dbus-run-session -- bash <<'EOF'
set -e

# Start the transaction service on the session bus:
"$SERVICE_BIN" --session >/tmp/dnfui-service.log 2>&1 &

# Save the service process ID so it can be stopped when the UI exits:
service_pid=$!

# Stop the transaction service when the UI exits:
trap 'kill "$service_pid" >/dev/null 2>&1 || true; wait "$service_pid" >/dev/null 2>&1 || true' EXIT

# Wait until the transaction service has claimed its D-Bus name:
gdbus wait --session "$TRANSACTION_SERVICE_NAME" >/dev/null

# Start the UI in the same session bus environment:
"$APP_BIN"
EOF
