# -----------------------------------------------------------------------------
# Build configuration
# -----------------------------------------------------------------------------

MESON ?= meson
CONTAINER_RUNTIME ?= docker
export CONTAINER_RUNTIME

include utils/transaction_service_paths.conf

APP_BIN_NAME = dnfui
APP_BIN_DEST = /usr/bin/dnfui
TEST_BIN_NAME = dnfui-tests
MEMCHECK ?= ./utils/run_memcheck.sh
MEMCHECK_APP_ARGS ?=
MEMCHECK_SMOKE_FILTER ?= Transaction request validation rejects an empty request
MEMCHECK_SMOKE_TIMEOUT ?= 2m
MEMCHECK_TEST_FILTER ?=
MEMCHECK_TEST_TIMEOUT ?= 30m

ifeq ($(FINAL),y)
  MESON_BUILD_NAME = final
  MESON_BUILD_TYPE = release
  MESON_FINAL_BUILD = true
  MESON_WARNING_LEVEL = 3
else
  MESON_BUILD_NAME = debug
  MESON_BUILD_TYPE = debug
  MESON_FINAL_BUILD = false
  MESON_WARNING_LEVEL = 0
endif

ifeq ($(ASAN),y)
  MESON_SANITIZE = address
else
  MESON_SANITIZE = none
endif

ifeq ($(DEBUG_TRACE),y)
  MESON_DEBUG_TRACE = true
else
  MESON_DEBUG_TRACE = false
endif

ifneq ($(filter test $(TEST_BIN_NAME) $(TRANSACTION_SERVICE_TEST_BIN_NAME) $(TRANSACTION_SERVICE_SMOKE_CLIENT_BIN_NAME) servicetest servicecanceltest serviceapplytest servicesystemtest servicesystemapplytest servicesystemdisconnecttest memcheck memcheck-smoke memcheck-tests memory-check,$(MAKECMDGOALS)),)
  MESON_BUILD_TESTS = true
else
  MESON_BUILD_TESTS = false
endif

MESON_BUILD_ROOT = build
MESON_BUILD_DIR = $(MESON_BUILD_ROOT)/$(MESON_BUILD_NAME)
MESON_SETUP_ARGS = \
	--prefix /usr \
	--libexecdir libexec \
	--buildtype $(MESON_BUILD_TYPE) \
	-Dwarning_level=$(MESON_WARNING_LEVEL) \
	-Dbuild_tests=$(MESON_BUILD_TESTS) \
	-Ddebug_trace=$(MESON_DEBUG_TRACE) \
	-Dfinal_build=$(MESON_FINAL_BUILD) \
	-Db_sanitize=$(MESON_SANITIZE)

APP_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/src/$(APP_BIN_NAME)
SERVICE_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/src/service/$(TRANSACTION_SERVICE_BIN_NAME)
SERVICE_TEST_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/src/service/$(TRANSACTION_SERVICE_TEST_BIN_NAME)
TEST_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/test/$(TEST_BIN_NAME)
SERVICE_SMOKE_CLIENT_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/test/$(TRANSACTION_SERVICE_SMOKE_CLIENT_BIN_NAME)

# Service-only native install:
# - Transaction service binary
# - Polkit policy
# - D-Bus service and policy
# - systemd unit
SERVICE_INSTALL_FILES = \
	"$(TRANSACTION_SERVICE_BIN_DEST)" \
	"$(TRANSACTION_SERVICE_POLICY_DEST)" \
	"$(TRANSACTION_SERVICE_DBUS_SERVICE_DEST)" \
	"$(TRANSACTION_SERVICE_DBUS_POLICY_DEST)" \
	"$(TRANSACTION_SERVICE_SYSTEMD_UNIT_DEST)"

# Full native install:
# - Desktop app binary
# - All transaction service files above
APP_INSTALL_FILES = \
	"$(APP_BIN_DEST)" \
	$(SERVICE_INSTALL_FILES)

# -----------------------------------------------------------------------------
# Shared helper rules
# -----------------------------------------------------------------------------

