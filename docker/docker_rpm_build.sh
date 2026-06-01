#!/usr/bin/env bash
set -e

# Docker wrapper for SRPM and RPM builds from the tracked working tree.

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
MODE="${1:-}"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/container_runtime.sh"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"
CONTAINER_RUN_ARGS=()
CONTAINER_BUILD_COMMAND='
  set -e

  # Docker normally starts as root. Create a matching user inside the container
  # so files written into the bind mount keep the host user ownership.
  if ! getent group "$HOST_GID" >/dev/null; then
    groupadd -g "$HOST_GID" dnfui-builder
  fi

  BUILDER_GROUP="$(getent group "$HOST_GID" | cut -d: -f1)"

  if ! getent passwd "$HOST_UID" >/dev/null; then
    useradd -u "$HOST_UID" -g "$BUILDER_GROUP" -M -d "$HOME" -s /bin/bash dnfui-builder
  fi

  BUILDER_USER="$(getent passwd "$HOST_UID" | cut -d: -f1)"

  mkdir -p "$HOME"
  chown "$HOST_UID:$HOST_GID" "$HOME"

  runuser -u "$BUILDER_USER" -- env HOME="$HOME" bash -lc "$BUILD_SCRIPT"
'

if [ "$CONTAINER_RUNTIME" = "podman" ] && [ "$HOST_UID" != "0" ]; then
  # Podman can preserve the host user directly, so the container does not need
  # to create a matching build user first.
  CONTAINER_RUN_ARGS=(
    --userns=keep-id
    --user "$HOST_UID:$HOST_GID"
    --passwd
    --passwd-entry "dnfui-builder:x:$HOST_UID:$HOST_GID:DNF UI Builder:/tmp/dnfui-home:/bin/bash"
  )
  CONTAINER_BUILD_COMMAND='
    set -e
    mkdir -p "$HOME"
    bash -lc "$BUILD_SCRIPT"
  '
fi

case "$MODE" in
srpm)
  BUILD_SCRIPT="./packaging/build_srpm.sh"
  ;;
rpm)
  BUILD_SCRIPT="./packaging/build_rpm.sh"
  ;;
*)
  echo "*** Use docker_rpm_build.sh with srpm or rpm ***" >&2
  exit 1
  ;;
esac

# Ensure image exists:
if ! container_image_exists "$IMAGE_NAME"; then
  color_print "$FMT_RED" "$(container_missing_image_message)"
  exit 1
fi

if [ "$MODE" = "srpm" ]; then
  color_print "$FMT_GREEN" "*** Building source RPM inside container... ***"
else
  color_print "$FMT_GREEN" "*** Building RPMs inside container... ***"
fi

"$CONTAINER_RUNTIME" run --rm \
  "${CONTAINER_RUN_ARGS[@]}" \
  --name "dnfui-$MODE-build" \
  --init \
  -w /workspace \
  -e HOME=/tmp/dnfui-home \
  -e HOST_UID="$HOST_UID" \
  -e HOST_GID="$HOST_GID" \
  -e BUILD_SCRIPT="$BUILD_SCRIPT" \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash -lc "$CONTAINER_BUILD_COMMAND"
