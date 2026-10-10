#
# Copyright (C) 2020 ETH Zurich and University of Bologna
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

import gvsoc.runner
import pulp.chips.arche3d.logic.snitch.snitch_core as iss
import pulp.chips.arche3d.logic.memory as memory
import interco.router as router
import gvsoc.systree
from pulp.chips.arche3d.logic.cluster_registers import ClusterRegisters
from pulp.chips.arche3d.logic.light_redmule import LightRedmule
from pulp.chips.arche3d.logic.offload_decoder import Arche3dOffloadDecoder
from pulp.chips.arche3d.logic.l1_fabric import L1Fabric
from pulp.chips.arche3d.logic.layout_engine import LayoutEngine
from pulp.chips.arche3d.logic.matrix_bridge import MatrixBridge
from pulp.mxcore_fp4 import MXCoreFP4
from pulp.chips.arche3d.logic.util_dumpper import UtilDumpper
from pulp.chips.arche3d.logic.snitch.zero_mem import ZeroMem
from pulp.chips.arche3d.logic.idma.snitch_dma import SnitchDma
import gvsoc.runner
from pulp.chips.arche3d.logic.snitch.sequencer import Sequencer


GAPY_TARGET = True

class Area:

    def __init__(self, base, size):
        self.base = base
        self.size = size



class ClusterArch:
    def __init__(self,  nb_core_per_cluster, base, cluster_id, tcdm_size,
                        zomem_base,         zomem_size,
                        reg_base,           reg_size,
                        tcdm_remote,        sync_wakeup_addr,
                        insn_base,          insn_size,
                        nb_tcdm_banks,      tcdm_bank_width,
                        redmule_ce_height,  redmule_ce_width,   redmule_ce_pipe,
                        redmule_elem_size,  redmule_queue_depth,
                        redmule_reg_base,   redmule_reg_size,
                        idma_outstand_txn,  idma_outstand_burst,
                        num_cluster_x,      num_cluster_y,
                        spatz_core_list,    spatz_num_vlsu,     spatz_num_fu,
                        spatz_vlsu_bw,      spatz_vreg_gather_eff,
                        data_bandwidth,     auto_fetch=False,   multi_idma_enable=0,
                        core_model="fast",  tech_node="5nm", idma_gather_enable=False,
                        matrix_engine="redmule", mxcore_fp4_core_list=(),
                        mxcore_fp4_reg_base=0x20020000, mxcore_fp4_reg_size=0x200,
                        mxcore_fp4_irq=20, hwpe_bandwidth=512, layout_conversion_latency=5):

        self.nb_core                = nb_core_per_cluster
        self.base                   = base
        self.cluster_id             = cluster_id
        self.auto_fetch             = auto_fetch
        self.barrier_irq            = 19
        # Scalar and vector ports share the lowest L1 priority.
        self.tcdm                   = ClusterArch.Tcdm(base, self.nb_core + len(spatz_core_list)*spatz_num_vlsu, tcdm_size, nb_tcdm_banks, tcdm_bank_width, tech_node)
        self.zomem_area             = Area(zomem_base, zomem_size)
        self.remote_tcdm_area       = Area(tcdm_remote, tcdm_size * num_cluster_x * num_cluster_y)
        self.sync_wakeup_addr       = sync_wakeup_addr
        self.reg_area               = Area(reg_base, reg_size)
        self.insn_area              = Area(insn_base, insn_size)

        #Spatz
        self.spatz_core_list        = spatz_core_list
        self.spatz_num_vlsu         = spatz_num_vlsu
        self.spatz_num_fu           = spatz_num_fu
        self.spatz_vlsu_bw          = spatz_vlsu_bw
        self.spatz_vreg_gather_eff  = spatz_vreg_gather_eff

        # Mutually exclusive matrix architectures; instance order defines MMIO order.
        self.matrix_engine = matrix_engine
        self.mxcore_fp4_core_list = list(mxcore_fp4_core_list) if matrix_engine == 'mxcore_fp4' else []
        self.mxcore_fp4_reg_base = mxcore_fp4_reg_base
        self.mxcore_fp4_reg_size = mxcore_fp4_reg_size
        self.mxcore_fp4_irq = mxcore_fp4_irq
        self.layout_conversion_latency = layout_conversion_latency
        self.tcdm.hwpe_bandwidth = hwpe_bandwidth
        self.tcdm.hwpe_ports = 1 + (len(self.mxcore_fp4_core_list) if matrix_engine == 'mxcore_fp4' else 1)

        #RedMule
        self.redmule_ce_height      = redmule_ce_height
        self.redmule_ce_width       = redmule_ce_width
        self.redmule_ce_pipe        = redmule_ce_pipe
        self.redmule_elem_size      = redmule_elem_size
        self.redmule_queue_depth    = redmule_queue_depth
        self.redmule_area           = Area(redmule_reg_base, redmule_reg_size)

        #IDMA
        self.idma_outstand_txn      = idma_outstand_txn
        self.idma_outstand_burst    = idma_outstand_burst
        self.data_bandwidth         = data_bandwidth
        self.multi_idma_enable      = multi_idma_enable
        self.idma_gather_enable     = bool(idma_gather_enable)
        self.core_model             = core_model

        #Global Information
        self.num_cluster_x          = num_cluster_x
        self.num_cluster_y          = num_cluster_y
        self.tech_node              = tech_node

    class Tcdm:
        def __init__(self, base, nb_masters, tcdm_size, nb_tcdm_banks, tcdm_bank_width, tech_node):
            self.area = Area( base, tcdm_size)
            self.nb_tcdm_banks = nb_tcdm_banks
            self.bank_width = tcdm_bank_width
            self.bank_size = self.area.size // self.nb_tcdm_banks
            self.nb_masters = nb_masters
            self.tech_node = tech_node


