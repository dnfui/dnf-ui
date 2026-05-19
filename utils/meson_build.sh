#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_BUILD_ROOT="$PROJECT_ROOT/build"

if [ "${FINAL:-}" = "y" ]; then
  BUILD_NAME="final"
  BUILD_TYPE="release"
  FINAL_BUILD="true"
  WARNING_LEVEL="3"
else
  BUILD_NAME="debug"
  BUILD_TYPE="debug"
  FINAL_BUILD="false"
  WARNING_LEVEL="0"
fi

if [ "${ASAN:-}" = "y" ]; then
  SANITIZE="address"
else
  SANITIZE="none"
fi

if [ "${DEBUG_TRACE:-}" = "y" ]; then
  DEBUG_TRACE_BUILD="true"
else
  DEBUG_TRACE_BUILD="false"
fi

BUILD_ROOT="${DNFUI_MESON_BUILD_ROOT:-$DEFAULT_BUILD_ROOT}"
BUILD_DIR="$BUILD_ROOT/$BUILD_NAME"
APP_BIN="$PROJECT_ROOT/dnfui"
SERVICE_BIN="$PROJECT_ROOT/dnfui-service"
SERVICE_TEST_BIN="$PROJECT_ROOT/dnfui-service-tests"
TEST_BIN="$PROJECT_ROOT/dnfui-tests"
SERVICE_SMOKE_CLIENT_BIN="$PROJECT_ROOT/dnfui-service-smoke-client"

# Keep repo-root convenience symlinks for the default native build tree, but
# avoid rewriting the workspace when callers intentionally build elsewhere
# (for example Docker builds under /tmp).
LINK_ROOT_SYMLINKS="${DNFUI_LINK_ROOT_SYMLINKS:-auto}"
case "$LINK_ROOT_SYMLINKS" in
auto)
  if [ "$BUILD_ROOT" = "$DEFAULT_BUILD_ROOT" ]; then
    LINK_ROOT_SYMLINKS="yes"
  else
    LINK_ROOT_SYMLINKS="no"
  fi
  ;;
yes | no) ;;
*)
  echo "*** Set DNFUI_LINK_ROOT_SYMLINKS to yes, no, or auto ***" >&2
  exit 1
  ;;
esac

build_tests="false"
target_names=()
link_app="no"
link_service="no"
link_service_tests="no"
link_smoke_client="no"
link_tests="no"

for arg in "$@"; do
  case "$arg" in
  build-dir)
    echo "$BUILD_DIR"
    exit 0
    ;;
  app)
    target_names+=("dnfui")
    link_app="yes"
    ;;
  service)
    target_names+=("dnfui-service")
    link_service="yes"
    ;;
  service-tests)
    build_tests="true"
    target_names+=("dnfui-service-tests")
    link_service_tests="yes"
    ;;
  smoke-client)
    build_tests="true"
    target_names+=("dnfui-service-smoke-client")
    link_smoke_client="yes"
    ;;
  tests)
    build_tests="true"
    target_names+=("dnfui-tests")
    link_tests="yes"
    ;;
  all)
    target_names+=("dnfui" "dnfui-service")
    link_app="yes"
    link_service="yes"
    ;;
  *)
    echo "*** Unknown meson build target: $arg ***" >&2
    exit 1
    ;;
  esac
done

if [ "${#target_names[@]}" -eq 0 ]; then
  echo "*** Set at least one meson build target. Use app, service, service-tests, smoke-client, tests, all, or build-dir. ***" >&2
  exit 1
fi

if [ -d "$BUILD_DIR" ]; then
  meson setup "$BUILD_DIR" \
    --reconfigure \
    --prefix /usr \
    --libexecdir libexec \
    --buildtype "$BUILD_TYPE" \
    -Dwarning_level="$WARNING_LEVEL" \
    -Dbuild_tests="$build_tests" \
    -Ddebug_trace="$DEBUG_TRACE_BUILD" \
    -Dfinal_build="$FINAL_BUILD" \
    -Db_sanitize="$SANITIZE"
else
  meson setup "$BUILD_DIR" \
    --prefix /usr \
    --libexecdir libexec \
    --buildtype "$BUILD_TYPE" \
    -Dwarning_level="$WARNING_LEVEL" \
    -Dbuild_tests="$build_tests" \
    -Ddebug_trace="$DEBUG_TRACE_BUILD" \
    -Dfinal_build="$FINAL_BUILD" \
    -Db_sanitize="$SANITIZE"
fi

meson compile -C "$BUILD_DIR" "${target_names[@]}"

if [ "$LINK_ROOT_SYMLINKS" = "yes" ] && [ "$link_app" = "yes" ]; then
  ln -sfn "$BUILD_DIR/src/dnfui" "$APP_BIN"
fi

if [ "$LINK_ROOT_SYMLINKS" = "yes" ] && [ "$link_service" = "yes" ]; then
  ln -sfn "$BUILD_DIR/src/service/dnfui-service" "$SERVICE_BIN"
fi

if [ "$LINK_ROOT_SYMLINKS" = "yes" ] && [ "$link_service_tests" = "yes" ]; then
  ln -sfn "$BUILD_DIR/src/service/dnfui-service-tests" "$SERVICE_TEST_BIN"
fi

if [ "$LINK_ROOT_SYMLINKS" = "yes" ] && [ "$link_smoke_client" = "yes" ]; then
  ln -sfn "$BUILD_DIR/test/dnfui-service-smoke-client" "$SERVICE_SMOKE_CLIENT_BIN"
fi

if [ "$LINK_ROOT_SYMLINKS" = "yes" ] && [ "$link_tests" = "yes" ]; then
  ln -sfn "$BUILD_DIR/test/dnfui-tests" "$TEST_BIN"
fi
