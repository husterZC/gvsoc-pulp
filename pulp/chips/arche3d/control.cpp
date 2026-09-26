// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include "dma_access.hpp"

class Control : public vp::Component {
    struct Stats {
        uint64_t issued = 0, completed = 0, bytes = 0, expected = 0, burst_bytes = 0;
        std::set<unsigned> ids;
        bool exited = false;
    };
    std::vector<vp::IoSlave> inputs;
    std::vector<vp::WireSlave<Arche3dDmaEvent>> activities;
    vp::WireMaster<bool> ready;
    vp::ClockEvent boot, release, progress;
    vp::Trace trace;
    std::vector<vp::IoReq *> waiters;
    std::vector<Stats> stats;
    unsigned nx, ny, count, arrived = 0, exited = 0;
    uint64_t memory_base, interleave, axi_bytes, issued = 0, completed = 0, bytes = 0, peak = 0;
    int64_t first = -1, last = -1, inject = -1, begin = -1, progress_cycles, watchdog;
    std::chrono::steady_clock::time_point wall;
    static void start(vp::Block *block, vp::ClockEvent *) {
        static_cast<Control *>(block)->ready.sync(true);
    }
    static void release_barrier(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<Control *>(block);
        auto waiters = self->waiters;
        std::fill(self->waiters.begin(), self->waiters.end(), nullptr);
        self->arrived = 0;
        self->begin = self->clock.get_cycles();
        for (auto req : waiters) req->get_resp_port()->resp(req);
    }
    static void tick(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<Control *>(block);
        int64_t now = self->clock.get_cycles();
        if (now >= self->watchdog) self->trace.fatal("arche3d software watchdog expired\n");
        printf("ARCHE3D_PROGRESS {\"cycle\":%ld,\"issued\":%lu,\"completed\":%lu,\"clusters_done\":%u}\n",
            now, self->issued, self->completed, self->exited);
        fflush(stdout);
        self->progress.enqueue(self->progress_cycles);
    }
    static void activity(vp::Block *block, Arche3dDmaEvent evt, int cluster) {
        auto self = static_cast<Control *>(block);
        auto &s = self->stats[cluster];
        int64_t now = self->clock.get_cycles();
        if (!evt.completed) {
            if (!s.ids.insert(evt.id).second) self->trace.fatal("Duplicate live I3D AXI ID\n");
            if (s.expected) {
                unsigned terminal = (cluster % self->nx) * self->ny + cluster / self->nx;
                uint64_t destination = (terminal + s.issued) % self->count;
                if (evt.write || evt.size != s.burst_bytes ||
                    evt.address != self->memory_base + destination * self->interleave)
                    self->trace.fatal("All-to-all DMA sequence mismatch in cluster %u\n", cluster);
            }
            ++s.issued; ++self->issued;
            s.bytes += evt.size; self->bytes += evt.size;
            if (self->first < 0) self->first = now;
            self->inject = now;
            self->peak = std::max(self->peak, self->issued - self->completed);
        } else {
            if (!s.ids.erase(evt.id)) self->trace.fatal("Unknown I3D DMA completion ID\n");
            ++s.completed; ++self->completed; self->last = now;
        }
    }
    void finish() {
        for (auto &s : stats)
            if (s.issued != s.completed || !s.ids.empty() || (s.expected && s.completed != s.expected))
                trace.fatal("Software exited with incomplete or missing DMA transfers\n");
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall).count();
        printf("ARCHE3D_RESULT {\"status\":\"PASS\",\"clusters\":%u,\"transactions\":%lu,\"bytes\":%lu,"
               "\"peak_outstanding\":%lu,\"network_cycles\":%.1f,\"injection_cycles\":%.1f,"
               "\"software_cycles\":%ld,\"total_cycles\":%ld,\"wall_seconds\":%.6f}\n",
            count, completed, bytes, peak, first < 0 ? 0.0 : last - first + 0.5,
            first < 0 ? 0.0 : inject - first + 0.5,
            begin < 0 ? 0 : clock.get_cycles() - begin, clock.get_cycles(), seconds);
        fflush(stdout);
        time.get_engine()->quit(0);
    }
    static vp::IoReqStatus input(vp::Block *block, vp::IoReq *req, int cluster) {
        auto self = static_cast<Control *>(block);
        uint64_t addr = req->get_addr();
        if (req->get_size() != 4) return vp::IO_REQ_INVALID;
        uint32_t value = 0;
        std::memcpy(&value, req->get_data(), 4);
        if (!req->get_is_write()) {
            switch (addr) {
                case 0x08: value = self->clock.get_cycles(); break;
                case 0x0c: value = uint64_t(self->clock.get_cycles()) >> 32; break;
                default: return vp::IO_REQ_INVALID;
            }
            std::memcpy(req->get_data(), &value, 4);
            return vp::IO_REQ_OK;
        }
        switch (addr) {
            case 0:
                if (value) {
                    fprintf(stderr, "ARCHE3D_FAIL cluster=%d code=%u\n", cluster, value);
                    self->time.get_engine()->quit(1);
                } else {
                    if (self->stats[cluster].exited) self->trace.fatal("Cluster exited twice\n");
                    self->stats[cluster].exited = true;
                    if (++self->exited == self->count) self->finish();
                }
                break;
            case 0x04: break; // Reserved system wakeup register.
            case 0x10: std::putchar(value); std::fflush(stdout); break;
            case 0x100:
                if (self->waiters[cluster]) self->trace.fatal("Duplicate global barrier arrival\n");
                self->waiters[cluster] = req;
                if (++self->arrived == self->count) self->release.enqueue();
                return vp::IO_REQ_PENDING;
            case 0x104: self->stats[cluster].expected = value; break;
            case 0x108: self->stats[cluster].burst_bytes = value; break;
            default: return vp::IO_REQ_INVALID;
        }
        return vp::IO_REQ_OK;
    }
public:
    explicit Control(vp::ComponentConf &config)
        : vp::Component(config), boot(this, start), release(this, release_barrier), progress(this, tick) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        auto cfg = get_js_config(); nx = cfg->get_uint("nx"); ny = cfg->get_uint("ny"); count = nx * ny;
        memory_base = cfg->get_uint("memory_base"); interleave = cfg->get_uint("interleave");
        axi_bytes = cfg->get_uint("axi_bytes"); progress_cycles = cfg->get_int("progress_cycles");
        watchdog = cfg->get_int("watchdog_cycles");
        if (progress_cycles <= 0) progress_cycles = watchdog;
        stats.resize(count); waiters.resize(count); inputs.resize(count); activities.resize(count);
        for (unsigned i = 0; i < count; ++i) {
            inputs[i].set_req_meth_muxed(input, i);
            new_slave_port("input_" + std::to_string(i), &inputs[i]);
            activities[i].set_sync_meth_muxed(activity, i);
            new_slave_port("activity_" + std::to_string(i), &activities[i]);
        }
        new_master_port("ready", &ready);
    }
    void reset(bool active) override {
        if (!active) {
            wall = std::chrono::steady_clock::now();
            boot.enqueue(); progress.enqueue(progress_cycles);
        }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new Control(config); }
