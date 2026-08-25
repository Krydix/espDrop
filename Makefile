ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TARGET := esp32s3
BUILD_DIR ?= $(ROOT_DIR)/build/$(TARGET)
SDKCONFIG ?= $(BUILD_DIR)/sdkconfig
IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_PYTHON_ENV_PATH ?= $(firstword $(wildcard $(HOME)/.espressif/python_env/idf5.4*_env))
PORT ?=
DURATION ?= 30
ESP_SERIAL ?=
OTA_HOST ?=
OTA_PORT ?= 0
WEB_INSTALLER_DIR ?= $(ROOT_DIR)/build/web-installer
LAB_BUILD_DIR ?= $(ROOT_DIR)/build/$(TARGET)-awdl-tx-lab
LAB_SDKCONFIG ?= $(LAB_BUILD_DIR)/sdkconfig
DIRECT_PEER_LAB_BUILD_DIR ?= $(ROOT_DIR)/build/$(TARGET)-awdl-direct-peer-lab
DIRECT_PEER_LAB_SDKCONFIG ?= $(DIRECT_PEER_LAB_BUILD_DIR)/sdkconfig
AIRDROP_RELAY_LAB_BUILD_DIR ?= $(ROOT_DIR)/build/$(TARGET)-airdrop-relay-lab
AIRDROP_RELAY_LAB_SDKCONFIG ?= $(AIRDROP_RELAY_LAB_BUILD_DIR)/sdkconfig
AIRDROP_RECEIVER_ORACLE_BUILD_DIR ?= $(ROOT_DIR)/build/$(TARGET)-airdrop-receiver-oracle
AIRDROP_RECEIVER_ORACLE_SDKCONFIG ?= $(AIRDROP_RECEIVER_ORACLE_BUILD_DIR)/sdkconfig

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

.PHONY: help build reconfigure clean fullclean flash monitor flash-monitor ports size test test-host-cli test-hardware-awdl web-installer provision-wifi ota-trigger ota-local relay-upload relay-ping relay-stats relay-wake relay-peers relay-target relay-restart relay-send lab-awdl-tx-build lab-awdl-tx-flash lab-awdl-tx-test lab-awdl-direct-peer-build lab-awdl-direct-peer-flash lab-awdl-direct-peer-test lab-awdl-distance-one-build lab-awdl-distance-one-flash lab-awdl-distance-one-test lab-airdrop-relay-build lab-airdrop-relay-flash lab-airdrop-relay-test lab-airdrop-relay-live lab-airdrop-receiver-oracle-build lab-airdrop-receiver-oracle-flash lab-airdrop-receiver-oracle-test

