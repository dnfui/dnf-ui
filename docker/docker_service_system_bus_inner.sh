#!/usr/bin/env bash
set -e

# Inner system bus smoke test used by the Docker service wrappers. It stages the
# policy files, starts a private bus and polkitd, then exercises preview, apply,
# and orphan cleanup through the installed service contract.

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$PROJECT_ROOT/utils/transaction_service_paths.conf"

TEST_USER="dnfuitest"
INSTALL_SPEC="${SERVICE_TEST_INSTALL_SPEC:-}"
REINSTALL_SPEC="${SERVICE_TEST_REINSTALL_NEVRA:-}"
ALLOW_APPLY="${SERVICE_SYSTEM_BUS_ALLOW_APPLY:-}"
DISCONNECT_MODE="${SERVICE_SYSTEM_BUS_DISCONNECT:-}"
TIMEOUT_SECONDS="${SERVICE_TEST_TIMEOUT_SECONDS:-180}"
SERVICE_BIN="${SERVICE_BIN:-$PROJECT_ROOT/$TRANSACTION_SERVICE_TEST_BIN_NAME}"
POLICY_FILE="$PROJECT_ROOT/$TRANSACTION_SERVICE_POLICY_FILE"
BUS_POLICY_FILE="$PROJECT_ROOT/$TRANSACTION_SERVICE_DBUS_POLICY_FILE"
RULES_FILE="/etc/polkit-1/rules.d/49-com.fedora.dnfui-test.rules"

if [ "$(id -u)" -ne 0 ]; then
  echo "*** docker_service_system_bus_inner.sh must run as root ***" >&2
  exit 1
fi

if [ ! -x "$SERVICE_BIN" ]; then
  echo "*** Missing service binary: $SERVICE_BIN ***" >&2
  exit 1
fi

if [ ! -f "$POLICY_FILE" ] || [ ! -f "$BUS_POLICY_FILE" ]; then
  echo "*** Missing service packaging files in $PROJECT_ROOT/packaging ***" >&2
  exit 1
fi

if [ -z "$INSTALL_SPEC" ] && [ -z "$REINSTALL_SPEC" ]; then
  echo "*** Set SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA before running this test ***" >&2
  exit 1
fi

if [ -n "$INSTALL_SPEC" ] && [ -n "$REINSTALL_SPEC" ]; then
  echo "*** Set only one of SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA ***" >&2
  exit 1
fi

if [ -n "$ALLOW_APPLY" ] && [ -n "$DISCONNECT_MODE" ]; then
  echo "*** Set only one of SERVICE_SYSTEM_BUS_ALLOW_APPLY or SERVICE_SYSTEM_BUS_DISCONNECT ***" >&2
  exit 1
fi

SERVICE_LOG="$(mktemp)"
POLKIT_LOG="$(mktemp)"

# Keep service and polkit logs until cleanup so failures can print useful
# diagnostics from the private bus test.
cleanup() {
  if [ -n "${service_pid:-}" ]; then
    kill "$service_pid" >/dev/null 2>&1 || true
    wait "$service_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${polkit_pid:-}" ]; then
    kill "$polkit_pid" >/dev/null 2>&1 || true
    wait "$polkit_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${dbus_pid:-}" ]; then
    kill "$dbus_pid" >/dev/null 2>&1 || true
    wait "$dbus_pid" >/dev/null 2>&1 || true
  fi
  rm -f "$SERVICE_LOG" "$POLKIT_LOG"
}
trap cleanup EXIT

print_logs() {
  echo "*** Service log ***"
  cat "$SERVICE_LOG" || true

  echo "*** Polkit log ***"
  cat "$POLKIT_LOG" || true
}

