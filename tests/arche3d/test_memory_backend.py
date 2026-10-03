# SPDX-License-Identifier: Apache-2.0
"""Check backend selection and compatibility without a simulator installation."""
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / 'pulp'))

from pulp.chips.arche3d.arch import load_arch
from pulp.chips.arche3d.configs.default import FlexClusterArch
from pulp.chips.arche3d.memory import create_memory_endpoint


class MemoryBackend(unittest.TestCase):
    def load_custom(self, arch):
        with patch('pulp.chips.arche3d.arch.importlib.import_module',
                   return_value=SimpleNamespace(FlexClusterArch=lambda: arch)):
            return load_arch('custom')

    def test_default_and_legacy_config(self):
        self.assertEqual(load_arch().dram3d_backend, 'dramsys')
        legacy = FlexClusterArch()
        del legacy.dram3d_backend
        del legacy.dram3d_ram_slots
        resolved = self.load_custom(legacy)
        self.assertEqual(resolved.dram3d_backend, 'dramsys')
        self.assertEqual(resolved.dram3d_ram_slots, 4)

    def test_invalid_backend_and_read_capacity(self):
        arch = FlexClusterArch()
        arch.dram3d_backend = 'unknown'
        with self.assertRaisesRegex(ValueError, 'dram3d_backend'):
            self.load_custom(arch)
        arch.dram3d_backend = 'memory'
        for value in (0, -1, 1.5, '4'):
            arch.dram3d_ram_slots = value
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'dram3d_ram_slots'):
                self.load_custom(arch)
        arch.dram3d_backend = 'dramsys'
        self.load_custom(arch)  # RAM read_slots must not limit DRAMSys capacity.

    def test_ram_uses_selected_terminal_preloads_without_dramsys(self):
        arch = FlexClusterArch()
        arch.dram3d_backend = 'memory'
        arch.dram3d_ram_slots = 7
        arch = self.load_custom(arch)
        fragments = [[0x8000, 0x100, 3, 8]]
        image = SimpleNamespace(binary='program.elf', channel_preloads=[[], fragments])
        constructor = Mock()

        def import_memory(name):
            self.assertEqual(name, 'pulp.3d_network.memory_endpoint')
            return SimpleNamespace(MemoryEndpoint=constructor)

        with patch('pulp.chips.arche3d.memory.importlib.import_module', side_effect=import_memory):
            create_memory_endpoint(None, 'channel', arch, 1, image, benchmark_init=True)
        kwargs = constructor.call_args[1]
        self.assertEqual(kwargs['size'], arch.dram3d_vault_space)
        self.assertEqual(kwargs['read_slots'], 7)
        self.assertEqual(kwargs['data_width'], arch.i3d_axi_data_width)
        self.assertEqual(kwargs['endpoint_id'], 1)
        self.assertTrue(kwargs['benchmark_init'])
        self.assertEqual(kwargs['preload_file'], 'program.elf')
        self.assertEqual(kwargs['preload_segments'], fragments)
        self.assertNotIn('dram_type', kwargs)


if __name__ == '__main__':
    unittest.main()
