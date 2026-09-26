# SPDX-License-Identifier: Apache-2.0
cfg ?= default
app ?= alltoall
ARCHE3D_PYTHON ?= python
ARCHE3D_SW_BUILD ?= $(abspath $(BUILDDIR)/arche3d/sw/$(notdir $(basename $(cfg)))/$(notdir $(app)))
export ARCHE3D_CONFIG := $(if $(wildcard $(cfg)),$(abspath $(cfg)),$(cfg))

# Resolve this target against its source models during incremental builds as well
# as a first checkout; an older installation may contain the previous tile API.
ifneq ($(filter arche3d%,$(TARGETS) $(MAKECMDGOALS)),)
export GVRUN_TARGET_DIRS := $(CURDIR)/pulp/targets:$(CURDIR)/pulp$(if $(GVRUN_TARGET_DIRS),:$(GVRUN_TARGET_DIRS))
endif

.PHONY: arche3d-sw arche3d-run
arche3d-sw:
	@test -f arche3d_sdk/Makefile || (echo 'Initialize the SDK: git submodule update --init arche3d_sdk'; exit 1)
	$(MAKE) -C arche3d_sdk GVSOC_ROOT="$(CURDIR)" cfg="$(ARCHE3D_CONFIG)" \
		app="$(app)" BUILD="$(ARCHE3D_SW_BUILD)" PYTHON="$(ARCHE3D_PYTHON)" all

arche3d-run:
	gvrun --target=arche3d --parameter=config="$(ARCHE3D_CONFIG)" \
		--binary="$(ARCHE3D_SW_BUILD)/$(notdir $(app)).elf" \
		--work-dir="$(BUILDDIR)/runs/arche3d_$(notdir $(app))" run
