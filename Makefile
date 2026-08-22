ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TARGET := esp32s3
BUILD_DIR ?= $(ROOT_DIR)/build/$(TARGET)
SDKCONFIG ?= $(BUILD_DIR)/sdkconfig
IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_PYTHON_ENV_PATH ?= $(firstword $(wildcard $(HOME)/.espressif/python_env/idf5.4*_env))
PORT ?=
DURATION ?= 30
WEB_INSTALLER_DIR ?= $(ROOT_DIR)/build/web-installer
LAB_BUILD_DIR ?= $(ROOT_DIR)/build/$(TARGET)-awdl-tx-lab
LAB_SDKCONFIG ?= $(LAB_BUILD_DIR)/sdkconfig

.DEFAULT_GOAL := help

define run_idf
	@bash -lc 'set -eo pipefail; \
		test -f "$(IDF_PATH)/export.sh" || { echo "ESP-IDF not found at $(IDF_PATH)"; exit 1; }; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(BUILD_DIR)" \
			-D SDKCONFIG="$(SDKCONFIG)" $(1)'
endef

.PHONY: help build reconfigure clean fullclean flash monitor flash-monitor ports size test test-hardware-awdl web-installer lab-awdl-tx-build lab-awdl-tx-flash lab-awdl-tx-test

help:
	@printf '%s\n' \
		'espDrop targets:' \
		'  make test                  Run host-side core and TapDrop tests' \
		'  make build                 Build ESP32-S3 firmware' \
		'  make flash PORT=/dev/...   Flash a connected ESP32-S3' \
		'  make monitor PORT=/dev/... Open the serial monitor' \
		'  make flash-monitor PORT=... Build, flash, and monitor' \
		'  make ports                 Identify attached Espressif USB ports' \
		'  make test-hardware-awdl PORT=... [DURATION=30]' \
		'  make lab-awdl-tx-flash PORT=...  Flash bounded 15 s TX experiment' \
		'  make lab-awdl-tx-test PORT=...   Flash and capture TX experiment' \
		'  make web-installer         Stage the GitHub Pages web flasher' \
		'  make size                  Show firmware size information'

build:
	$(call run_idf,build)

reconfigure:
	$(call run_idf,reconfigure)

clean:
	$(call run_idf,clean)

fullclean:
	$(call run_idf,fullclean)

flash:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	$(call run_idf,-p "$(PORT)" flash)

monitor:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	$(call run_idf,-p "$(PORT)" monitor)

flash-monitor:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	$(call run_idf,-p "$(PORT)" flash monitor)

ports:
	@python3 "$(ROOT_DIR)/scripts/list_esp_ports.py"

size:
	$(call run_idf,size)

test:
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/peer_table.c" \
		"$(ROOT_DIR)/tests/test_peer_table.c" \
		-o "$(BUILD_DIR)/host-tests/test_peer_table"
	@"$(BUILD_DIR)/host-tests/test_peer_table"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		-I"$(ROOT_DIR)/tapdrop/include" \
		"$(ROOT_DIR)/tapdrop/src/correlation.c" \
		"$(ROOT_DIR)/tests/test_tapdrop_correlation.c" \
		-o "$(BUILD_DIR)/host-tests/test_tapdrop_correlation"
	@"$(BUILD_DIR)/host-tests/test_tapdrop_correlation"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/awdl_frame.c" \
		"$(ROOT_DIR)/tests/test_awdl_frame.c" \
		-o "$(BUILD_DIR)/host-tests/test_awdl_frame"
	@"$(BUILD_DIR)/host-tests/test_awdl_frame"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/awdl_frame.c" \
		"$(ROOT_DIR)/core/src/awdl_tlv.c" \
		"$(ROOT_DIR)/tests/test_awdl_tlv.c" \
		-o "$(BUILD_DIR)/host-tests/test_awdl_tlv"
	@"$(BUILD_DIR)/host-tests/test_awdl_tlv"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/awdl_frame.c" \
		"$(ROOT_DIR)/core/src/awdl_tlv.c" \
		"$(ROOT_DIR)/core/src/awdl_tx.c" \
		"$(ROOT_DIR)/tests/test_awdl_tx.c" \
		-o "$(BUILD_DIR)/host-tests/test_awdl_tx"
	@"$(BUILD_DIR)/host-tests/test_awdl_tx"

test-hardware-awdl:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@python3 "$(ROOT_DIR)/scripts/capture_awdl_probe.py" \
		--port "$(PORT)" \
		--seconds "$(DURATION)" \
		--output "$(BUILD_DIR)/hardware/awdl-probe.json"

lab-awdl-tx-build:
	@bash -lc 'set -eo pipefail; \
		test -f "$(IDF_PATH)/export.sh"; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(LAB_BUILD_DIR)" \
			-D SDKCONFIG="$(LAB_SDKCONFIG)" \
			-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.awdl-tx-lab.defaults" build'

lab-awdl-tx-flash: lab-awdl-tx-build
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@bash -lc 'set -eo pipefail; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(LAB_BUILD_DIR)" \
			-D SDKCONFIG="$(LAB_SDKCONFIG)" -p "$(PORT)" flash'

lab-awdl-tx-test: lab-awdl-tx-flash
	@python3 "$(ROOT_DIR)/scripts/capture_awdl_probe.py" \
		--port "$(PORT)" \
		--seconds "$(DURATION)" \
		--output "$(LAB_BUILD_DIR)/hardware/awdl-tx-lab.json"

web-installer: build
	@python3 "$(ROOT_DIR)/scripts/stage_web_installer.py" \
		--build "$(BUILD_DIR)" \
		--output-dir "$(WEB_INSTALLER_DIR)"