help:
	@printf '%s\n' \
		'espDrop targets:' \
		'  make test                  Run host-side core and TapDrop tests' \
		'  make build                 Build ESP32-S3 firmware' \
		'  make flash PORT=/dev/...   Flash a connected ESP32-S3' \
		'  make monitor PORT=/dev/... Open the serial monitor' \
		'  make flash-monitor PORT=... Build, flash, and monitor' \
		'  make ports                 Identify attached Espressif USB ports' \
		'  make relay-upload PORT=... ESP_SERIAL=... FILE=...  Stage a file over USB' \
		'  make relay-ping PORT=... ESP_SERIAL=...                  Report live firmware uptime' \
		'  make relay-stats PORT=... ESP_SERIAL=...                 Report AWDL/TCP counters' \
		'  make relay-wake PORT=... ESP_SERIAL=...                  Re-arm AirDrop BLE wake' \
		'  make relay-peers PORT=... ESP_SERIAL=...                 List AirDrop candidates' \
		'  make relay-target PORT=... ESP_SERIAL=... PEER=...       Select receiver' \
		'  make relay-restart PORT=... ESP_SERIAL=...                Restart without flashing' \
		'  make relay-send PORT=... ESP_SERIAL=... PEER=... FILE=...  Select and stream live' \
		'  make lab-airdrop-relay-live PORT=... ESP_SERIAL=... PEER=... FILE=...  Flash then stream' \
		'  make lab-airdrop-relay-test PORT=...  Send staged file in attended lab' \
		'  make lab-airdrop-receiver-oracle-test PORT=...  Advertise anonymous receiver' \
		'  make test-hardware-awdl PORT=... [DURATION=30]' \
		'  make lab-awdl-tx-flash PORT=...  Flash bounded TX experiment' \
		'  make lab-awdl-tx-test PORT=...   Flash and capture TX experiment' \
		'  make lab-awdl-direct-peer-test PORT=...  Test a live AirDrop peer in common windows' \
		'  make web-installer         Stage the GitHub Pages web flasher' \
		'  make provision-wifi PORT=...  Securely provision OTA Wi-Fi over USB' \
		'  make ota-trigger PORT=...     Ask firmware to install GitHub Pages build' \
		'  make ota-local PORT=... ESP_SERIAL=...  Build and serve a LAN OTA image' \
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
		"$(ROOT_DIR)/core/src/ble_wake_payload.c" \
		"$(ROOT_DIR)/tests/test_ble_wake.c" \
		-o "$(BUILD_DIR)/host-tests/test_ble_wake"
	@"$(BUILD_DIR)/host-tests/test_ble_wake"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		-I"$(ROOT_DIR)/tapdrop/include" \
		"$(ROOT_DIR)/tapdrop/src/correlation.c" \
		"$(ROOT_DIR)/tests/test_tapdrop_correlation.c" \
		-o "$(BUILD_DIR)/host-tests/test_tapdrop_correlation"
	@"$(BUILD_DIR)/host-tests/test_tapdrop_correlation"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/awdl_election.c" \
		"$(ROOT_DIR)/tests/test_awdl_election.c" \
		-o "$(BUILD_DIR)/host-tests/test_awdl_election"
	@"$(BUILD_DIR)/host-tests/test_awdl_election"
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
		"$(ROOT_DIR)/core/src/awdl_tlv.c" \
		"$(ROOT_DIR)/core/src/awdl_service.c" \
		"$(ROOT_DIR)/tests/test_awdl_service.c" \
		-o "$(BUILD_DIR)/host-tests/test_awdl_service"
	@"$(BUILD_DIR)/host-tests/test_awdl_service"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/awdl_frame.c" \
		"$(ROOT_DIR)/core/src/awdl_tlv.c" \
		"$(ROOT_DIR)/core/src/awdl_service.c" \
		"$(ROOT_DIR)/core/src/awdl_tx.c" \
		"$(ROOT_DIR)/tests/test_awdl_tx.c" \
		-o "$(BUILD_DIR)/host-tests/test_awdl_tx"
	@"$(BUILD_DIR)/host-tests/test_awdl_tx"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/awdl_data.c" \
		"$(ROOT_DIR)/tests/test_awdl_data.c" \
		-o "$(BUILD_DIR)/host-tests/test_awdl_data"
	@"$(BUILD_DIR)/host-tests/test_awdl_data"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-D_GNU_SOURCE \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/airdrop_ask.c" \
		"$(ROOT_DIR)/core/src/airdrop_upload.c" \
		"$(ROOT_DIR)/tests/test_airdrop_ask.c" \
		-o "$(BUILD_DIR)/host-tests/test_airdrop_ask"
	@"$(BUILD_DIR)/host-tests/test_airdrop_ask" \
		"$(BUILD_DIR)/host-tests/ask.plist"
	@python3 "$(ROOT_DIR)/tests/test_airdrop_ask_plist.py" \
		"$(BUILD_DIR)/host-tests/ask.plist"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/airdrop_http.c" \
		"$(ROOT_DIR)/tests/test_airdrop_http.c" \
		-o "$(BUILD_DIR)/host-tests/test_airdrop_http"
	@"$(BUILD_DIR)/host-tests/test_airdrop_http"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		-I"$(ROOT_DIR)/core/src" \
		"$(ROOT_DIR)/core/src/airdrop_upload.c" \
		"$(ROOT_DIR)/tests/test_airdrop_upload.c" \
		-o "$(BUILD_DIR)/host-tests/test_airdrop_upload"
	@"$(BUILD_DIR)/host-tests/test_airdrop_upload" \
		"$(BUILD_DIR)/host-tests/upload.cpio" \
		"$(BUILD_DIR)/host-tests/upload-stored.dvzip"
	@python3 "$(ROOT_DIR)/tests/test_airdrop_upload_archive.py" \
		"$(BUILD_DIR)/host-tests/upload.cpio" \
		"$(BUILD_DIR)/host-tests/upload-stored.dvzip"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/core/include" \
		"$(ROOT_DIR)/core/src/airdrop_mdns.c" \
		"$(ROOT_DIR)/tests/test_airdrop_mdns.c" \
		-o "$(BUILD_DIR)/host-tests/test_airdrop_mdns"
	@"$(BUILD_DIR)/host-tests/test_airdrop_mdns"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		-I"$(ROOT_DIR)/main" \
		"$(ROOT_DIR)/main/improv_serial_codec.c" \
		"$(ROOT_DIR)/tests/test_improv_serial_codec.c" \
		-o "$(BUILD_DIR)/host-tests/test_improv_serial_codec"
	@"$(BUILD_DIR)/host-tests/test_improv_serial_codec"
	@$(MAKE) --no-print-directory test-host-cli

