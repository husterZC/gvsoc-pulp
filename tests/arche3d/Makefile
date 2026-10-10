GVSOC_ROOT ?= ../../..
ROOT := $(abspath $(GVSOC_ROOT))
PYTHON ?= python3
export USE_GVRUN := 1
RAM := $(ROOT)/arche3d_sdk/apps/collective_row_sweep/ram_config.py
RED := $(ROOT)/pulp/tests/arche3d/ram_redmule.py

.PHONY: all build run clean
all: build run
build:
	$(PYTHON) layout_fixtures.py $(ROOT)/build/arche3d_work/layout.bin
	$(MAKE) -C $(ROOT) TARGETS=arche3d_dma_test MODULES=$(CURDIR) cfg=$(RAM) build
	$(MAKE) -C $(ROOT) TARGETS='arche3d_dma_test;arche3d_layout_test' MODULES=$(CURDIR) cfg=$(RED) build
run:
	$(PYTHON) test_accelerator_config.py
	$(PYTHON) run_accelerators.py
clean:
	@true