# Require root for targets that change the live system:
define require_root
	@test "$$(id -u)" -eq 0 || { echo "*** $(1) must run as root ***" >&2; exit 1; }
endef

# Keep native build output owned by the regular user:
define require_non_root
	@test "$$(id -u)" -ne 0 || { echo "*** $(1) must run as a normal user. Build first, then use sudo make install/serviceinstall/uninstall/serviceuninstall as needed. ***" >&2; exit 1; }
endef

# Require a built artifact before install with no rebuild:
define require_built_file
	@test -e "$(1)" || { echo "*** Build target missing: $(1). Build as normal user first. ***" >&2; exit 1; }
endef

# Stop the transaction service if it is currently running:
define stop_transaction_service
	systemctl stop "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
endef

# Reload systemd, D-Bus, and Polkit state after service file changes:
define refresh_transaction_service_state
	systemctl daemon-reload
	systemctl reset-failed "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
	gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ReloadConfig >/dev/null
	systemctl reload polkit.service >/dev/null 2>&1 || true
endef

.DEFAULT_GOAL := all

# Print available Make targets:
.PHONY: help
help:
	@printf '\n%s\n' 'Native build and local run'
	@printf '%s\n' '----------------------------------------'
	@printf '%-36s %s\n' 'all' 'Build the desktop app and transaction service.'
	@printf '%-36s %s\n' 'meson-setup' 'Configure or reconfigure the active Meson build directory.'
	@printf '%-36s %s\n' 'dnfui' 'Build the desktop app binary.'
	@printf '%-36s %s\n' 'dnfui-service' 'Build the transaction service binary.'
	@printf '%-36s %s\n' 'dnfui-service-tests' 'Build the test-only transaction service binary.'
	@printf '%-36s %s\n' 'dnfui-service-smoke-client' 'Build the native system bus smoke-test client.'
	@printf '%-36s %s\n' 'dnfui-tests' 'Build the test binary.'
	@printf '%-36s %s\n' 'run' 'Build and run the desktop app locally.'
	@printf '%-36s %s\n' 'test' 'Build and run the local test suite.'
	@printf '%-36s %s\n' 'nativetests' 'Run the full native test matrix.'
	@printf '\n%s\n' 'Native install'
	@printf '%s\n' '----------------------------------------'
	@printf '%-36s %s\n' 'install' 'Install the app and transaction service files.'
	@printf '%-36s %s\n' 'uninstall' 'Remove the installed app and transaction service files.'
	@printf '%-36s %s\n' 'serviceinstall' 'Install only the transaction service files.'
	@printf '%-36s %s\n' 'serviceuninstall' 'Remove only the transaction service files.'
	@printf '\n%s\n' 'Native transaction service tests'
	@printf '%s\n' '----------------------------------------'
	@printf '%-36s %s\n' 'servicetest' 'Run the native session bus preview service test.'
	@printf '%-36s %s\n' 'servicecanceltest' 'Run the native session bus cancel service test.'
	@printf '%-36s %s\n' 'serviceapplytest' 'Run the native session bus apply service test.'
	@printf '%-36s %s\n' 'servicesystemtest' 'Run the native system bus preview service test.'
	@printf '%-36s %s\n' 'servicesystemapplytest' 'Run the native system bus apply service test.'
	@printf '%-36s %s\n' 'servicesystemdisconnecttest' 'Run the native system bus disconnect service test.'
	@printf '\n%s\n' 'Docker'
	@printf '%s\n' '----------------------------------------'
	@printf '%-36s %s\n' 'dockersetup' 'Build the Docker development image.'
	@printf '%-36s %s\n' 'dockerlogin' 'Open a shell in the Docker development container.'
	@printf '%-36s %s\n' 'dockerrun' 'Run the desktop app in Docker.'
	@printf '%-36s %s\n' 'dockerrunoffline' 'Run the desktop app in Docker without networking.'
	@printf '%-36s %s\n' 'dockerruncoldoffline' 'Run the desktop app in Docker without networking or repo cache.'
	@printf '%-36s %s\n' 'dockertest' 'Run the backend test suite in Docker.'
	@printf '%-36s %s\n' 'dockermemcheck' 'Run Valgrind memory checks in Docker.'
	@printf '%-36s %s\n' 'dockerofflinetest' 'Run the Docker offline smoke tests.'
	@printf '%-36s %s\n' 'dockertests' 'Run the full Docker test matrix.'
	@printf '%-36s %s\n' 'dockerservicetest' 'Run the Docker session bus preview service test.'
	@printf '%-36s %s\n' 'dockerservicepreviewfailuretest' 'Run the Docker forced preview failure service test.'
	@printf '%-36s %s\n' 'dockerserviceapplytest' 'Run the Docker session bus apply service test.'
	@printf '%-36s %s\n' 'dockerservicecanceltest' 'Run the Docker session bus cancel service test.'
	@printf '%-36s %s\n' 'dockerservicesystemtest' 'Run the Docker system bus preview service test.'
	@printf '%-36s %s\n' 'dockerservicesystemapplytest' 'Run the Docker system bus apply service test.'
	@printf '%-36s %s\n' 'dockerservicesystemdisconnecttest' 'Run the Docker system bus disconnect service test.'
	@printf '%-36s %s\n' 'dockergdb' 'Run the desktop app in Docker under GDB server.'
	@printf '%-36s %s\n' 'dockergdbstop' 'Stop the Docker GDB container.'
	@printf '\n%s\n' 'RPM packaging'
	@printf '%s\n' '----------------------------------------'
	@printf '%-36s %s\n' 'srpm' 'Build the source tarball and source RPM.'
	@printf '%-36s %s\n' 'rpm' 'Build binary and source RPMs.'
	@printf '%-36s %s\n' 'dockersrpm' 'Build the source RPM in Docker.'
	@printf '%-36s %s\n' 'dockerrpm' 'Build binary and source RPMs in Docker.'
	@printf '\n%s\n' 'Developer utilities'
	@printf '%s\n' '----------------------------------------'
	@printf '%-36s %s\n' 'memcheck' 'Run the default Valgrind Memcheck smoke test.'
	@printf '%-36s %s\n' 'memcheck-smoke' 'Run a quick automated Valgrind Memcheck smoke test.'
	@printf '%-36s %s\n' 'memcheck-tests' 'Run the automated test binary under Valgrind Memcheck.'
	@printf '%-36s %s\n' 'memory-check' 'Run the main automated memory checks.'
	@printf '%-36s %s\n' 'run-memcheck-tests' 'Run the shared automated Memcheck test command.'
	@printf '%-36s %s\n' 'memcheck-app' 'Run the desktop app under Valgrind Memcheck.'
	@printf '%-36s %s\n' 'valgrind' 'Alias for running the desktop app under Valgrind Memcheck.'
	@printf '%-36s %s\n' 'cppcheck' 'Run cppcheck on the source tree.'
	@printf '%-36s %s\n' 'indent' 'Run clang format in Docker.'
	@printf '%-36s %s\n' 'clean' 'Remove generated build output and symlinks.'
	@printf '%-36s %s\n' 'distclean' 'Remove generated build output, symlinks, and RPM artifacts.'

