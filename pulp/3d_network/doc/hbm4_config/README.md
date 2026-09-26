# Archived measurement configuration

These files preserve the resolved inputs and hashes for the original
[HBM4 context sweep](../soc_context_b16_hbm4.md). They are measurement artifacts,
not the active configuration or an additional GVSoC configuration source.

Current tests start from
`gvsoc/add_dramsyslib_patches/dramsys_configs/hbm4-emu-example.json`, including
the user's edited memspec. The bundled library needs a compatibility conversion;
`tests/hbm4.sh build` generates it under `build/network3d_hbm4/dramsys_configs`.
The model selects the simulation using `dram_type`, resolves it via
`DRAMSYS_PATH`, and loads the library using `LD_LIBRARY_PATH`.

See [the integration instructions](../../README.md#dramsys-hbm4-endpoints).
Keep these archived JSON files unchanged so their hashes continue to describe
the published measurements. Regenerate current inputs from the source, rather
than editing this snapshot.
