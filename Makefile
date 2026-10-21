# LPM-10C replacement UI: host build, tests, simulator and Cortex-M4 size check.
# All targets run from the repository root.

CC ?= cc
PYTHON ?= python3
CLANG_FORMAT ?= $(shell command -v clang-format || echo /opt/homebrew/opt/llvm@22/bin/clang-format)
LLVM_SIZE ?= $(shell command -v llvm-size || echo /opt/homebrew/opt/llvm@22/bin/llvm-size)

BUILD := build
UI_DIR := src/lpm_ui
HOST_DIR := src/host

UI_SRC := $(wildcard $(UI_DIR)/*.c)
HOST_SRC := $(HOST_DIR)/lpm_fb.c $(HOST_DIR)/lpm_demo.c
# Hand-written sources; lpm_assets.{c,h} are generated and not reformatted.
FORMAT_SRC := $(filter-out $(UI_DIR)/lpm_assets.%,$(wildcard $(UI_DIR)/*.[ch])) $(wildcard $(HOST_DIR)/*.[ch] tests/*.c)
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror
CFLAGS ?= -std=c99 -O2 -g $(WARNINGS)
CPPFLAGS := -I$(UI_DIR) -I$(HOST_DIR)
SANITIZE := -fsanitize=address,undefined -fno-omit-frame-pointer

# Freestanding Cortex-M4F objects: no libc, as on the tester's N32G45x-family MCU.
ARM_FLAGS := --target=thumbv7em-none-eabihf -mcpu=cortex-m4 -mfloat-abi=hard -ffreestanding -fno-builtin \
	-std=c99 -Os -ffunction-sections -fdata-sections $(WARNINGS)

.PHONY: all test test-scripts sim screens assets check-assets arm-size format format-check clean

all: $(BUILD)/lpm_sim $(BUILD)/lpm_tests

$(BUILD):
	mkdir -p $@

$(BUILD)/lpm_tests: tests/test_lpm_ui.c $(UI_SRC) $(HOST_SRC) $(wildcard $(UI_DIR)/*.h $(HOST_DIR)/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZE) -o $@ tests/test_lpm_ui.c $(UI_SRC) $(HOST_SRC)

$(BUILD)/lpm_sim: $(HOST_DIR)/lpm_sim.c $(UI_SRC) $(HOST_SRC) $(wildcard $(UI_DIR)/*.h $(HOST_DIR)/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(HOST_DIR)/lpm_sim.c $(UI_SRC) $(HOST_SRC)

test: $(BUILD)/lpm_tests check-assets test-scripts
	$(BUILD)/lpm_tests

# Hardware helper scripts, tested without a probe or tester.
test-scripts:
	$(PYTHON) -m unittest tests/test_scripts.py

sim screens: $(BUILD)/lpm_sim
	mkdir -p output/screens
	$(BUILD)/lpm_sim output/screens 2

assets:
	$(PYTHON) scripts/gen_ui_assets.py

# Fails if the committed generated assets differ from the generator's output.
check-assets:
	@mkdir -p $(BUILD)/assets
	@$(PYTHON) scripts/gen_ui_assets.py --out $(BUILD)/assets
	@cmp -s $(BUILD)/assets/lpm_assets.c $(UI_DIR)/lpm_assets.c && cmp -s $(BUILD)/assets/lpm_assets.h $(UI_DIR)/lpm_assets.h \
		|| (echo "lpm_assets.{c,h} are stale: run 'make assets'" && exit 1)

arm-size: | $(BUILD)
	@mkdir -p $(BUILD)/arm
	@for f in $(UI_SRC); do clang $(ARM_FLAGS) -I$(UI_DIR) -c $$f -o $(BUILD)/arm/$$(basename $$f .c).o || exit 1; done
	$(LLVM_SIZE) $(BUILD)/arm/*.o
	@$(LLVM_SIZE) -A $(BUILD)/arm/*.o | awk '/^\.(text|rodata)/ {flash += $$2} /^\.(bss|data)/ {ram += $$2} \
		END {printf "Total: flash %d bytes (code + const), static RAM %d bytes\n", flash, ram}'

format:
	$(CLANG_FORMAT) -i $(FORMAT_SRC)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SRC)

clean:
	rm -rf $(BUILD)
