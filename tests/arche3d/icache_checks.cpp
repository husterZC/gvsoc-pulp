// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/itf/wire.hpp>
#include <array>
#include <cstdio>
#include "../../pulp/chips/arche3d/icache_preload.hpp"

class Checks : public vp::Component {
    static constexpr uint64_t A = 0x800001c0, B = A + 64, C = A + 32768;
    vp::IoMaster fetch;
    vp::IoSlave memory;
    vp::WireMaster<bool> flush;
    vp::WireMaster<Arche3dIcachePreload> preload;
    vp::ClockEvent step, respond, watchdog;
    vp::Trace trace;
    std::array<vp::IoReq, 3> reqs;
    std::array<std::array<uint8_t, 64>, 3> buffers;
    vp::IoReq *pending = nullptr;
    unsigned phase = 0, replies = 0, refills = 0, hits = 0;
    static uint8_t pattern(uint64_t addr) {
        return (addr >> 6) + (addr >> 15) * 17 + 13 * (addr & 63);
    }
    void check(vp::IoReq *req) {
        if (req->get_resp_status() != vp::IO_RESP_OK) trace.fatal("Unexpected cache error\n");
        for (uint64_t i = 0; i < req->get_size(); ++i)
            if (req->get_data()[i] != pattern(req->get_addr() + i))
                trace.fatal("Incorrect cache data at 0x%lx\n", req->get_addr() + i);
    }
    vp::IoReqStatus send(unsigned slot, uint64_t addr, unsigned size = 32, bool write = false) {
        auto &req = reqs[slot];
        req.prepare(); req.set_addr(addr); req.set_size(size); req.set_data(buffers[slot].data());
        req.set_is_write(write); req.is_first = req.is_last = true;
        req.initiator = nullptr; req.parent = nullptr; req.burst_id = slot;
        return fetch.req(&req);
    }
    void hit(unsigned slot, uint64_t addr) {
        if (!pending || send(slot, addr) != vp::IO_REQ_DONE || reqs[slot].get_full_latency())
            trace.fatal("Resident cache hit stalled behind an unrelated refill\n");
        check(&reqs[slot]); ++hits;
    }
    void miss(unsigned slot, uint64_t addr) {
        if (send(slot, addr) != vp::IO_REQ_GRANTED) trace.fatal("Expected an asynchronous cache miss\n");
    }
    static vp::IoReqStatus read(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<Checks *>(block);
        if (self->pending || req->get_is_write() || req->get_size() != 64)
            self->trace.fatal("Invalid cache refill\n");
        self->pending = req; ++self->refills;
        // Fill the destination before the response: a victim's old tag must
        // already be invalid even though this acknowledgement is still pending.
        for (uint64_t i = 0; i < req->get_size(); ++i) req->get_data()[i] = pattern(req->get_addr() + i);
        self->respond.enqueue(100);
        return vp::IO_REQ_GRANTED;
    }
    static void reply(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<Checks *>(block);
        auto req = self->pending; self->pending = nullptr;
        self->memory.resp(req);
    }
    static vp::IoRespAck response(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<Checks *>(block);
        self->check(req);
        if (++self->replies == (self->phase == 3 ? 2u : 1u)) self->step.enqueue();
        return vp::IO_RESP_ACCEPTED;
    }
    static void tick(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<Checks *>(block);
        switch (self->phase++) {
            case 0:
                if (self->send(0, A) != vp::IO_REQ_DONE ||
                    self->reqs[0].get_full_latency() || self->refills)
                    self->trace.fatal("Preloaded instruction line missed or consumed cycles\n");
                self->check(&self->reqs[0]); self->step.enqueue(); break;
            case 1:
                self->replies = 0;
                self->miss(0, B); self->hit(1, A); break;
            case 2:
                self->replies = 0;
                self->miss(0, C);
                self->hit(1, B);
                self->miss(2, A); // Same set as C: old A must not hit overwritten data.
                break;
            case 3:
                if (self->replies != 2) self->trace.fatal("Missing queued victim response\n");
                if (self->send(0, B, 4, true) != vp::IO_REQ_DONE ||
                    self->reqs[0].get_resp_status() != vp::IO_RESP_INVALID)
                    self->trace.fatal("Instruction cache accepted a write\n");
                if (self->send(0, B + 48, 32) != vp::IO_REQ_DONE ||
                    self->reqs[0].get_resp_status() != vp::IO_RESP_INVALID)
                    self->trace.fatal("Instruction cache accepted a straddling read\n");
                self->flush.sync(true); self->replies = 0; self->miss(0, B); break;
            case 4:
                if (self->refills != 4 || self->hits != 2 || self->pending)
                    self->trace.fatal("Cache transaction accounting mismatch\n");
                printf("ARCHE3D_ICACHE_RESULT {\"status\":\"PASS\",\"preloaded_lines\":1,\"refills\":%u,"
                       "\"hits_during_refill\":%u,\"cycles\":%ld}\n",
                       self->refills, self->hits, self->clock.get_cycles());
                self->time.get_engine()->quit(0); break;
        }
    }
    static void timeout(vp::Block *block, vp::ClockEvent *) {
        static_cast<Checks *>(block)->trace.fatal("Cache regression watchdog\n");
    }
public:
    explicit Checks(vp::ComponentConf &config)
        : vp::Component(config), fetch(nullptr, response), memory(read),
          step(this, tick), respond(this, reply), watchdog(this, timeout) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        new_master_port("fetch", &fetch); new_slave_port("memory", &memory);
        new_master_port("flush", &flush);
        new_master_port("preload", &preload);
    }
    void reset(bool active) override {
        if (!active) {
            std::array<uint8_t, 64> data;
            for (unsigned i = 0; i < data.size(); ++i) data[i] = pattern(A + i);
            preload.sync({A, data.data(), data.size()});
            step.enqueue(); watchdog.enqueue(10000);
        }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new Checks(config); }
