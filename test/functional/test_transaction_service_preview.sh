#!/usr/bin/env bash
set -e

# Session bus smoke test for the standalone test-only transaction service
# binary. This is the smallest native check for the D-Bus manager and preview
# flow.

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
  echo "*** Set SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA before running servicetest ***" >&2
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

echo "*** Running transaction service preview test ***"

SERVICE_BIN="$SERVICE_BIN" \
SERVICE_NAME="$TRANSACTION_SERVICE_NAME" \
MANAGER_PATH="$TRANSACTION_SERVICE_MANAGER_PATH" \
MANAGER_METHOD="$TRANSACTION_SERVICE_START_METHOD" \
RESULT_METHOD="$TRANSACTION_SERVICE_RESULT_METHOD" \
INSTALL_SPEC="$INSTALL_SPEC" \
REINSTALL_SPEC="$REINSTALL_SPEC" \
LOG_FILE="$LOG_FILE" \
TIMEOUT_SECONDS="$TIMEOUT_SECONDS" \
dbus-run-session -- bash <<'EOF'
  set -e

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
  expected_summary_text="reinstalled"
  if [ -n "$INSTALL_SPEC" ]; then
    start_install="[\"$INSTALL_SPEC\"]"
    expected_summary_text="installed"
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

  deadline=$((SECONDS + TIMEOUT_SECONDS))
  while :; do
    result="$(gdbus call \
      --session \
      --dest "$SERVICE_NAME" \
      --object-path "$transaction_path" \
      --method "$RESULT_METHOD")"

    if printf "%s\n" "$result" | grep -Fq "'preview-running'"; then
      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "*** Timed out waiting for transaction preview result after ${TIMEOUT_SECONDS} seconds ***" >&2
        exit 1
      fi
      sleep 1
      continue
    fi

    if printf "%s\n" "$result" | grep -Fq "'preview-ready', true, true,"; then
      echo "$result"
      break
    fi

    echo "$result"
    echo "*** Transaction preview did not finish successfully ***" >&2
    exit 1
  done

  case "$result" in
    *"$expected_summary_text"* )
      ;;
    * )
      echo "*** Transaction preview result did not contain the expected summary text ***" >&2
      exit 1
      ;;
  esac

  echo "*** Service log ***"
  cat "$LOG_FILE"
EOF
