#!/usr/bin/env bash
set -e

# Docker GDB host helper that starts the debug container and waits until
# gdbserver is ready for VS Code to connect.

FMT_RED=$(printf '\033[31m')
FMT_GREEN=$(printf '\033[32m')
FMT_BLUE=$(printf '\033[34m')
FMT_RESET=$(printf '\033[0m')

color_print() {
  local color="$1"
  shift
  echo -e "${color}$*${FMT_RESET}"
}

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-gdb"
THEME_MODE="${THEME:-default}"
GDB_PORT="${GDB_PORT:-2345}"
CACHE_MODE="${CACHE_MODE:-persistent}"
CACHE_VOLUME_NAME="${CACHE_VOLUME_NAME:-dnfui-repo-cache}"
SESSION_BUS_ADDRESS="unix:path=/tmp/dnfui-gdb-session-bus"

# Configure the optional GTK theme override:
THEME_OPTS=()
case "$THEME_MODE" in
"" | "default")
  color_print "$FMT_BLUE" "*** GTK theme: default ***"
  ;;
"dark")
  color_print "$FMT_BLUE" "*** GTK theme: dark ***"
  THEME_OPTS=(-e GTK_THEME=Adwaita:dark)
  ;;
"light")
  color_print "$FMT_BLUE" "*** GTK theme: light ***"
  THEME_OPTS=(-e GTK_THEME=Adwaita)
  ;;
*)
  color_print "$FMT_RED" "*** Invalid THEME value: $THEME_MODE. Use dark, light, or default. ***"
  exit 1
  ;;
esac

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/container_runtime.sh"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

# Pass through the host display server connection:
if [ "$XDG_SESSION_TYPE" = "wayland" ] && [ -n "$WAYLAND_DISPLAY" ]; then
  color_print "$FMT_BLUE" "*** Wayland detected ***"
  DISPLAY_OPTS=(
    -e WAYLAND_DISPLAY="$WAYLAND_DISPLAY"
    -e XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR"
    -v "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
  )
else
  color_print "$FMT_BLUE" "*** X11 detected ***"
  if [ "$CONTAINER_RUNTIME" = "docker" ]; then
    xhost +local:docker >/dev/null 2>&1 || true
  fi
  DISPLAY_OPTS=(
    -e DISPLAY="$DISPLAY"
    -v /tmp/.X11-unix:/tmp/.X11-unix
  )
fi

# Require the development image before starting the container:
if ! container_image_exists "$IMAGE_NAME"; then
  color_print "$FMT_RED" "$(container_missing_image_message)"
  exit 1
fi

# Remove any previous debug container before starting a new one:
if "$CONTAINER_RUNTIME" container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
  color_print "$FMT_BLUE" "*** Removing existing GDB container ***"
  "$CONTAINER_RUNTIME" rm -f "$CONTAINER_NAME" >/dev/null
fi

# Configure the DNF cache used by libdnf5 inside the container:
CACHE_OPTS=()
case "$CACHE_MODE" in
"persistent")
  color_print "$FMT_BLUE" "*** DNF cache: persistent volume $CACHE_VOLUME_NAME ***"
  CACHE_OPTS=(-v "$CACHE_VOLUME_NAME:/var/cache/libdnf5")
  ;;
"empty")
  color_print "$FMT_BLUE" "*** DNF cache: empty tmpfs ***"
  CACHE_OPTS=(--tmpfs /var/cache/libdnf5)
  ;;
*)
  color_print "$FMT_RED" "*** Invalid CACHE_MODE value: $CACHE_MODE. Use persistent or empty. ***"
  exit 1
  ;;
esac

# Start the debug container in the background:
color_print "$FMT_GREEN" "*** Starting Docker GDB debug server... ***"
"$CONTAINER_RUNTIME" run -d \
  --name "$CONTAINER_NAME" \
  --init \
  --cap-add=SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -p "127.0.0.1:$GDB_PORT:$GDB_PORT" \
  -w /workspace \
  "${DISPLAY_OPTS[@]}" \
  "${THEME_OPTS[@]}" \
  --device /dev/dri \
  -e GSETTINGS_BACKEND=memory \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -e GDB_PORT="$GDB_PORT" \
  -e DNFUI_TRANSACTION_BUS=session \
  -e DBUS_SESSION_BUS_ADDRESS="$SESSION_BUS_ADDRESS" \
  -v "$HOST_DIR:/workspace" \
  "${CACHE_OPTS[@]}" \
  "$IMAGE_NAME" \
  bash /workspace/docker/docker_gdb_inner.sh >/dev/null

ready="no"

# Wait until gdbserver is listening before letting VS Code continue:
for _ in $(seq 1 120); do
  if ! "$CONTAINER_RUNTIME" container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
    color_print "$FMT_RED" "*** Docker GDB container exited before gdbserver was ready ***"
    "$CONTAINER_RUNTIME" logs "$CONTAINER_NAME" 2>&1 || true
    exit 1
  fi

  if "$CONTAINER_RUNTIME" logs --tail 200 "$CONTAINER_NAME" 2>&1 | grep -q "Listening on port "; then
    ready="yes"
    break
  fi

  sleep 1
done

# Print the latest container log output for the VS Code task terminal:
"$CONTAINER_RUNTIME" logs --tail 200 "$CONTAINER_NAME" 2>&1 || true

if [ "$ready" != "yes" ]; then
  color_print "$FMT_RED" "*** Docker GDB server did not start ***"
  exit 1
fi

color_print "$FMT_GREEN" "*** Docker GDB server is ready ***"
exit 0