test-host-cli:
	@cargo test --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml"

relay-upload:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	@test -n "$(FILE)" || { echo "set FILE=/path/to/photo.jpg"; exit 1; }
	@cargo run --quiet --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml" -- \
		upload --port "$(PORT)" --serial "$(ESP_SERIAL)" "$(FILE)"

relay-ping:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	@cargo run --quiet --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml" -- \
		ping --port "$(PORT)" --serial "$(ESP_SERIAL)"

relay-stats:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	@cargo run --quiet --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml" -- \
		stats --port "$(PORT)" --serial "$(ESP_SERIAL)"

relay-wake:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	@cargo run --quiet --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml" -- \
		wake --port "$(PORT)" --serial "$(ESP_SERIAL)"

relay-send:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	@test -n "$(PEER)" || { echo "set PEER to the receiver AWDL MAC, AUTO, or ONLY"; exit 1; }
	@test -n "$(FILE)" || { echo "set FILE=/path/to/photo.jpg"; exit 1; }
	@cargo run --quiet --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml" -- \
		send --port "$(PORT)" --serial "$(ESP_SERIAL)" \
		--target "$(PEER)" \
		$(if $(filter 1 yes true,$(RESTART)),--restart,) "$(FILE)"

relay-peers:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	@cargo run --quiet --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml" -- \
		peers --port "$(PORT)" --serial "$(ESP_SERIAL)"

relay-target:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	@test -n "$(PEER)" || { echo "set PEER to a receiver AWDL MAC, AUTO, NONE, or ONLY"; exit 1; }
	@cargo run --quiet --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml" -- \
		target --port "$(PORT)" --serial "$(ESP_SERIAL)" --target "$(PEER)"

relay-restart:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	@cargo run --quiet --manifest-path "$(ROOT_DIR)/host/espdrop-cli/Cargo.toml" -- \
		restart --port "$(PORT)" --serial "$(ESP_SERIAL)"

lab-airdrop-relay-live: lab-airdrop-relay-flash
	@$(MAKE) --no-print-directory relay-send \
		PORT="$(PORT)" ESP_SERIAL="$(ESP_SERIAL)" PEER="$(PEER)" FILE="$(FILE)"

provision-wifi:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@python3 "$(ROOT_DIR)/scripts/provision_wifi.py" --port "$(PORT)"

ota-trigger:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@python3 "$(ROOT_DIR)/scripts/trigger_ota.py" --port "$(PORT)"

ota-local:
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@test -n "$(ESP_SERIAL)" || { echo "set ESP_SERIAL to the target board serial/MAC"; exit 1; }
	$(call run_idf,reconfigure build)
	@python3 "$(ROOT_DIR)/scripts/local_ota.py" \
		--port "$(PORT)" \
		--expected-serial "$(ESP_SERIAL)" \
		--firmware "$(BUILD_DIR)/espdrop.bin" \
		$(if $(OTA_HOST),--host "$(OTA_HOST)",) \
		--listen-port "$(OTA_PORT)"

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

