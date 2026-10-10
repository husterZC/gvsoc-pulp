GVSOC_ROOT ?= ../../..
export USE_GVRUN := 1
CASE ?= sync
TARGET = test:mode=$(CASE)
include $(GVSOC_ROOT)/core/tests/common.mk

PYTHON ?= python3
.PHONY: rtl-calibrate fixtures
build: fixtures
fixtures:
	$(PYTHON) fixtures.py

rtl-calibrate:
	$(PYTHON) calibrate.py --fetch --export $(RTL_ARGS)