# Preview and apply finish after the D-Bus method returns. Poll the Result
# method until the request reaches the expected final state or fails.
wait_for_result() {
  local transaction_path="$1"
  local expected_stage="$2"
  local expected_success="$3"
  local deadline="$((SECONDS + TIMEOUT_SECONDS))"
  local result=""

  while :; do
    result="$(runuser -u "$TEST_USER" -- \
      env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
      gdbus call \
      --system \
      --dest "$TRANSACTION_SERVICE_NAME" \
      --object-path "$transaction_path" \
      --method "$TRANSACTION_SERVICE_RESULT_METHOD")"

    if printf "%s\n" "$result" | grep -Fq "'$expected_stage', true, $expected_success,"; then
      printf "%s\n" "$result"
      return 0
    fi

    if printf "%s\n" "$result" | grep -Fq "'preview-running'"; then
      :
    elif printf "%s\n" "$result" | grep -Fq "'apply-running'"; then
      :
    else
      printf "%s\n" "$result"
      echo "*** Transaction did not reach the expected result state ***" >&2
      print_logs >&2
      return 1
    fi

    if [ "$SECONDS" -ge "$deadline" ]; then
      echo "*** Timed out waiting for transaction service result after ${TIMEOUT_SECONDS} seconds ***" >&2
      echo "*** Last observed result: $result ***" >&2
      print_logs >&2
      return 1
    fi

    sleep 1
  done
}

# Release is checked from a separate process. A live request can still deny that
# process while ownership cleanup is pending, so AccessDenied is retried.
wait_for_release() {
  local transaction_path="$1"
  local deadline="$((SECONDS + TIMEOUT_SECONDS))"
  local result=""
  local status=0

  while :; do
    set +e
    result="$(runuser -u "$TEST_USER" -- \
      env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
      gdbus call \
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
          # A different gdbus process is not the request owner, so it may be
          # denied until cleanup removes the request object.
          ;;
        * )
          printf "%s\n" "$result"
          echo "*** Transaction request did not fail with the expected release error ***" >&2
          print_logs >&2
          return 1
          ;;
      esac

      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "*** Timed out waiting for released transaction request to disappear after ${TIMEOUT_SECONDS} seconds ***" >&2
        echo "*** Last observed result: $result ***" >&2
        print_logs >&2
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
      print_logs >&2
      return 1
    fi

    if [ "$SECONDS" -ge "$deadline" ]; then
      echo "*** Timed out waiting for released transaction request to disappear after ${TIMEOUT_SECONDS} seconds ***" >&2
      echo "*** Last observed result: $result ***" >&2
      print_logs >&2
      return 1
    fi

    sleep 1
  done
}

if ! id "$TEST_USER" >/dev/null 2>&1; then
  useradd -m "$TEST_USER"
fi

# Install files into their packaged system locations inside the disposable
# container so the smoke test exercises the same service contract as install.
install -D -m 0755 "$SERVICE_BIN" "$TRANSACTION_SERVICE_BIN_DEST"
install -D -m 0644 "$POLICY_FILE" "$TRANSACTION_SERVICE_POLICY_DEST"
install -D -m 0644 "$BUS_POLICY_FILE" "$TRANSACTION_SERVICE_DBUS_POLICY_DEST"

if [ -n "$ALLOW_APPLY" ]; then
  # The apply test needs a deterministic polkit answer for the test user.
  install -d /etc/polkit-1/rules.d
  cat >"$RULES_FILE" <<'EOF'
polkit.addRule(function(action, subject) {
  if (action.id == "com.fedora.dnfui.apply-transactions" &&
      subject.user == "dnfuitest") {
    return polkit.Result.YES;
  }

  return polkit.Result.NOT_HANDLED;
});
EOF
fi

mkdir -p /run/dbus
dbus-uuidgen --ensure=/etc/machine-id

# Start a private system bus instead of relying on any bus from the container
# host environment.
dbus_info="$(dbus-daemon --system --fork --nopidfile --print-address=1 --print-pid=1)"
DBUS_SYSTEM_BUS_ADDRESS="$(printf "%s\n" "$dbus_info" | sed -n '1p')"
dbus_pid="$(printf "%s\n" "$dbus_info" | sed -n '2p')"
export DBUS_SYSTEM_BUS_ADDRESS

/usr/lib/polkit-1/polkitd --no-debug >"$POLKIT_LOG" 2>&1 &
polkit_pid=$!

"$SERVICE_BIN" --system >"$SERVICE_LOG" 2>&1 &
service_pid=$!

if [ -n "$ALLOW_APPLY" ]; then
  echo "*** Running transaction service system bus apply test ***"
  echo "*** NOTE: This test applies a real package transaction inside the disposable Docker container. ***"
  if [ -n "$INSTALL_SPEC" ]; then
    echo "*** Package spec: $INSTALL_SPEC ***"
  else
    echo "*** Package spec: $REINSTALL_SPEC ***"
  fi
  echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"
