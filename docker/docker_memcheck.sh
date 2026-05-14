#!/usr/bin/env bash
set -e

# Run the project memory checks inside the development container.
# The build runs as the host user so the Makefile normal user guard still works.
# Valgrind fails in some containers when the file descriptor limit is huge, so
# the command lowers it to a normal value before starting the checks.

# Print colors:
FMT_RED=$(printf '\033[31m')
FMT_GREEN=$(printf '\033[32m')
FMT_RESET=$(printf '\033[0m')

color_print() {
  local color="$1"
  shift
  echo -e "${color}$*${FMT_RESET}"
}

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-memcheck"
DOCKER_TTY_ARGS=()
if [ "${DOCKER_USE_TTY:-1}" != "0" ]; then
  DOCKER_TTY_ARGS=(-it)
fi

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/container_runtime.sh"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"
OPTIONAL_ENV_NAMES=(
  FINAL
  ASAN
  MEMCHECK_GEN_SUPPRESSIONS
  MEMCHECK_SMOKE_FILTER
  MEMCHECK_SMOKE_TIMEOUT
  MEMCHECK_TEST_FILTER
  MEMCHECK_TEST_TIMEOUT
  MEMCHECK_TRACK_FDS
)

# Ensure image exists:
if ! container_image_exists "$IMAGE_NAME"; then
  color_print "$FMT_RED" "$(container_missing_image_message)"
  exit 1
fi

CONTAINER_RUN_ARGS=()
CONTAINER_ENV_ARGS=(
  -e "DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build"
  -e "HOST_UID=$HOST_UID"
  -e "HOST_GID=$HOST_GID"
  -e "HOME=/tmp/dnfui-home"
  -e "MEMCHECK_SCRIPT=ulimit -n 1024 && make memory-check"
)
for env_name in "${OPTIONAL_ENV_NAMES[@]}"; do
  if [ -n "${!env_name+x}" ]; then
    CONTAINER_ENV_ARGS+=(-e "$env_name=${!env_name}")
  fi
done

# shellcheck disable=SC2016
CONTAINER_MEMCHECK_COMMAND='
  set -e

  if ! getent group "$HOST_GID" >/dev/null; then
    groupadd -g "$HOST_GID" dnfui-memcheck
  fi

  MEMCHECK_GROUP="$(getent group "$HOST_GID" | cut -d: -f1)"

  if ! getent passwd "$HOST_UID" >/dev/null; then
    useradd -u "$HOST_UID" -g "$MEMCHECK_GROUP" -M -d "$HOME" -s /bin/bash dnfui-memcheck
  fi

  MEMCHECK_USER="$(getent passwd "$HOST_UID" | cut -d: -f1)"

  mkdir -p "$HOME"
  chown "$HOST_UID:$HOST_GID" "$HOME"

  MEMCHECK_ENV=(
    HOME="$HOME"
    DNFUI_MESON_BUILD_ROOT="$DNFUI_MESON_BUILD_ROOT"
  )

  for env_name in FINAL ASAN MEMCHECK_GEN_SUPPRESSIONS MEMCHECK_SMOKE_FILTER MEMCHECK_SMOKE_TIMEOUT MEMCHECK_TEST_FILTER MEMCHECK_TEST_TIMEOUT MEMCHECK_TRACK_FDS; do
    if [ -n "${!env_name+x}" ]; then
      MEMCHECK_ENV+=("$env_name=${!env_name}")
    fi
  done

  runuser -u "$MEMCHECK_USER" -- env "${MEMCHECK_ENV[@]}" bash -lc "$MEMCHECK_SCRIPT"
'

if [ "$CONTAINER_RUNTIME" = "podman" ] && [ "$HOST_UID" != "0" ]; then
  CONTAINER_RUN_ARGS=(
    --userns=keep-id
    --user "$HOST_UID:$HOST_GID"
    --passwd
    --passwd-entry "dnfui-memcheck:x:$HOST_UID:$HOST_GID:DNF UI Memcheck:/tmp/dnfui-home:/bin/bash"
  )
  # shellcheck disable=SC2016
  CONTAINER_MEMCHECK_COMMAND='
    set -e
    mkdir -p "$HOME"
    bash -lc "$MEMCHECK_SCRIPT"
  '
fi

color_print "$FMT_GREEN" "*** Running memory checks inside container... ***"

"$CONTAINER_RUNTIME" run --rm "${DOCKER_TTY_ARGS[@]}" \
  "${CONTAINER_RUN_ARGS[@]}" \
  --name "$CONTAINER_NAME" \
  --init \
  -w /workspace \
  "${CONTAINER_ENV_ARGS[@]}" \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash -lc "$CONTAINER_MEMCHECK_COMMAND"
