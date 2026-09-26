// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

class SyncChecks : public vp::Component {
    vp::Trace trace;
    vp::ClockEvent step, watchdog;
    std::vector<vp::IoMaster> sources;
    std::vector<vp::IoSlave> targets;
    std::vector<vp::IoReq> requests;
    std::vector<uint32_t> data, memory, expected, received;
    unsigned nx, ny, count, phase = 0, pending = 0, responses = 0;
    uint64_t remote_base, l1_size, wakeup_addr, notifications = 0;

    void send(unsigned source, uint32_t xs, uint32_t ys, bool write = true) {
        auto &req = requests[source];
        req.init(); req.set_is_write(write); req.set_size(4);
        req.set_second_data(nullptr); req.status = vp::IO_REQ_OK;
        req.set_data(reinterpret_cast<uint8_t *>(&data[source]));
        std::memset(req.get_payload(), 0, req.get_payload_size());
        // Ordinary L1 addresses must ignore stale collective metadata.
        req.get_payload()[0] = 1;
        std::memcpy(req.get_payload() + 1, &ys, 4);
        std::memcpy(req.get_payload() + 5, &xs, 4);
        if (phase < 5) {
            req.set_addr(wakeup_addr); data[source] = 1;
            for (unsigned i = 0; i < count; ++i)
                if ((xs & (1U << (i % nx))) && (ys & (1U << (i / nx)))) ++expected[i];
        } else {
            unsigned destination = write ? (source + 1) % count : source;
            req.set_addr(remote_base + destination * l1_size + 0x400);
            data[source] = write ? 0xabc00000 + source : 0;
        }
        ++pending;
        auto status = sources[source].req(&req);
        if (status != vp::IO_REQ_PENDING) trace.fatal("Sync check request was not accepted asynchronously\n");
    }

    static void tick(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<SyncChecks *>(block);
        if (self->phase == 7) {
            printf("ARCHE3D_SYNC_RESULT {\"status\":\"PASS\",\"clusters\":%u,\"phases\":7,"
                   "\"notifications\":%lu,\"responses\":%u,\"cycles\":%ld}\n",
                   self->count, self->notifications, self->responses, self->clock.get_cycles());
            self->time.get_engine()->quit(0); return;
        }
        std::fill(self->expected.begin(), self->expected.end(), 0);
        std::fill(self->received.begin(), self->received.end(), 0);
        printf("ARCHE3D_SYNC_PHASE %u\n", self->phase);
        fflush(stdout);
        switch (self->phase) {
            case 0: self->send(0, UINT32_MAX, UINT32_MAX); break;
            case 1: self->send(0, 1U << 31, 1U << 31); break;
            case 2: self->send(self->count - 1, 0x80000001, 0x80000001); break;
            case 3:
                for (unsigned i = 0; i < self->count; ++i) self->send(i, 1, 1);
                break;
            case 4:
                for (unsigned i : {0U, self->nx - 1, self->count - self->nx, self->count - 1})
                    self->send(i, UINT32_MAX, UINT32_MAX);
                break;
            case 5:
            case 6:
                for (unsigned i = 0; i < self->count; ++i)
                    self->send(i, UINT32_MAX, UINT32_MAX, self->phase == 5);
                break;
        }
    }

    static vp::IoReqStatus target(vp::Block *block, vp::IoReq *req, int cluster) {
        auto self = static_cast<SyncChecks *>(block);
        if (req->get_size() != 4) return vp::IO_REQ_INVALID;
        if (req->get_addr() == self->wakeup_addr) {
            if (!req->get_is_write()) return vp::IO_REQ_INVALID;
            ++self->received[cluster]; ++self->notifications;
        } else {
            if (req->get_addr() != 0x400) return vp::IO_REQ_INVALID;
            if (req->get_is_write()) std::memcpy(&self->memory[cluster], req->get_data(), 4);
            else std::memcpy(req->get_data(), &self->memory[cluster], 4);
        }
        return vp::IO_REQ_OK;
    }

    static void response(vp::Block *block, vp::IoReq *req, int source) {
        auto self = static_cast<SyncChecks *>(block);
        if (req->status != vp::IO_REQ_OK) self->trace.fatal("Invalid sync response\n");
        if (self->phase == 6 && self->data[source] != 0xabc00000 + (source + self->count - 1) % self->count)
            self->trace.fatal("Remote L1 response mismatch\n");
        ++self->responses;
        if (--self->pending == 0) {
            if (self->received != self->expected) self->trace.fatal("Multicast destination/count mismatch in phase %u\n", self->phase);
            ++self->phase;
            self->step.enqueue();
        }
    }

    static void timeout(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<SyncChecks *>(block);
        self->trace.fatal("Sync regression timeout in phase %u with %u outstanding\n", self->phase, self->pending);
    }
public:
    explicit SyncChecks(vp::ComponentConf &config)
        : vp::Component(config), step(this, tick), watchdog(this, timeout) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        auto cfg = get_js_config(); nx = cfg->get_uint("nx"); ny = cfg->get_uint("ny"); count = nx * ny;
        remote_base = cfg->get_uint("remote_base"); l1_size = cfg->get_uint("l1_size");
        wakeup_addr = cfg->get_uint("wakeup_addr");
        sources.resize(count); targets.resize(count); requests.resize(count);
        data.resize(count); memory.resize(count); expected.resize(count); received.resize(count);
        for (unsigned i = 0; i < count; ++i) {
            sources[i].set_resp_meth_muxed(response, i);
            targets[i].set_req_meth_muxed(target, i);
            new_master_port("source_" + std::to_string(i), &sources[i]);
            new_slave_port("target_" + std::to_string(i), &targets[i]);
        }
    }
    void reset(bool active) override {
        if (!active) { step.enqueue(); watchdog.enqueue(100000); }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new SyncChecks(config); }
