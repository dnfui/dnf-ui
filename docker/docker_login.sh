#!/usr/bin/env bash
set -e

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-shell"
DOCKER_NETWORK_MODE="${DOCKER_NETWORK_MODE:-}"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/container_runtime.sh"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

# Ensure image exists:
if ! container_image_exists "$IMAGE_NAME"; then
  container_missing_image_message
  exit 1
fi

docker_network_args=()
if [ -n "$DOCKER_NETWORK_MODE" ]; then
  docker_network_args+=(--network "$DOCKER_NETWORK_MODE")
fi

# Run an interactive shell inside the container:
"$CONTAINER_RUNTIME" run --rm -it \
  --name "$CONTAINER_NAME" \
  --init \
  "${docker_network_args[@]}" \
  -w /workspace \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash
