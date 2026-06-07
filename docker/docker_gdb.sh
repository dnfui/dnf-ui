#!/usr/bin/env bash
set -e

# Docker GDB server wrapper for VS Code debugging.
# It uses the same dnf5daemon system bus setup as the normal Docker run target.

FMT_RED=$(printf '\033[31m')
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
CACHE_VOLUME_NAME="${CACHE_VOLUME_NAME:-dnfui-repo-cache}"
GDB_WAIT_TIMEOUT="${GDB_WAIT_TIMEOUT:-120}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/container_runtime.sh"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

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

LOCALE_OPTS=()
for locale_var in LANG LANGUAGE LC_ALL; do
  if [ -n "${!locale_var:-}" ]; then
    LOCALE_OPTS+=(-e "$locale_var=${!locale_var}")
  fi
done

if [ "${XDG_SESSION_TYPE:-}" = "wayland" ] && [ -n "${WAYLAND_DISPLAY:-}" ]; then
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
    -e DISPLAY="${DISPLAY:-}"
    -v /tmp/.X11-unix:/tmp/.X11-unix
  )
fi

if ! container_image_exists "$IMAGE_NAME"; then
  color_print "$FMT_RED" "$(container_missing_image_message)"
  exit 1
fi

if "$CONTAINER_RUNTIME" container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
  "$CONTAINER_RUNTIME" stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
fi

"$CONTAINER_RUNTIME" run --rm -d \
  --name "$CONTAINER_NAME" \
  --init \
  --cap-add=SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -w /workspace \
  "${DISPLAY_OPTS[@]}" \
  "${THEME_OPTS[@]}" \
  "${LOCALE_OPTS[@]}" \
  --device /dev/dri \
  -e GSETTINGS_BACKEND=memory \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -v "$HOST_DIR:/workspace" \
  -v "$CACHE_VOLUME_NAME:/var/cache/libdnf5" \
  "$IMAGE_NAME" \
  bash /workspace/docker/docker_gdb_inner.sh

echo "*** Docker GDB server container started: $CONTAINER_NAME ***"

# VS Code validates the remote program path as soon as the pre-launch task
# returns. Wait until the container has built the app and gdbserver is ready.
waited=0
while [ "$waited" -lt "$GDB_WAIT_TIMEOUT" ]; do
  if ! "$CONTAINER_RUNTIME" container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
    color_print "$FMT_RED" "*** Docker GDB container exited before gdbserver started. ***"
    "$CONTAINER_RUNTIME" logs "$CONTAINER_NAME" 2>/dev/null || true
    exit 1
  fi

  if "$CONTAINER_RUNTIME" logs "$CONTAINER_NAME" 2>&1 | grep -q "Listening on port 2345"; then
    echo "*** Docker GDB server is listening on port 2345. ***"
    echo "*** VS Code can now attach through DEBUG DOCKER. ***"
    exit 0
  fi

  sleep 1
  waited=$((waited + 1))
done

color_print "$FMT_RED" "*** Timed out waiting for Docker GDB server. ***"
"$CONTAINER_RUNTIME" logs "$CONTAINER_NAME" 2>/dev/null || true
"$CONTAINER_RUNTIME" stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
exit 1