# -----------------------------------------------------------------------------
# Native build and local run targets
# -----------------------------------------------------------------------------

# Build both native runtime binaries in the active Meson directory:
.PHONY: all
all: dnfui dnfui-service

# Configure the active Meson build directory:
.PHONY: meson-setup
meson-setup:
	$(call require_non_root,native build targets)
	if [ -d "$(MESON_BUILD_DIR)" ]; then \
		$(MESON) setup "$(MESON_BUILD_DIR)" --reconfigure $(MESON_SETUP_ARGS); \
	else \
		$(MESON) setup "$(MESON_BUILD_DIR)" $(MESON_SETUP_ARGS); \
	fi

# Build the desktop app binary and update the local convenience symlink:
.PHONY: dnfui
dnfui: meson-setup
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnfui
	ln -sfn "$(APP_BUILD_PATH)" "$(APP_BIN_NAME)"

# Build the native transaction service binary and update the local symlink:
.PHONY: dnfui-service
dnfui-service: meson-setup
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnfui-service
	ln -sfn "$(SERVICE_BUILD_PATH)" "$(TRANSACTION_SERVICE_BIN_NAME)"

# Build the test-only transaction service binary and update the local symlink:
.PHONY: dnfui-service-tests
dnfui-service-tests: meson-setup
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnfui-service-tests
	ln -sfn "$(SERVICE_TEST_BUILD_PATH)" "$(TRANSACTION_SERVICE_TEST_BIN_NAME)"