elif [ -n "$DISCONNECT_MODE" ]; then
  echo "*** Running transaction service system bus disconnect test ***"
  echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"
elif [ -n "$INSTALL_SPEC" ]; then
  echo "*** Running transaction service system bus install preview test ***"
  echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"
else
  echo "*** Running transaction service system bus test ***"
  echo "*** Result timeout: ${TIMEOUT_SECONDS} seconds ***"
fi

start_install="[]"
start_remove="[]"
start_reinstall="[\"$REINSTALL_SPEC\"]"
if [ -n "$INSTALL_SPEC" ]; then
  start_install="[\"$INSTALL_SPEC\"]"
  start_reinstall="[]"
fi

gdbus wait --system "$TRANSACTION_SERVICE_NAME" >/dev/null

reply="$(runuser -u "$TEST_USER" -- \
  env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
  gdbus call \
  --system \
  --dest "$TRANSACTION_SERVICE_NAME" \
  --object-path "$TRANSACTION_SERVICE_MANAGER_PATH" \
  --method "$TRANSACTION_SERVICE_START_METHOD" \
  "$start_install" \
  "$start_remove" \
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

if [ -n "$DISCONNECT_MODE" ]; then
  echo "*** The StartTransaction caller has disconnected. Waiting for automatic cleanup. ***"
  released_result="$(wait_for_release "$transaction_path")"
  echo "$released_result"
  print_logs
  exit 0
fi

preview_result="$(wait_for_result "$transaction_path" "preview-ready" "true")"
echo "$preview_result"

preview_data="$(runuser -u "$TEST_USER" -- \
  env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
  gdbus call \
  --system \
  --dest "$TRANSACTION_SERVICE_NAME" \
  --object-path "$transaction_path" \
  --method "$TRANSACTION_SERVICE_PREVIEW_METHOD")"
echo "$preview_data"

expected_preview_spec="$REINSTALL_SPEC"
if [ -n "$INSTALL_SPEC" ]; then
  expected_preview_spec="$INSTALL_SPEC"
fi

case "$preview_data" in
  *"$expected_preview_spec"* )
    ;;
  * )
    echo "*** Structured preview did not contain the expected package spec ***" >&2
    print_logs >&2
    exit 1
    ;;
esac

if [ -n "$ALLOW_APPLY" ]; then
  runuser -u "$TEST_USER" -- \
    env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
    gdbus call \
    --system \
    --dest "$TRANSACTION_SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$TRANSACTION_SERVICE_APPLY_METHOD" >/dev/null

  apply_result="$(wait_for_result "$transaction_path" "apply-succeeded" "true")"
  echo "$apply_result"

  case "$apply_result" in
    *"Transaction applied successfully."* )
      ;;
    * )
      echo "*** Transaction apply result did not contain the expected success text ***" >&2
      print_logs >&2
      exit 1
      ;;
  esac

  runuser -u "$TEST_USER" -- \
    env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
    gdbus call \
    --system \
    --dest "$TRANSACTION_SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$TRANSACTION_SERVICE_RELEASE_METHOD" >/dev/null

  set +e
  released_result="$(runuser -u "$TEST_USER" -- \
    env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
    gdbus call \
    --system \
    --dest "$TRANSACTION_SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$TRANSACTION_SERVICE_RESULT_METHOD" 2>&1)"
  released_status=$?
  set -e

  if [ "$released_status" -eq 0 ]; then
    echo "*** Released transaction request is still reachable ***" >&2
    print_logs >&2
    exit 1
  fi
else
  set +e
  apply_output="$(runuser -u "$TEST_USER" -- \
    env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
    gdbus call \
    --system \
    --dest "$TRANSACTION_SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$TRANSACTION_SERVICE_APPLY_METHOD" 2>&1)"
  apply_status=$?
  set -e

  echo "$apply_output"

  if [ "$apply_status" -eq 0 ]; then
    echo "*** Unprivileged apply unexpectedly succeeded on the system bus ***" >&2
    exit 1
  fi

  case "$apply_output" in
    *"AccessDenied"* )
      ;;
    *"Not authorized to apply package transactions."* )
      ;;
    * )
      echo "*** Apply did not fail with the expected authorization error ***" >&2
      print_logs >&2
      exit 1
      ;;
  esac
fi

print_logs
