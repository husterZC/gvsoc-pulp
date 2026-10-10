// SPDX-License-Identifier: Apache-2.0
#include "Vmxcore_hwpe_wrap.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

// One-cycle SRAM responses; eight 32-bit banks form one 256-bit TCDM beat.
struct Sim {
    VerilatedContext context;
    Vmxcore_hwpe_wrap dut{&context};
    std::vector<uint8_t> memory;
    std::ofstream trace;
    uint64_t cycle = 0, trigger = 0;
    bool recording = false;
    std::array<uint32_t, 6> base, size;
    uint32_t read32(uint32_t addr) {
        if (addr + 4 > memory.size()) throw std::runtime_error("RTL read outside memory");
        uint32_t v = 0;
        for (int b=0; b<4; ++b) v |= uint32_t(memory[addr+b]) << (b*8);
        return v;
    }
    void tick() {
        dut.clk_i = 0;
        dut.eval();
        dut.tcdm_gnt_i = 0xff;
        dut.eval();
        uint32_t req = dut.tcdm_req_o, wen = dut.tcdm_wen_o;
        std::array<uint32_t, 8> response{};
        if (req && req != 0xff) throw std::runtime_error("Partial TCDM request");
        if (recording && req) {
            uint32_t addr = dut.tcdm_add_o[0];
            int region = -1;
            for (int i=0; i<6; ++i)
                if (addr >= base[i] && addr < base[i]+size[i]) region=i;
            if (region < 0) throw std::runtime_error("Unmapped TCDM access");
            bool write = !(wen & 1);
            if (write != (region >= 4)) throw std::runtime_error("Wrong TCDM direction");
            if (write && dut.tcdm_be_o != 0xffffffffU) throw std::runtime_error("Partial TCDM byte enable");
            trace << cycle-trigger << ',' << region << ',' << addr-base[region] << '\n';
        }
        for (int i=0; i<8; ++i) {
            if (!(req & (1<<i))) continue;
            uint32_t addr = dut.tcdm_add_o[i];
            response[i] = read32(addr);
            if (!(wen & (1<<i))) {
                for (int b=0; b<4; ++b)
                    if ((dut.tcdm_be_o >> (i*4+b)) & 1)
                        memory[addr+b] = dut.tcdm_data_o[i] >> (b*8);
            }
        }
        dut.clk_i = 1;
        dut.eval();
        context.timeInc(1);
        // Present the just-accepted read at the next rising edge.
        dut.clk_i = 0;
        dut.tcdm_r_valid_i = req;
        for (int i=0; i<8; ++i) dut.tcdm_r_data_i[i] = response[i];
        dut.eval();
        context.timeInc(1);
        ++cycle;
        if (cycle > 1000000) throw std::runtime_error("RTL watchdog timeout");
    }
    void write(uint32_t addr, uint32_t data, bool start=false) {
        dut.periph_req_i=1; dut.periph_wen_i=0;
        dut.periph_add_i=addr; dut.periph_data_i=data; dut.periph_be_i=15;
        dut.eval();
        while (!dut.periph_gnt_o) tick();
        if (start) { trigger=cycle; recording=true; }
        tick();
        dut.periph_req_i=0;
        dut.eval();
        if (!start) tick();
    }
    uint32_t read(uint32_t addr) {
        dut.periph_req_i=1; dut.periph_wen_i=1; dut.periph_add_i=addr;
        dut.eval();
        while (!dut.periph_gnt_o) tick();
        tick();
        dut.periph_req_i=0; dut.eval();
        while (!dut.periph_r_valid_o) tick();
        uint32_t value=dut.periph_r_data_o;
        tick();
        return value;
    }
};

int main(int argc, char **argv) {
    try {
        if (argc != 7) throw std::runtime_error("usage: rtl M N K memory.bin output.bin trace.csv");
        Sim sim;
        unsigned m=std::stoul(argv[1]), n=std::stoul(argv[2]), k=std::stoul(argv[3]);
        if (m%32 || n%32 || k%32 || !m || !n || !k) throw std::runtime_error("Bad shape");
        sim.size = {m*k/2, n*k/2, m*k/32, n*k/32, m*n/2, m*n/32};
        sim.base[0]=0x1000;
        for (int i=1; i<6; ++i) sim.base[i]=sim.base[i-1]+sim.size[i-1];
        std::ifstream input(argv[4], std::ios::binary);
        if (!input) throw std::runtime_error("Missing input file");
        sim.memory.assign(std::istreambuf_iterator<char>(input), {});
        sim.trace.open(argv[6]);
        if (!sim.trace) throw std::runtime_error("Cannot write trace");
        sim.dut.rst_ni=0;
        for (int i=0; i<10; ++i) sim.tick();
        sim.dut.rst_ni=1;
        for (int i=0; i<10; ++i) sim.tick();
        sim.write(0x14, 0);
        for (int i=0; i<10; ++i) sim.tick();
        if (int32_t(sim.read(4)) < 0) throw std::runtime_error("Could not acquire HWPE job");
        for (unsigned i=0; i<6; ++i) sim.write(0x20+4*i, sim.base[i]);
        sim.write(0x38, m | (k<<10) | (n<<22));
        sim.write(0x3c, (1<<21) | (8<<9) | (19<<3)); // quantize, FP4, SDOTP, FP32, RNE
        sim.write(0x40, (m/32) | ((n/32)<<4) | ((k/16)<<9) | ((k/32)<<16));
        sim.write(0x44, 32*16*4);
        sim.write(0x48, 16*32*4);
        sim.write(0x4c, 32*32*4);
        sim.write(0x50, k); // Reuse * K / (2 * VectorSize)
        sim.write(0, 0, true);
        while (!sim.dut.evt_o) sim.tick();
        uint64_t elapsed=sim.cycle-sim.trigger;
        std::ofstream output(argv[5], std::ios::binary);
        output.write(reinterpret_cast<char *>(sim.memory.data()+sim.base[4]), sim.size[4]+sim.size[5]);
        if (!output) throw std::runtime_error("Cannot write result");
        std::cout << "MXCORE_RTL," << m << ',' << n << ',' << k << ',' << elapsed << '\n';
        sim.dut.final();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