# Build the native system bus smoke-test client and update the local symlink:
.PHONY: dnfui-service-smoke-client
dnfui-service-smoke-client: meson-setup
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnfui-service-smoke-client
	ln -sfn "$(SERVICE_SMOKE_CLIENT_BUILD_PATH)" "$(TRANSACTION_SERVICE_SMOKE_CLIENT_BIN_NAME)"

# Build the backend test binary and update the local symlink:
.PHONY: dnfui-tests
dnfui-tests: meson-setup
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnfui-tests
	ln -sfn "$(TEST_BUILD_PATH)" "$(TEST_BIN_NAME)"

# Run the app from the current build:
.PHONY: run
run: dnfui
	@./$(APP_BIN_NAME)

# Run the test suite:
.PHONY: test
test: dnfui-tests dnfui-service-tests
	@echo "*** Running test suite ***"
	@./$(TEST_BIN_NAME)

# Run the full native test matrix, including service smoke tests:
.PHONY: nativetests
nativetests:
	@bash ./utils/run_native_tests.sh

# -----------------------------------------------------------------------------
# Native install targets
# -----------------------------------------------------------------------------

# Install:
# - Full native install
# - Installs the desktop app plus all transaction service files
# - Can stage into DESTDIR without root
.PHONY: install
install:
	@if [ -z "$$DESTDIR" ] && [ "$$(id -u)" -ne 0 ]; then \
		echo "*** install must run as root unless DESTDIR is set ***" >&2; \
		exit 1; \
	fi
	$(call require_built_file,$(APP_BUILD_PATH))
	$(call require_built_file,$(SERVICE_BUILD_PATH))
ifeq ($(strip $(DESTDIR)),)
	$(call stop_transaction_service)
endif
	$(MESON) install -C "$(MESON_BUILD_DIR)" --no-rebuild --only-changed
ifeq ($(strip $(DESTDIR)),)
	$(call refresh_transaction_service_state)
endif

# Uninstall:
# - Full native uninstall
# - Removes the desktop app plus all transaction service files
.PHONY: uninstall
uninstall:
	$(call require_root,uninstall)
	$(call stop_transaction_service)
	rm -f $(APP_INSTALL_FILES)
	$(call refresh_transaction_service_state)
	@echo "*** Removed installed DNF UI runtime files. ***"

# Service install:
# - Native service-only install
# - Installs the transaction service binary and its system integration files
# - Does not install the desktop app binary
.PHONY: serviceinstall
serviceinstall:
	$(call require_root,serviceinstall)
	$(call require_built_file,$(SERVICE_BUILD_PATH))
	$(call stop_transaction_service)
	$(MESON) install -C "$(MESON_BUILD_DIR)" --no-rebuild --only-changed --tags transaction-service
	$(call refresh_transaction_service_state)
	@echo "*** Installed $(TRANSACTION_SERVICE_BIN_NAME) service files for native testing. ***"
	@echo "*** Run dnfui as a regular desktop user and apply a transaction to trigger the Polkit prompt. ***"

# Service uninstall:
# - Native service-only uninstall
# - Removes the transaction service files but leaves the desktop app binary intact
.PHONY: serviceuninstall
serviceuninstall:
	$(call require_root,serviceuninstall)
	$(call stop_transaction_service)
	rm -f $(SERVICE_INSTALL_FILES)
	$(call refresh_transaction_service_state)
	@echo "*** Removed native transaction service files. ***"