class ClusterTcdm(gvsoc.systree.Component):

    def __init__(self, parent, name, arch):
        super().__init__(parent, name)

        fabric = L1Fabric(self, 'fabric', banks=arch.nb_tcdm_banks,
            bank_width=arch.bank_width, size=arch.area.size, low_ports=arch.nb_masters,
            hwpe_ports=arch.hwpe_ports, hwpe_bandwidth=arch.hwpe_bandwidth)
        for i in range(arch.nb_tcdm_banks):
            bank = memory.Memory(self, f'bank_{i}', size=arch.bank_size, atomics=True,
                                 width_log2=0, tech_node=arch.tech_node)
            self.bind(fabric, f'out_{i}', bank, 'input')
        for i in range(arch.nb_masters):
            self.bind(self, f'in_{i}', fabric, f'in_{i}')
        for port in ('dma_input', 'bus_input', 'sync_input'):
            self.bind(self, port, fabric, port)
        for i in range(arch.hwpe_ports):
            self.bind(self, f'hwpe_{i}', fabric, f'hwpe_{i}')

    def i_INPUT(self, port: int) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'in_{port}', signature='io')

    def i_DMA_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'dma_input', signature='io')

    def i_BUS_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'bus_input', signature='io')

    def i_HWPE_INPUT(self, port=0) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'hwpe_{port}', signature='io')

    def i_SYNC_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'sync_input', signature='io')



