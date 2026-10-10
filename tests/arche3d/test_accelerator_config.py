#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Configuration selection and validation, without a simulator installation."""
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from pulp.chips.arche3d.arch import load_arch
from pulp.chips.arche3d.configs.default import FlexClusterArch


class ConfigTests(unittest.TestCase):
    def custom(self, **kwargs):
        arch = FlexClusterArch()
        for key, value in kwargs.items():
            setattr(arch, key, value)
        with patch('importlib.import_module', return_value=SimpleNamespace(FlexClusterArch=lambda: arch)):
            return load_arch('custom')

    def test_default_and_redmule(self):
        default = load_arch()
        self.assertEqual((default.matrix_engine_kind, default.mxcore_fp4_count), (2, 4))
        self.assertEqual(default.mxcore_fp4_core_list, [0, 1, 2, 3])
        self.assertEqual(default.hwpe_bandwidth, 512)
        redmule = load_arch('arche3d_redmule')
        self.assertEqual((redmule.matrix_engine_kind, redmule.mxcore_fp4_count), (1, 0))
        self.assertEqual(redmule.redmule_ce_height, 16)

    def test_owner_order_and_count(self):
        arch = self.custom(mxcore_fp4_core_list=[3, 1], hwpe_bandwidth=256)
        self.assertEqual(arch.mxcore_fp4_core_list, [3, 1])
        self.assertEqual((arch.mxcore_fp4_count, arch.mxcore_fp4_core_mask), (2, 10))

    def test_invalid_selection_and_maps(self):
        for kwargs in [dict(matrix_engine='both'), dict(mxcore_fp4_core_list=[]),
                       dict(mxcore_fp4_core_list=[1, 1]), dict(mxcore_fp4_core_list=[6]),
                       dict(mxcore_fp4_irq=19), dict(mxcore_fp4_reg_size=4),
                       dict(mxcore_fp4_reg_base=0x6a000), dict(mxcore_fp4_reg_base=0x20000000),
                       dict(hwpe_bandwidth=513), dict(hwpe_bandwidth=1024),
                       dict(layout_conversion_latency=4)]:
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.custom(**kwargs)

    def test_legacy_standalone_keeps_redmule(self):
        arch = FlexClusterArch()
        for attr in list(vars(arch)):
            if attr.startswith('mxcore_fp4') or attr in ('matrix_engine', 'hwpe_bandwidth', 'layout_conversion_latency'):
                delattr(arch, attr)
        with patch('importlib.import_module', return_value=SimpleNamespace(FlexClusterArch=lambda: arch)):
            resolved = load_arch('legacy')
        self.assertEqual((resolved.matrix_engine_kind, resolved.mxcore_fp4_count), (1, 0))


if __name__ == '__main__':
    unittest.main()