# -----------------------------------------------------------------------------
# Native transaction service smoke tests
# -----------------------------------------------------------------------------

# Session bus smoke test: preview flow from the locally built service binary
.PHONY: servicetest
servicetest: dnfui-service-tests
	@./test/functional/test_transaction_service_preview.sh

# Session bus smoke test: cancel flow from the locally built service binary
.PHONY: servicecanceltest
servicecanceltest: dnfui-service-tests
	@./test/functional/test_transaction_service_cancel.sh

# Session bus smoke test: apply flow from the locally built service binary
.PHONY: serviceapplytest
serviceapplytest: dnfui-service-tests
	@./test/functional/test_transaction_service_apply.sh

# System bus smoke test against the natively installed service
.PHONY: servicesystemtest
servicesystemtest: dnfui-service-smoke-client
	@./test/functional/test_transaction_service_system_bus.sh

# System bus smoke test: apply flow against the natively installed service
.PHONY: servicesystemapplytest
servicesystemapplytest: dnfui-service-smoke-client
	@SERVICE_SYSTEM_APPLY=yes ./test/functional/test_transaction_service_system_bus.sh

# System bus smoke test: disconnect flow against the natively installed service
.PHONY: servicesystemdisconnecttest
servicesystemdisconnecttest: dnfui-service-smoke-client
	@SERVICE_SYSTEM_DISCONNECT=yes ./test/functional/test_transaction_service_system_bus.sh

# -----------------------------------------------------------------------------
# Docker targets
# -----------------------------------------------------------------------------

# Build the Docker development image:
.PHONY: dockersetup
dockersetup:
	@./docker/docker_setup.sh

# Login to the Docker container:
.PHONY: dockerlogin
dockerlogin:
	@./docker/docker_login.sh

# Run the app in Docker:
.PHONY: dockerrun
dockerrun:
	@THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_build.sh

# Run the app in Docker with networking disabled:
.PHONY: dockerrunoffline
dockerrunoffline:
	@DOCKER_NETWORK_MODE=none THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_build.sh

# Run the app in Docker with networking disabled and an empty repo cache:
.PHONY: dockerruncoldoffline
dockerruncoldoffline:
	@DOCKER_NETWORK_MODE=none CACHE_MODE=empty THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_build.sh

# Run the backend test suite in Docker:
.PHONY: dockertest
dockertest:
	@./docker/docker_test.sh

# Run Valgrind memory checks in Docker:
.PHONY: dockermemcheck
dockermemcheck:
	@./docker/docker_memcheck.sh

# Run the offline Docker smoke tests after priming the repo cache online:
.PHONY: dockerofflinetest
dockerofflinetest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_offline_test.sh

# Run the full Docker-backed test matrix:
.PHONY: dockertests
dockertests:
	@bash ./docker/run_docker_tests.sh

# Run the session bus preview smoke test in Docker:
.PHONY: dockerservicetest
dockerservicetest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_test.sh

# Run the session bus forced preview failure smoke test in Docker:
.PHONY: dockerservicepreviewfailuretest
dockerservicepreviewfailuretest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_preview_failure_test.sh

# Run the session bus apply smoke test in Docker:
.PHONY: dockerserviceapplytest
dockerserviceapplytest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_apply_test.sh

# Run the session bus cancel smoke test in Docker:
.PHONY: dockerservicecanceltest
dockerservicecanceltest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_cancel_test.sh

# Run the system bus preview smoke test in Docker:
.PHONY: dockerservicesystemtest
dockerservicesystemtest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_test.sh

# Run the system bus apply smoke test in Docker:
.PHONY: dockerservicesystemapplytest
dockerservicesystemapplytest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_apply_test.sh

# Run the system bus disconnect smoke test in Docker:
.PHONY: dockerservicesystemdisconnecttest
dockerservicesystemdisconnecttest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_disconnect_test.sh

