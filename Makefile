# -----------------------------------------------------------------------------
# Build configuration
# -----------------------------------------------------------------------------

MESON ?= meson
CONTAINER_RUNTIME ?= docker
export CONTAINER_RUNTIME

APP_BIN_NAME = dnfui
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

ifneq ($(filter test $(TEST_BIN_NAME) memcheck memcheck-smoke memcheck-tests memory-check run-memcheck-tests,$(MAKECMDGOALS)),)
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
TEST_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/test/$(TEST_BIN_NAME)

# -----------------------------------------------------------------------------
# Shared helper rules
# -----------------------------------------------------------------------------

# Keep native build output owned by the regular user:
define require_non_root
	@test "$$(id -u)" -ne 0 || { echo "*** $(1) must run as a normal user. ***" >&2; exit 1; }
endef

.DEFAULT_GOAL := all

# Print available Make targets:
.PHONY: help
help:
	@printf '\n%s\n' 'Native build and local run'
	@printf '%s\n' '----------------------------------------'
	@printf '%-36s %s\n' 'all' 'Build the desktop app.'
	@printf '%-36s %s\n' 'meson-setup' 'Configure or reconfigure the active Meson build directory.'
	@printf '%-36s %s\n' 'dnfui' 'Build the desktop app binary.'
	@printf '%-36s %s\n' 'dnfui-tests' 'Build the test binary.'
	@printf '%-36s %s\n' 'run' 'Build and run the desktop app locally.'
	@printf '%-36s %s\n' 'test' 'Build and run the local test suite.'
	@printf '%-36s %s\n' 'dnf5daemontest' 'Run native dnf5daemon transaction client tests.'
	@printf '%-36s %s\n' 'srpm' 'Build the source tarball and source RPM.'
	@printf '%-36s %s\n' 'rpm' 'Build binary and source RPMs.'
	@printf '%-36s %s\n' 'nativeinstalltest' 'Build the RPM and reinstall it on the native system.'
	@printf '\n%s\n' 'Docker'
	@printf '%s\n' '----------------------------------------'
	@printf '%-36s %s\n' 'dockersetup' 'Build the Docker development image.'
	@printf '%-36s %s\n' 'dockerlogin' 'Open a shell in the Docker development container.'
	@printf '%-36s %s\n' 'dockerrun' 'Run the desktop app in Docker.'
	@printf '%-36s %s\n' 'dockertest' 'Run the backend test suite in Docker.'
	@printf '%-36s %s\n' 'dockerrunoffline' 'Run the desktop app in Docker without networking.'
	@printf '%-36s %s\n' 'dockerruncoldoffline' 'Run the desktop app in Docker without networking or repo cache.'
	@printf '%-36s %s\n' 'dockerofflinetest' 'Run the Docker offline smoke tests.'
	@printf '%-36s %s\n' 'dockerdnf5daemontest' 'Run dnf5daemon transaction client tests in Docker.'
	@printf '%-36s %s\n' 'dockergdb' 'Run the desktop app under GDB in Docker.'
	@printf '%-36s %s\n' 'dockergdbstop' 'Stop the Docker GDB container if it is still running.'
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

# Build the native desktop app binary in the active Meson directory:
.PHONY: all
all: dnfui

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
test: dnfui-tests
	@echo "*** Running test suite ***"
	@./$(TEST_BIN_NAME)

# Run native dnf5daemon transaction client tests:
.PHONY: dnf5daemontest
dnf5daemontest:
	@./utils/native_dnf5daemon_test.sh

# Build the source RPM from tracked files:
.PHONY: srpm
srpm:
	@./packaging/build_srpm.sh

# Build binary and source RPMs from tracked files:
.PHONY: rpm
rpm:
	@./packaging/build_rpm.sh

# Build and reinstall the RPM on the native system:
.PHONY: nativeinstalltest
nativeinstalltest:
	$(call require_non_root,native install test)
	@./utils/native_install_test.sh

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

# Run the backend test suite in Docker:
.PHONY: dockertest
dockertest:
	@$(CONTAINER_RUNTIME) run --rm \
		--name dnfui-test \
		--init \
		-w /workspace \
		-e FINAL \
		-e ASAN \
		-e DEBUG_TRACE \
		-e DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build \
		-v "$(CURDIR):/workspace" \
		dnfui-dev \
		bash -lc 'BUILD_DIR="$$(./utils/meson_build.sh build-dir)" && ./utils/meson_build.sh tests && "$$BUILD_DIR/test/dnfui-tests" "~[dnf5daemon]" "~[offline]"'

# Run the app in Docker with networking disabled:
.PHONY: dockerrunoffline
dockerrunoffline:
	@DOCKER_NETWORK_MODE=none THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_build.sh

# Run the app in Docker with networking disabled and an empty repo cache:
.PHONY: dockerruncoldoffline
dockerruncoldoffline:
	@DOCKER_NETWORK_MODE=none CACHE_MODE=empty THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_build.sh

# Run the offline Docker smoke tests after priming the repo cache online:
.PHONY: dockerofflinetest
dockerofflinetest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_offline_test.sh

# Run dnf5daemon transaction client tests in Docker:
.PHONY: dockerdnf5daemontest
dockerdnf5daemontest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_dnf5daemon_test.sh

# Run the app under GDB in Docker:
.PHONY: dockergdb
dockergdb:
	@THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_gdb.sh

# Stop the Docker GDB container:
.PHONY: dockergdbstop
dockergdbstop:
	@./docker/docker_gdb_stop.sh

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

# Run the test binary under Valgrind Memcheck:
.PHONY: memcheck-tests
memcheck-tests: run-memcheck-tests

# Run the main automated memory checks:
.PHONY: memory-check
memory-check: memcheck-tests

# Run the shared automated Memcheck test command:
.PHONY: run-memcheck-tests
run-memcheck-tests: dnfui-tests
	@if [ -n "$(MEMCHECK_TEST_FILTER)" ]; then \
		MEMCHECK_TIMEOUT="$(MEMCHECK_TEST_TIMEOUT)" $(MEMCHECK) test ./$(TEST_BIN_NAME) "$(MEMCHECK_TEST_FILTER)"; \
	else \
		MEMCHECK_TIMEOUT="$(MEMCHECK_TEST_TIMEOUT)" $(MEMCHECK) test ./$(TEST_BIN_NAME); \
	fi

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
	$(RM) "$(APP_BIN_NAME)" "$(TEST_BIN_NAME)"

# Remove everything:
.PHONY: distclean
distclean: clean
	$(RM) -r rpmbuild
	$(RM) dnf-ui-latest.rpm dnf-ui-latest.src.rpm
