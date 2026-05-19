#!/usr/bin/env bash
set -e

# Native system bus smoke test for the installed transaction service. The apply
# mode is meant to be run as a regular desktop user so Polkit can prompt.

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$PROJECT_ROOT/utils/transaction_service_paths.conf"

SMOKE_CLIENT_BIN="${SMOKE_CLIENT_BIN:-$PROJECT_ROOT/$TRANSACTION_SERVICE_SMOKE_CLIENT_BIN_NAME}"
APPLY_MODE="${SERVICE_SYSTEM_APPLY:-}"
DISCONNECT_MODE="${SERVICE_SYSTEM_DISCONNECT:-}"
INSTALL_SPEC="${SERVICE_TEST_INSTALL_SPEC:-}"
REINSTALL_SPEC="${SERVICE_TEST_REINSTALL_NEVRA:-}"
TIMEOUT_SECONDS="${SERVICE_TEST_TIMEOUT_SECONDS:-180}"

if [ ! -x "$SMOKE_CLIENT_BIN" ]; then
  echo "*** Missing smoke-test client binary: $SMOKE_CLIENT_BIN ***" >&2
  echo "*** Build it first with: make dnfui-service-smoke-client ***" >&2
  exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
  echo "*** Run this test as a regular user, not as root. ***" >&2
  exit 1
fi

if [ -z "$INSTALL_SPEC" ] && [ -z "$REINSTALL_SPEC" ]; then
  echo "*** Set SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA before running this test. ***" >&2
  exit 1
fi

if [ -n "$INSTALL_SPEC" ] && [ -n "$REINSTALL_SPEC" ]; then
  echo "*** Set only one of SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA ***" >&2
  exit 1
fi

if [ -n "$APPLY_MODE" ] && [ -n "$DISCONNECT_MODE" ]; then
  echo "*** Set only one of SERVICE_SYSTEM_APPLY or SERVICE_SYSTEM_DISCONNECT ***" >&2
  exit 1
fi

wait_for_release() {
  local transaction_path="$1"
  local deadline="$((SECONDS + TIMEOUT_SECONDS))"
  local result=""
  local status=0

  while :; do
    set +e
    result="$(gdbus call \
      --system \
      --dest "$TRANSACTION_SERVICE_NAME" \
      --object-path "$transaction_path" \
      --method "$TRANSACTION_SERVICE_RESULT_METHOD" 2>&1)"
    status=$?
    set -e

    if [ "$status" -ne 0 ]; then
      case "$result" in
        *"UnknownObject"* | *"UnknownMethod"* | *"No such interface"* )
          printf "%s\n" "$result"
          return 0
          ;;
        *"AccessDenied"* )
          # A different client owns the request object until cleanup removes it.
          ;;
        * )
          printf "%s\n" "$result"
          echo "*** Transaction request did not fail with the expected release error ***" >&2
          return 1
          ;;
      esac

      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "*** Timed out waiting for released transaction request to disappear after ${TIMEOUT_SECONDS} seconds ***" >&2
        echo "*** Last observed result: $result ***" >&2
        return 1
      fi

      sleep 1
      continue
    fi

    if printf "%s\n" "$result" | grep -Fq "'preview-running'"; then
      :
    elif printf "%s\n" "$result" | grep -Fq "'cancelled'"; then
      :
    elif printf "%s\n" "$result" | grep -Fq "'preview-ready'"; then
      :
    else
      printf "%s\n" "$result"
      echo "*** Transaction request stayed reachable in an unexpected state ***" >&2
      return 1
    fi

    if [ "$SECONDS" -ge "$deadline" ]; then
      echo "*** Timed out waiting for released transaction request to disappear after ${TIMEOUT_SECONDS} seconds ***" >&2
      echo "*** Last observed result: $result ***" >&2
      return 1
    fi

    sleep 1
  done
}

run_smoke_client_with_timeout() {
  if command -v timeout >/dev/null 2>&1; then
    timeout --foreground "${TIMEOUT_SECONDS}s" "$SMOKE_CLIENT_BIN" "$@"
    return $?
  fi

  "$SMOKE_CLIENT_BIN" "$@"
}

client_args=()
if [ -n "$APPLY_MODE" ]; then
  client_args+=(--apply)
elif [ -n "$DISCONNECT_MODE" ]; then
  client_args+=(--disconnect)
else
  client_args+=(--preview)
fi

if [ -n "$INSTALL_SPEC" ]; then
  client_args+=(--install-spec "$INSTALL_SPEC")
else
  client_args+=(--reinstall-spec "$REINSTALL_SPEC")
fi

if [ -n "$APPLY_MODE" ]; then
  echo "*** Running native transaction service apply test ***"
  echo "*** WARNING: This test applies a real package transaction on the current system. ***"
  if [ -n "$INSTALL_SPEC" ]; then
    echo "*** Package spec: $INSTALL_SPEC ***"
  else
    echo "*** Package spec: $REINSTALL_SPEC ***"
  fi
  echo "*** Use only a harmless package for install tests. ***"
  echo "*** Use only a non critical installed package for reinstall tests. ***"
  echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"
elif [ -n "$DISCONNECT_MODE" ]; then
  echo "*** Running native transaction service disconnect test ***"
  echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"
else
  echo "*** Running native transaction service preview test ***"
  echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"
fi

if [ -n "$DISCONNECT_MODE" ]; then
  transaction_path="$("$SMOKE_CLIENT_BIN" "${client_args[@]}")"
  echo "$transaction_path"
  if [ -z "$transaction_path" ]; then
    echo "*** Failed to read transaction object path from the smoke-test client ***" >&2
    exit 1
  fi

  echo "*** The StartTransaction caller has disconnected. Waiting for automatic cleanup. ***"
  released_result="$(wait_for_release "$transaction_path")"
  echo "$released_result"
  exit 0
fi

set +e
run_smoke_client_with_timeout "${client_args[@]}"
status=$?
set -e

if [ "$status" -eq 124 ]; then
  echo "*** Timed out waiting for transaction service result after ${TIMEOUT_SECONDS} seconds ***" >&2
  exit 1
fi

exit "$status"