# Run the app in Docker under GDB server:
.PHONY: dockergdb
dockergdb:
	@THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" GDB_PORT="$(GDB_PORT)" ./docker/docker_gdb.sh

# Stop the Docker GDB container:
.PHONY: dockergdbstop
dockergdbstop:
	@./docker/docker_gdb_stop.sh

# -----------------------------------------------------------------------------
# RPM packaging targets
# -----------------------------------------------------------------------------

# Build the source tarball and source RPM in ./rpmbuild:
.PHONY: srpm
srpm:
	@./packaging/build_srpm.sh

# Build binary and source RPMs in ./rpmbuild:
.PHONY: rpm
rpm:
	@./packaging/build_rpm.sh

# Build the source RPM in Docker:
.PHONY: dockersrpm
dockersrpm:
	@./docker/docker_rpm_build.sh srpm

# Build binary and source RPMs in Docker:
.PHONY: dockerrpm
dockerrpm:
	@./docker/docker_rpm_build.sh rpm

# -----------------------------------------------------------------------------
# Developer utility targets
# -----------------------------------------------------------------------------

# Run a quick automated smoke test under Valgrind Memcheck:
.PHONY: memcheck
memcheck: memcheck-smoke

# Run a quick automated smoke test under Valgrind Memcheck:
.PHONY: memcheck-smoke
memcheck-smoke: dnfui-tests
	@MEMCHECK_TIMEOUT="$(MEMCHECK_SMOKE_TIMEOUT)" $(MEMCHECK) test ./$(TEST_BIN_NAME) "$(MEMCHECK_SMOKE_FILTER)"

# Run the automated test binary under Valgrind Memcheck:
.PHONY: memcheck-tests
memcheck-tests: dnfui-tests dnfui-service-tests
	@$(MAKE) run-memcheck-tests

# Run the main automated memory checks:
.PHONY: memory-check
memory-check: dnfui-tests dnfui-service-tests
	@$(MAKE) run-memcheck-tests

# Internal helper used by the public automated memory check targets:
.PHONY: run-memcheck-tests
run-memcheck-tests:
	@MEMCHECK_TIMEOUT="$(MEMCHECK_TEST_TIMEOUT)" $(MEMCHECK) test ./$(TEST_BIN_NAME) $(if $(strip $(MEMCHECK_TEST_FILTER)),"$(MEMCHECK_TEST_FILTER)")

# Run the desktop app under Valgrind Memcheck:
.PHONY: memcheck-app
memcheck-app: dnfui
	@$(MEMCHECK) app ./$(APP_BIN_NAME) $(MEMCHECK_APP_ARGS)

# Backward compatible alias for running the desktop app under Valgrind:
.PHONY: valgrind
valgrind: memcheck-app

# FIXME: Run cppcheck on the source tree:
.PHONY: cppcheck
cppcheck:
	@echo "*** Static code analysis"
	@cppcheck $(shell find src -name "*.cpp" -o -name "*.hpp") \
		--quiet --enable=all -DDEBUG=1 \
		--suppress=missingIncludeSystem \
		--suppress=unusedStructMember \
		--suppress=knownConditionTrueFalse

# Run clang format in Docker:
.PHONY: indent
indent:
	@echo "*** Formatting code"
	@./utils/docker-clang-format.sh

# Remove generated build output and symlinks:
.PHONY: clean
clean:
	$(RM) -r "$(MESON_BUILD_ROOT)"
	$(RM) "$(APP_BIN_NAME)" "$(TRANSACTION_SERVICE_BIN_NAME)" "$(TRANSACTION_SERVICE_TEST_BIN_NAME)" "$(TRANSACTION_SERVICE_SMOKE_CLIENT_BIN_NAME)" "$(TEST_BIN_NAME)"

# Remove everything:
.PHONY: distclean
distclean: clean
	$(RM) -r rpmbuild
	$(RM) dnf-ui-latest.rpm dnf-ui-latest.src.rpm