lab-awdl-direct-peer-build:
	@bash -lc 'set -eo pipefail; \
		test -f "$(IDF_PATH)/export.sh"; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(DIRECT_PEER_LAB_BUILD_DIR)" \
			-D SDKCONFIG="$(DIRECT_PEER_LAB_SDKCONFIG)" \
			-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.awdl-direct-peer-lab.defaults" build'

lab-awdl-direct-peer-flash: lab-awdl-direct-peer-build
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@bash -lc 'set -eo pipefail; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(DIRECT_PEER_LAB_BUILD_DIR)" \
			-D SDKCONFIG="$(DIRECT_PEER_LAB_SDKCONFIG)" -p "$(PORT)" flash'

lab-awdl-direct-peer-test: lab-awdl-direct-peer-flash
	@python3 "$(ROOT_DIR)/scripts/capture_awdl_probe.py" \
		--port "$(PORT)" \
		--seconds "$(DURATION)" \
		--output "$(DIRECT_PEER_LAB_BUILD_DIR)/hardware/awdl-direct-peer.json"

lab-airdrop-relay-build:
	@bash -lc 'set -eo pipefail; \
		test -f "$(IDF_PATH)/export.sh"; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(AIRDROP_RELAY_LAB_BUILD_DIR)" \
			-D SDKCONFIG="$(AIRDROP_RELAY_LAB_SDKCONFIG)" \
			-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.airdrop-relay-lab.defaults" build'

lab-airdrop-relay-flash: lab-airdrop-relay-build
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@bash -lc 'set -eo pipefail; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(AIRDROP_RELAY_LAB_BUILD_DIR)" \
			-D SDKCONFIG="$(AIRDROP_RELAY_LAB_SDKCONFIG)" -p "$(PORT)" flash'

lab-airdrop-relay-test: lab-airdrop-relay-flash
	@python3 "$(ROOT_DIR)/scripts/capture_awdl_probe.py" \
		--port "$(PORT)" \
		--seconds "$(DURATION)" \
		--output "$(AIRDROP_RELAY_LAB_BUILD_DIR)/hardware/airdrop-relay.json"

lab-airdrop-receiver-oracle-build:
	@bash -lc 'set -eo pipefail; \
		test -f "$(IDF_PATH)/export.sh"; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(AIRDROP_RECEIVER_ORACLE_BUILD_DIR)" \
			-D SDKCONFIG="$(AIRDROP_RECEIVER_ORACLE_SDKCONFIG)" \
			-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.airdrop-receiver-oracle.defaults" build'

lab-airdrop-receiver-oracle-flash: lab-airdrop-receiver-oracle-build
	@test -n "$(PORT)" || { echo "set PORT=/dev/..."; exit 1; }
	@bash -lc 'set -eo pipefail; \
		if [ -n "$(IDF_PYTHON_ENV_PATH)" ]; then export IDF_PYTHON_ENV_PATH="$(IDF_PYTHON_ENV_PATH)"; fi; \
		. "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
		cd "$(ROOT_DIR)"; \
		IDF_TARGET="$(TARGET)" idf.py -B "$(AIRDROP_RECEIVER_ORACLE_BUILD_DIR)" \
			-D SDKCONFIG="$(AIRDROP_RECEIVER_ORACLE_SDKCONFIG)" -p "$(PORT)" flash'

lab-airdrop-receiver-oracle-test: lab-airdrop-receiver-oracle-flash
	@python3 "$(ROOT_DIR)/scripts/capture_awdl_probe.py" \
		--port "$(PORT)" \
		--seconds "$(DURATION)" \
		--output "$(AIRDROP_RECEIVER_ORACLE_BUILD_DIR)/hardware/airdrop-receiver-oracle.json"

# Backward-compatible aliases for the superseded experiment name.
lab-awdl-distance-one-build: lab-awdl-direct-peer-build
lab-awdl-distance-one-flash: lab-awdl-direct-peer-flash
lab-awdl-distance-one-test: lab-awdl-direct-peer-test

web-installer: build
	@python3 "$(ROOT_DIR)/scripts/stage_web_installer.py" \
		--build "$(BUILD_DIR)" \
		--output-dir "$(WEB_INSTALLER_DIR)"
