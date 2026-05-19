#!/usr/bin/env bash
set -e

# Session bus smoke test for transaction preview cancellation. This exercises
# the request object state changes for Cancel, GetPreview, and Release through
# the test-only transaction service binary.

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$PROJECT_ROOT/utils/transaction_service_paths.conf"

SERVICE_BIN="${SERVICE_BIN:-$PROJECT_ROOT/$TRANSACTION_SERVICE_TEST_BIN_NAME}"
INSTALL_SPEC="${SERVICE_TEST_INSTALL_SPEC:-}"
REINSTALL_SPEC="${SERVICE_TEST_REINSTALL_NEVRA:-}"
TIMEOUT_SECONDS="${SERVICE_TEST_TIMEOUT_SECONDS:-180}"

if [ ! -x "$SERVICE_BIN" ]; then
  echo "*** Missing service binary: $SERVICE_BIN ***" >&2
  echo "*** Build it first with: ./utils/meson_build.sh service-tests ***" >&2
  exit 1
fi

if [ -z "$INSTALL_SPEC" ] && [ -z "$REINSTALL_SPEC" ]; then
  echo "*** Set SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA before running servicecanceltest ***" >&2
  exit 1
fi

if [ -n "$INSTALL_SPEC" ] && [ -n "$REINSTALL_SPEC" ]; then
  echo "*** Set only one of SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA ***" >&2
  exit 1
fi

LOG_FILE="$(mktemp)"
cleanup_log() {
  rm -f "$LOG_FILE"
}
trap cleanup_log EXIT

echo "*** Running transaction service cancel test ***"
echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"

SERVICE_BIN="$SERVICE_BIN" \
SERVICE_NAME="$TRANSACTION_SERVICE_NAME" \
MANAGER_PATH="$TRANSACTION_SERVICE_MANAGER_PATH" \
MANAGER_METHOD="$TRANSACTION_SERVICE_START_METHOD" \
RESULT_METHOD="$TRANSACTION_SERVICE_RESULT_METHOD" \
CANCEL_METHOD="$TRANSACTION_SERVICE_CANCEL_METHOD" \
PREVIEW_METHOD="$TRANSACTION_SERVICE_PREVIEW_METHOD" \
RELEASE_METHOD="$TRANSACTION_SERVICE_RELEASE_METHOD" \
INSTALL_SPEC="$INSTALL_SPEC" \
REINSTALL_SPEC="$REINSTALL_SPEC" \
LOG_FILE="$LOG_FILE" \
TIMEOUT_SECONDS="$TIMEOUT_SECONDS" \
dbus-run-session -- bash <<'EOF'
  set -e

  wait_for_result() {
    local transaction_path="$1"
    local expected_stage="$2"
    local expected_success="$3"
    local deadline="$((SECONDS + TIMEOUT_SECONDS))"
    local result=""

    while :; do
      result="$(gdbus call \
        --session \
        --dest "$SERVICE_NAME" \
        --object-path "$transaction_path" \
        --method "$RESULT_METHOD")"

      if printf "%s\n" "$result" | grep -Fq "'$expected_stage', true, $expected_success,"; then
        printf "%s\n" "$result"
        return 0
      fi

      if printf "%s\n" "$result" | grep -Fq "'preview-running'"; then
        :
      else
        printf "%s\n" "$result"
        echo "*** Transaction did not reach the expected result state ***" >&2
        return 1
      fi

      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "*** Timed out waiting for transaction service result after ${TIMEOUT_SECONDS} seconds ***" >&2
        echo "*** Last observed result: $result ***" >&2
        echo "*** Service log ***" >&2
        cat "$LOG_FILE" >&2 || true
        return 1
      fi

      sleep 1
    done
  }

  cleanup() {
    if [ -n "${service_pid:-}" ]; then
      kill "$service_pid" >/dev/null 2>&1 || true
      wait "$service_pid" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup EXIT

  "$SERVICE_BIN" --session >"$LOG_FILE" 2>&1 &
  service_pid=$!

  gdbus wait --session "$SERVICE_NAME" >/dev/null

  start_install="[]"
  start_reinstall="[]"
  if [ -n "$INSTALL_SPEC" ]; then
    start_install="[\"$INSTALL_SPEC\"]"
  else
    start_reinstall="[\"$REINSTALL_SPEC\"]"
  fi

  reply="$(gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$MANAGER_PATH" \
    --method "$MANAGER_METHOD" \
    "$start_install" \
    "[]" \
    "$start_reinstall")"

  echo "$reply"

  case "$reply" in
    *"/com/fedora/Dnfui/Transaction1/requests/"* )
      ;;
    * )
      echo "*** Service did not return a transaction object path ***" >&2
      exit 1
      ;;
  esac

  transaction_path="${reply#*objectpath \'}"
  transaction_path="${transaction_path%%\'*}"
  if [ -z "$transaction_path" ]; then
    echo "*** Failed to parse transaction object path ***" >&2
    exit 1
  fi

  gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$CANCEL_METHOD" >/dev/null

  cancelled_result="$(wait_for_result "$transaction_path" "cancelled" "false")"
  echo "$cancelled_result"

  set +e
  preview_output="$(gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$PREVIEW_METHOD" 2>&1)"
  preview_status=$?
  set -e

  echo "$preview_output"

  if [ "$preview_status" -eq 0 ]; then
    echo "*** Cancelled transaction unexpectedly returned preview data ***" >&2
    exit 1
  fi

  case "$preview_output" in
    *"Transaction preview is not available."* )
      ;;
    * )
      echo "*** Cancelled transaction did not reject GetPreview as expected ***" >&2
      exit 1
      ;;
  esac

  gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$RELEASE_METHOD" >/dev/null

  set +e
  released_result="$(gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$RESULT_METHOD" 2>&1)"
  released_status=$?
  set -e

  if [ "$released_status" -eq 0 ]; then
    echo "*** Released cancelled transaction is still reachable ***" >&2
    exit 1
  fi

  echo "*** Service log ***"
  cat "$LOG_FILE"
EOF
