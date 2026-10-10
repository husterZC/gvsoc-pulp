// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

class Checks : public vp::Component {
    struct Case { std::array<uint32_t,8> h; std::array<std::vector<uint8_t>,4> data; };
    vp::Trace trace;
    vp::IoMaster memory, bus, low, mid, control;
    vp::ClockEvent event, watchdog;
    std::vector<Case> cases;
    std::array<vp::IoReq, 5> priority_req;
    std::array<uint32_t, 5> priority_data{0x11, 0, 0x33, 0x44, 0x55};
    std::array<int64_t, 5> priority_done{};
    vp::IoReq req, wait_req;
    uint32_t wait_value = 0;
    std::array<uint8_t,512> buffer;
    unsigned phase = 0, which = 0, array = 0, offset = 0, replies = 0;
    unsigned cycles = 0, traffic = 0;
    bool pending = false, contended = false;
    static constexpr unsigned bases[4] = {0x1000, 0x64000, 0x23000, 0x66000};

    uint32_t mmio(unsigned index, uint32_t value=0, bool write=false) {
        vp::IoReq r; r.init(); r.set_addr(index*4); r.set_size(4);
        r.set_data(reinterpret_cast<uint8_t *>(&value)); r.set_is_write(write);
        if (control.req(&r) != vp::IO_REQ_OK) trace.fatal("Unexpected layout MMIO failure\n");
        return value;
    }
    void submit(vp::IoMaster &port, unsigned id, unsigned addr, bool write) {
        auto &r = priority_req[id]; r.init(); r.set_addr(addr); r.set_size(4);
        r.set_data(reinterpret_cast<uint8_t *>(&priority_data[id])); r.set_is_write(write);
        if (port.req(&r) != vp::IO_REQ_PENDING) trace.fatal("L1 must queue accesses\n");
    }
    static void response(vp::Block *block, vp::IoReq *r) {
        auto self = static_cast<Checks *>(block);
        if (r->status != vp::IO_REQ_OK) self->trace.fatal("L1 request failed\n");
        if (self->phase == 1) {
            self->priority_done[r - self->priority_req.data()] = self->clock.get_cycles();
            if (++self->replies == 5) { self->phase = 2; self->event.enqueue(); }
            return;
        }
        if (r == &self->wait_req) {
            self->cycles = self->mmio(11); self->traffic = self->mmio(12);
            if (self->wait_value || self->mmio(10) != 4) self->trace.fatal("Layout command failed\n");
            self->phase = 5; self->array = 2; self->offset = 0;
        } else {
            self->pending = false;
            auto &expected = self->cases[self->which].data[self->array];
            if (self->phase == 5 && std::memcmp(r->get_data(), expected.data() + self->offset, r->get_size()))
                self->trace.fatal("Layout data mismatch: case %u mode %u array %u offset %u\n",
                    self->which, self->cases[self->which].h[0], self->array, self->offset);
            self->offset += r->get_size();
        }
        self->event.enqueue();
    }
    void move_array(bool write) {
        auto &c = cases[which];
        while (array < (write ? 2u : 4u) && offset == c.data[array].size()) { ++array; offset = 0; }
        if (array == (write ? 2u : 4u)) { phase = write ? 4 : 6; event.enqueue(); return; }
        unsigned size = std::min<unsigned>(512, c.data[array].size() - offset);
        req.init(); req.set_addr(bases[array] + offset); req.set_size(size); req.set_is_write(write);
        req.set_data(write ? c.data[array].data() + offset : buffer.data());
        pending = true;
        if (memory.req(&req) != vp::IO_REQ_PENDING) trace.fatal("Expected queued L1 transfer\n");
    }
    static void step(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<Checks *>(block);
        switch (self->phase) {
            case 0:
                self->phase = 1;
                // Submit lower priorities first, to catch callback-order latency models.
                self->submit(self->low, 0, 0, true);
                self->submit(self->low, 4, 4, true);
                self->submit(self->mid, 1, 0, false);
                self->submit(self->memory, 2, 0, true);
                self->submit(self->bus, 3, 0, true);
                break;
            case 2:
                if (!(self->priority_done[2] < self->priority_done[1] &&
                      self->priority_done[3] < self->priority_done[1] &&
                      self->priority_done[1] < self->priority_done[0]) || self->priority_data[1] != 0x44)
                    self->trace.fatal("Priority/data visibility mismatch\n");
                printf("ARCHE3D_L1_PRIORITY_PASS dma_bus_before_hwpe_before_scalar\n");
                self->phase = 3; self->event.enqueue(); break;
            case 3: if (!self->pending) self->move_array(true); break;
            case 4: {
                auto &c = self->cases[self->which];
                if (self->mmio(13) != 0 || self->mmio(13) != UINT32_MAX)
                    self->trace.fatal("Layout acquisition must be exclusive\n");
                self->mmio(0,c.h[1],true); self->mmio(1,c.h[2],true);
                self->mmio(2,bases[0],true); self->mmio(3,bases[2],true);
                self->mmio(4,c.h[3],true); self->mmio(7,c.h[0],true);
                self->mmio(8,bases[1],true); self->mmio(9,bases[3],true);
                if (self->mmio(15) != 5 || self->mmio(5)) self->trace.fatal("Layout start failed\n");
                self->phase = 7;
                self->wait_req.init(); self->wait_req.set_addr(24); self->wait_req.set_size(4);
                self->wait_req.set_data(reinterpret_cast<uint8_t *>(&self->wait_value));
                self->wait_req.set_is_write(false);
                if (self->control.req(&self->wait_req) != vp::IO_REQ_PENDING)
                    self->trace.fatal("Layout wait must block\n");
                break;
            }
            case 5: if (!self->pending) self->move_array(false); break;
            case 6: {
                auto &c = self->cases[self->which];
                unsigned bytes = 0; for (auto &a : c.data) bytes += a.size();
                if (self->traffic != bytes) self->trace.fatal("Layout byte accounting mismatch\n");
                double efficiency = double(bytes) / (512 * self->cycles);
                if (c.h[0] && c.h[1] * c.h[2] == 65536 && efficiency < 0.90)
                    self->trace.fatal("Bulk streaming uses less than 90%% of shared HWPE bandwidth: "
                                      "mode=%u rows=%u cols=%u bytes=%u cycles=%u\n",
                                      c.h[0], c.h[1], c.h[2], bytes, self->cycles);
                if (c.h[0] && c.h[1] * c.h[2] == 32 && self->cycles != 9)
                    self->trace.fatal("Small conversion lost its five-cycle pipeline latency\n");
                if (c.h[0] >= 6 && c.h[1] * c.h[2] == 1024 && self->cycles != 10)
                    self->trace.fatal("MX reblocking lost its five-cycle pipeline latency\n");
                printf("ARCHE3D_LAYOUT_RESULT {\"mode\":%u,\"rows\":%u,\"cols\":%u,\"elements\":%u,\"bytes\":%u,\"cycles\":%u,"
                       "\"bytes_per_cycle\":%.3f,\"status\":\"PASS\"}\n",
                    c.h[0], c.h[1], c.h[2], c.h[1]*c.h[2], bytes, self->cycles, double(bytes)/self->cycles);
                if (++self->which == self->cases.size()) {
                    self->mmio(7,1,true); self->mmio(0,1,true); self->mmio(1,31,true);
                    if (self->mmio(5) != 2) self->trace.fatal("Partial MX block accepted\n");
                    self->mmio(1,32,true); self->mmio(3,bases[0],true);
                    if (self->mmio(5) != 2) self->trace.fatal("Overlapping buffers accepted\n");
                    self->mmio(3,bases[2],true); self->mmio(8,0x6c000,true);
                    if (self->mmio(5) != 2) self->trace.fatal("Out-of-L1 scales accepted\n");
                    for (unsigned mode : {6u, 7u}) {
                        self->mmio(7,mode,true); self->mmio(0,16,true); self->mmio(1,64,true);
                        self->mmio(8,bases[1],true);
                        if (self->mmio(5) != 2) self->trace.fatal("Partial MX reblock rows accepted\n");
                        self->mmio(0,64,true); self->mmio(1,16,true);
                        if (self->mmio(5) != 2) self->trace.fatal("Partial MX reblock columns accepted\n");
                        self->mmio(0,32,true); self->mmio(1,32,true);
                        self->mmio(9,bases[1],true);
                        if (self->mmio(5) != 2) self->trace.fatal("Aliased MX scale outputs accepted\n");
                        self->mmio(9,0x6c000,true);
                        if (self->mmio(5) != 2) self->trace.fatal("Out-of-L1 MX scale output accepted\n");
                        self->mmio(9,bases[3],true); self->mmio(3,bases[0]+32,true);
                        if (self->mmio(5) != 2) self->trace.fatal("Overlapping MX payloads accepted\n");
                        self->mmio(3,bases[2],true);
                    }
                    printf("ARCHE3D_LAYOUT_INVALID_PASS\n");
                    self->time.get_engine()->quit(0); break;
                }
                self->array = self->offset = 0; self->phase = 3; self->event.enqueue(); break;
            }
        }
    }
    static void timeout(vp::Block *block, vp::ClockEvent *) {
        static_cast<Checks *>(block)->trace.fatal("Layout regression timed out\n");
    }
public:
    explicit Checks(vp::ComponentConf &config) : vp::Component(config), event(this,step), watchdog(this,timeout) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        for (auto p : {&memory,&bus,&low,&mid,&control}) p->set_resp_meth(response);
        new_master_port("memory",&memory); new_master_port("bus",&bus);
        new_master_port("low",&low); new_master_port("mid",&mid); new_master_port("control",&control);
        std::ifstream input(get_js_config()->get("fixtures")->get_str(), std::ios::binary);
        uint32_t count = 0; input.read(reinterpret_cast<char *>(&count),4);
        if (!input || !count) trace.fatal("Run layout_fixtures.py before this test\n");
        cases.resize(count);
        for (auto &c : cases) {
            input.read(reinterpret_cast<char *>(c.h.data()),32);
            for (unsigned i=0;i<4;++i) {
                c.data[i].resize(c.h[4+i]);
                input.read(reinterpret_cast<char *>(c.data[i].data()),c.data[i].size());
            }
        }
        if (!input) trace.fatal("Truncated fixtures\n");
    }
    void reset(bool active) override { if (!active) { event.enqueue(); watchdog.enqueue(1000000); } }
};
constexpr unsigned Checks::bases[4];
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new Checks(config); }
