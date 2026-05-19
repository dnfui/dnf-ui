#!/usr/bin/env bash
set -e

# Session bus smoke test for forced preview worker failure.
# Confirms that an unexpected worker exception still ends in preview-failed
# instead of leaving the request stuck in preview-running. This uses the
# test-only transaction service binary.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$PROJECT_ROOT/utils/transaction_service_paths.conf"

SERVICE_BIN="${SERVICE_BIN:-$PROJECT_ROOT/$TRANSACTION_SERVICE_TEST_BIN_NAME}"
INSTALL_SPEC="${SERVICE_TEST_INSTALL_SPEC:-}"
TIMEOUT_SECONDS="${SERVICE_TEST_TIMEOUT_SECONDS:-180}"

if [ ! -x "$SERVICE_BIN" ]; then
  echo "*** Missing service binary: $SERVICE_BIN ***" >&2
  echo "*** Build it first with: ./utils/meson_build.sh service-tests ***" >&2
  exit 1
fi

if [ -z "$INSTALL_SPEC" ]; then
  echo "*** Set SERVICE_TEST_INSTALL_SPEC before running this test ***" >&2
  exit 1
fi

LOG_FILE="$(mktemp)"
cleanup_log() {
  rm -f "$LOG_FILE"
}
trap cleanup_log EXIT

echo "*** Running transaction service preview failure test ***"
echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"

SERVICE_BIN="$SERVICE_BIN" \
SERVICE_NAME="$TRANSACTION_SERVICE_NAME" \
MANAGER_PATH="$TRANSACTION_SERVICE_MANAGER_PATH" \
MANAGER_METHOD="$TRANSACTION_SERVICE_START_METHOD" \
RESULT_METHOD="$TRANSACTION_SERVICE_RESULT_METHOD" \
INSTALL_SPEC="$INSTALL_SPEC" \
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

  DNFUI_TEST_FORCE_PREVIEW_WORKER_EXCEPTION=1 "$SERVICE_BIN" --session >"$LOG_FILE" 2>&1 &
  service_pid=$!

  gdbus wait --session "$SERVICE_NAME" >/dev/null

  reply="$(gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$MANAGER_PATH" \
    --method "$MANAGER_METHOD" \
    "[\"$INSTALL_SPEC\"]" \
    "[]" \
    "[]")"

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
        echo "*** Timed out waiting for failed preview result after ${TIMEOUT_SECONDS} seconds ***" >&2
        echo "*** Last observed result: $result ***" >&2
        echo "*** Service log ***" >&2
        cat "$LOG_FILE" >&2 || true
        exit 1
      fi
      sleep 1
      continue
    fi

    if printf "%s\n" "$result" | grep -Fq "'preview-failed', true, false,"; then
      echo "$result"
      break
    fi

    echo "$result"
    echo "*** Transaction preview did not fail as expected ***" >&2
    exit 1
  done

  case "$result" in
    *"Forced transaction preview worker exception."* )
      ;;
    * )
      echo "*** Failed preview result did not contain the expected error text ***" >&2
      exit 1
      ;;
  esac

  echo "*** Service log ***"
  cat "$LOG_FILE"
EOF