class ClusterUnit(gvsoc.systree.Component):

    def __init__(self, parent, name, arch, entry=0,
                 extra_dma_factory=None, instruction_cache_factory=None):
        super().__init__(parent, name)

        #
        # Components
        #

        boot_addr = entry
        self.instruction_cache = instruction_cache_factory(self)

        # Main router
        wide_axi_goto_tcdm = router.Router(self, 'wide_axi_goto_tcdm')
        wide_axi_from_idma = router.Router(self, 'wide_axi_from_idma')
        narrow_axi = router.Router(self, 'narrow_axi', bandwidth=8)

        # L1 Memory
        tcdm = ClusterTcdm(self, 'tcdm', arch.tcdm)

        # Optional dedicated offload DMA for architectures reusing this logic tile.
        # The factory returns (core index, DMA); all existing tiles keep their wiring.
        extra_dma_core, self.extra_dma = (extra_dma_factory(self, arch)
            if extra_dma_factory is not None else (None, None))
        if extra_dma_core is not None:
            if arch.multi_idma_enable or not 0 <= extra_dma_core < arch.nb_core:
                raise ValueError('Dedicated DMA requires a valid core and shared legacy DMA')

        # Cores
        cores = []
        fp_cores = []
        cores_ico = []
        use_fast_core = arch.core_model == "fast"
        xfrep = not use_fast_core
        if xfrep:
            fpu_sequencers = []
        for core_id in range(0, arch.nb_core):
            core_has_spatz = core_id in arch.spatz_core_list
            core_isa = 'rv32imfdva' if core_has_spatz else 'rv32imfda'
            if use_fast_core:
                cores.append(iss.SnitchFast(self, f'pe{core_id}', isa=core_isa,
                    fetch_enable=arch.auto_fetch, boot_addr=boot_addr,
                    core_id=core_id, htif=False, inc_spatz=core_has_spatz,
                    spatz_nb_lanes=arch.spatz_num_vlsu,
                    # The VLSU/compute model takes bytes; the architecture and VLEN use bits.
                    spatz_lane_width=arch.spatz_vlsu_bw // 8,
                    vlen=arch.spatz_num_vlsu * arch.spatz_vlsu_bw,
                    ssr=True, sequencer=True))
            else:
                cores.append(iss.Snitch(self, f'pe{core_id}', isa=core_isa,
                    fetch_enable=arch.auto_fetch, boot_addr=boot_addr,
                    core_id=core_id, htif=False, inc_spatz=core_has_spatz))

                fp_cores.append(iss.Snitch_fp_ss(self, f'fp_ss{core_id}', isa=core_isa,
                    fetch_enable=arch.auto_fetch, boot_addr=boot_addr,
                    core_id=core_id, htif=False, inc_spatz=core_has_spatz))
                fpu_sequencers.append(Sequencer(self, f'fpu_sequencer{core_id}', latency=0))

            cores_ico.append(router.Router(self, f'pe{core_id}_ico', bandwidth=arch.tcdm.bank_width))

        redmule = None
        if arch.matrix_engine == 'redmule':
            redmule = LightRedmule(self, f'redmule',
                                        tcdm_bank_width     = arch.tcdm.bank_width,
                                        tcdm_bank_number    = arch.tcdm.nb_tcdm_banks,
                                        elem_size           = arch.redmule_elem_size,
                                        ce_height           = arch.redmule_ce_height,
                                        ce_width            = arch.redmule_ce_width,
                                        ce_pipe             = arch.redmule_ce_pipe,
                                        queue_depth         = arch.redmule_queue_depth,
                                        tech_node           = arch.tech_node)

        # Cluster peripherals
        cluster_registers = ClusterRegisters(self, 'cluster_registers',
            num_cluster_x=arch.num_cluster_x, num_cluster_y=arch.num_cluster_y, nb_cores=arch.nb_core,
            boot_addr=boot_addr, cluster_id=arch.cluster_id, sync_wakeup_addr=arch.sync_wakeup_addr,
            matrix_engine=1 if redmule else 2, mxcore_owners=arch.mxcore_fp4_core_list,
            matrix_base=arch.redmule_area.base if redmule else arch.mxcore_fp4_reg_base,
            matrix_stride=arch.redmule_area.size if redmule else arch.mxcore_fp4_reg_size,
            matrix_irq=arch.mxcore_fp4_irq, layout_base=arch.reg_area.base + arch.reg_area.size + 64)

        #data dumpper
        data_dumpper = UtilDumpper(self, 'data_dumpper', arch.cluster_id)
        data_dumpper_ctrl_base = arch.reg_area.base + arch.reg_area.size
        data_dumpper_ctrl_size = 64
        data_dumpper_input_base = arch.tcdm.area.base + arch.tcdm.area.size
        data_dumpper_input_size = arch.tcdm.area.size
        ctrl_base_update = arch.reg_area.base + arch.reg_area.size + data_dumpper_ctrl_size


        #Layout Engine
        layout_engine = LayoutEngine(self, 'layout_engine',
            bandwidth=arch.tcdm.hwpe_bandwidth, l1_base=arch.tcdm.area.base,
            l1_size=arch.tcdm.area.size, conversion_latency=arch.layout_conversion_latency)
        layout_engine_ctrl_base = ctrl_base_update
        layout_engine_ctrl_size = 64
        ctrl_base_update += 64

        # Cluster DMA
        if arch.multi_idma_enable:
            idma_list = []
            for x in range(arch.nb_core):
                idma_list.append(SnitchDma(self, f'idma_{x}', loc_base=arch.tcdm.area.base, loc_size=arch.tcdm.area.size + data_dumpper_input_size,
                tcdm_width=(arch.tcdm.nb_tcdm_banks * arch.tcdm.bank_width), transfer_queue_size=arch.idma_outstand_txn, burst_queue_size=arch.idma_outstand_burst,
                gather_enable=arch.idma_gather_enable))
                pass
        else:
            idma = SnitchDma(self, 'idma', loc_base=arch.tcdm.area.base, loc_size=arch.tcdm.area.size + data_dumpper_input_size,
                tcdm_width=(arch.tcdm.nb_tcdm_banks * arch.tcdm.bank_width), transfer_queue_size=arch.idma_outstand_txn, burst_queue_size=arch.idma_outstand_burst,
                gather_enable=arch.idma_gather_enable)
            pass

        #zero memory
        zero_mem = ZeroMem(self, 'zero_mem', size=arch.zomem_area.size)

        #synchronization router
        sync_router_master = router.Router(self, 'sync_router_master', bandwidth=4)
        sync_router_slave = router.Router(self, 'sync_router_slave', bandwidth=4)

        #
        # Bindings
        #

        # The system releases all tiles after direct cache initialization and ELF loading.
        self.itf_bind('boot_ready', cluster_registers.i_BOOT_READY(),
                      signature='wire<bool>', composite_bind=True)

        # Narrow router for cores data accesses
        self.o_NARROW_INPUT(narrow_axi.i_INPUT())
        narrow_axi.o_MAP(self.i_NARROW_SOC())
        # TODO check on real HW where this should go. This probably go through wide axi to
        # have good bandwidth when transferring from one cluster to another
        narrow_axi.o_MAP(cores_ico[0].i_INPUT(), base=arch.tcdm.area.base, size=arch.tcdm.area.size, rm_base=False)

        #binding to cluster registers
        narrow_axi.o_MAP(cluster_registers.i_INPUT(), base=arch.reg_area.base, size=arch.reg_area.size, rm_base=True)

        #binding to data dumpper
        narrow_axi.o_MAP(data_dumpper.i_CTRL(), base=data_dumpper_ctrl_base, size=data_dumpper_ctrl_size, rm_base=True)

        #binding to layout engine
        narrow_axi.o_MAP(layout_engine.i_INPUT(), base=layout_engine_ctrl_base, size=layout_engine_ctrl_size, rm_base=True)

        #binding to redmule
        if redmule:
            narrow_axi.o_MAP(redmule.i_INPUT(), base=arch.redmule_area.base, size=arch.redmule_area.size, rm_base=True)
        else:
            for index, owner in enumerate(arch.mxcore_fp4_core_list):
                engine = MXCoreFP4(self, f'mxcore_fp4_{index}')
                bridge = MatrixBridge(self, f'mxcore_fp4_bridge_{index}', engine, arch.tcdm.area.base)
                narrow_axi.o_MAP(bridge.i_INPUT(), base=arch.mxcore_fp4_reg_base + index * arch.mxcore_fp4_reg_size,
                                 size=arch.mxcore_fp4_reg_size, rm_base=True)
                bridge.o_MEMORY(tcdm.i_HWPE_INPUT(index))
                engine.o_IRQ(cores[owner].i_IRQ(arch.mxcore_fp4_irq))

        # Read-only program/rodata alias, backed by the shared instruction cache.
        narrow_axi.o_MAP(self.instruction_cache.i_DATA(), base=arch.insn_area.base,
            size=arch.insn_area.size, rm_base=False)

        # Scalar remote L1 accesses use the synchronization bus. The special
        # wakeup address launches a multicast using the cluster's target masks.
        narrow_axi.o_MAP(sync_router_master.i_INPUT(), base=arch.remote_tcdm_area.base,
            size=arch.remote_tcdm_area.size, rm_base=False)
        narrow_axi.o_MAP(cluster_registers.i_WAKEUP_SEND(), name='wakeup_send', base=arch.sync_wakeup_addr,
            size=4, rm_base=True)
        sync_router_master.o_MAP(self.i_SYNC_OUTPUT())


        #RedMule to TCDM
        if redmule:
            redmule.o_TCDM(tcdm.i_HWPE_INPUT(0))

        #Layout Engine to TCDM
        layout_engine.o_TCDM(tcdm.i_HWPE_INPUT(arch.tcdm.hwpe_ports - 1))

        # Wire router for DMA and instruction caches
        self.o_WIDE_INPUT(wide_axi_goto_tcdm.i_INPUT())
        wide_axi_goto_tcdm.o_MAP(tcdm.i_BUS_INPUT())
        wide_axi_from_idma.o_MAP(self.i_WIDE_SOC())
        wide_axi_from_idma.o_MAP(zero_mem.i_INPUT(), base=arch.zomem_area.base, size=arch.zomem_area.size, rm_base=True)


        # iDMA connection
        if arch.multi_idma_enable:
            for x in range(arch.nb_core):
                offload_decoder = Arche3dOffloadDecoder(self, f'offload_decoder_{x}', nb_cores=1)
                cores[x].o_OFFLOAD(offload_decoder.i_OFFLOAD())
                offload_decoder.o_OFFLOAD_GRANT(0, cores[x].i_OFFLOAD_GRANT())
                offload_decoder.o_DMA(idma_list[x].i_OFFLOAD())
                idma_list[x].o_OFFLOAD_GRANT(offload_decoder.i_DMA_GRANT())
                if redmule:
                    offload_decoder.o_REDMULE(redmule.i_OFFLOAD())
                    redmule.o_OFFLOAD_GRANT(offload_decoder.i_REDMULE_GRANT())
                pass
        else:
            offload_decoder = Arche3dOffloadDecoder(self, 'offload_decoder', nb_cores=arch.nb_core)
            for core_id in range(0, arch.nb_core):
                if core_id == extra_dma_core:
                    cores[core_id].o_OFFLOAD(self.extra_dma.i_OFFLOAD())
                    self.extra_dma.o_OFFLOAD_GRANT(cores[core_id].i_OFFLOAD_GRANT())
                    continue
                cores[core_id].o_OFFLOAD(offload_decoder.i_OFFLOAD(core_id))
                offload_decoder.o_OFFLOAD_GRANT(core_id, cores[core_id].i_OFFLOAD_GRANT())
            offload_decoder.o_DMA(idma.i_OFFLOAD())
            idma.o_OFFLOAD_GRANT(offload_decoder.i_DMA_GRANT())
            if redmule:
                offload_decoder.o_REDMULE(redmule.i_OFFLOAD())
                redmule.o_OFFLOAD_GRANT(offload_decoder.i_REDMULE_GRANT())
            pass

        # Cores
        for core_id in range(0, arch.nb_core):
            cluster_registers.o_FETCH_START( cores[core_id].i_FETCHEN() )

        for core_id in range(0, arch.nb_core):
            cores[core_id].o_BARRIER_REQ(cluster_registers.i_BARRIER_ACK(core_id))
        for core_id in range(0, arch.nb_core):
            cores[core_id].o_DATA(cores_ico[core_id].i_INPUT())
            cores_ico[core_id].o_MAP(tcdm.i_INPUT(core_id), base=arch.tcdm.area.base,
                size=arch.tcdm.area.size, rm_base=True)
            cores_ico[core_id].o_MAP(narrow_axi.i_INPUT())
            cores[core_id].o_FETCH(self.instruction_cache.i_FETCH(core_id))
            cores[core_id].o_FLUSH_CACHE(self.instruction_cache.i_FLUSH())
            self.instruction_cache.o_FLUSH_ACK(cores[core_id].i_FLUSH_CACHE_ACK())
            if core_id in arch.spatz_core_list:
                spatz_index = arch.spatz_core_list.index(core_id)
                for vlsu_port in range(arch.spatz_num_vlsu):
                    vlsu_router = router.Router(self, f'spatz_{core_id}_vlsu_{vlsu_port}_router', bandwidth=arch.tcdm.bank_width)
                    vlsu_router.add_mapping("output")
                    if use_fast_core:
                        cores[core_id].o_VLSU(vlsu_port, vlsu_router.i_INPUT())
                    else:
                        self.bind(cores[core_id], f'vlsu_{vlsu_port}', vlsu_router, 'input')
                    self.bind(vlsu_router, 'output', tcdm, f'in_{arch.nb_core + spatz_index*arch.spatz_num_vlsu + vlsu_port}')

        if not use_fast_core:
            for core_id in range(0, arch.nb_core):
                fp_cores[core_id].o_DATA( cores_ico[core_id].i_INPUT() )
                cluster_registers.o_FETCH_START( fp_cores[core_id].i_FETCHEN() )

                # SSR in fp subsystem datem mover <-> memory port
                self.bind(fp_cores[core_id], 'ssr_dm0', cores_ico[core_id], 'input')
                self.bind(fp_cores[core_id], 'ssr_dm1', cores_ico[core_id], 'input')
                self.bind(fp_cores[core_id], 'ssr_dm2', cores_ico[core_id], 'input')

                # Use WireMaster & WireSlave
                # Add fpu sequence buffer in between int core and fp core to issue instructions
                if xfrep:
                    self.bind(cores[core_id], 'acc_req', fpu_sequencers[core_id], 'input')
                    self.bind(fpu_sequencers[core_id], 'output', fp_cores[core_id], 'acc_req')
                    self.bind(cores[core_id], 'acc_req_ready', fpu_sequencers[core_id], 'acc_req_ready')
                    self.bind(fpu_sequencers[core_id], 'acc_req_ready_o', fp_cores[core_id], 'acc_req_ready')
                else:
                    # Comment out if we want to add sequencer
                    self.bind(cores[core_id], 'acc_req', fp_cores[core_id], 'acc_req')
                    self.bind(cores[core_id], 'acc_req_ready', fp_cores[core_id], 'acc_req_ready')

                self.bind(fp_cores[core_id], 'acc_rsp', cores[core_id], 'acc_rsp')

        for core_id in range(0, arch.nb_core):
            self.bind(cluster_registers, f'barrier_ack', cores[core_id], 'barrier_ack')
        for core_id in range(0, arch.nb_core):
            cluster_registers.o_EXTERNAL_IRQ(core_id, cores[core_id].i_IRQ(arch.barrier_irq))

        #Global Synchronization
        self.o_SYNC_INPUT(sync_router_slave.i_INPUT())
        sync_router_slave.o_MAP(tcdm.i_SYNC_INPUT(), base=0, size=arch.tcdm.area.size)
        sync_router_slave.o_MAP(cluster_registers.i_WAKEUP_RECV(), base=arch.sync_wakeup_addr,
            size=4, rm_base=True)
        cluster_registers.o_WAKEUP(sync_router_master.i_INPUT())

        # Cluster DMA
        if self.extra_dma is not None:
            extra_dma_router = router.Router(self, 'extra_dma_tcdm')
            extra_dma_router.o_MAP(tcdm.i_DMA_INPUT(), base=arch.tcdm.area.base,
                size=arch.tcdm.area.size, rm_base=True)
            self.extra_dma.o_TCDM(extra_dma_router.i_INPUT())
            self.extra_dma.o_INDEX(extra_dma_router.i_INPUT())
        data_dumpper_arbiter = router.Router(self, 'data_dumpper_arbiter')
        data_dumpper_arbiter.o_MAP(tcdm.i_DMA_INPUT())
        data_dumpper_arbiter.o_MAP(data_dumpper.i_INPUT(), base=data_dumpper_input_base, size=data_dumpper_input_size, rm_base=True)
        if arch.idma_gather_enable:
            # The gather index port carries absolute addresses and reads one
            # 64-bit word. Use the real banked TCDM and its DMA priority path.
            index_router = router.Router(self, 'idma_index_router', bandwidth=8, latency=0)
            index_router.o_MAP(tcdm.i_DMA_INPUT(), base=arch.tcdm.area.base,
                size=arch.tcdm.area.size, rm_base=True)
        if arch.multi_idma_enable:
            for x in range(arch.nb_core):
                idma_list[x].o_TCDM(data_dumpper_arbiter.i_INPUT())
                idma_list[x].o_AXI(wide_axi_from_idma.i_INPUT())
                if arch.idma_gather_enable:
                    idma_list[x].o_INDEX(index_router.i_INPUT())
                pass
        else:
            idma.o_TCDM(data_dumpper_arbiter.i_INPUT())
            idma.o_AXI(wide_axi_from_idma.i_INPUT())
            if arch.idma_gather_enable:
                idma.o_INDEX(index_router.i_INPUT())
            pass

    def i_WIDE_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'wide_input', signature='io')

    def o_WIDE_INPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('wide_input', itf, signature='io', composite_bind=True)

    def i_WIDE_SOC(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'wide_soc', signature='io')

    def o_WIDE_SOC(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('wide_soc', itf, signature='io')

    def i_NARROW_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'narrow_input', signature='io')

    def o_NARROW_INPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('narrow_input', itf, signature='io', composite_bind=True)

    def i_NARROW_SOC(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'narrow_soc', signature='io')

    def o_NARROW_SOC(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('narrow_soc', itf, signature='io')

    def i_SYNC_OUTPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'sync_output', signature='io')

    def o_SYNC_OUTPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('sync_output', itf, signature='io')

    def i_SYNC_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'sync_input', signature='io')

    def o_SYNC_INPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('sync_input', itf, signature='io', composite_bind=True)

    def i_BOOT_READY(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'boot_ready', signature='wire<bool>')
