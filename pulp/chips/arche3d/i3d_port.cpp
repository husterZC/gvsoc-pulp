// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/itf/wire.hpp>
#include <algorithm>
#include <array>
#include <deque>
#include <vector>

// One physical I3D source per cluster. DMA and instruction-cache refills
// arbitrate here; every live downstream burst has a distinct AXI ID.
class I3dPort : public vp::Component {
    struct Transfer {
        vp::IoReq *original;
        vp::IoReq child;
        unsigned port, id;
        uint64_t address, offset = 0, received = 0;
        int64_t ready = 0;
        bool error = false;
    };
    std::array<std::unique_ptr<vp::IoSlave>, 2> inputs;
    vp::IoMaster output;
    vp::WireMaster<uint64_t> cache_refills;
    uint64_t refill_count = 0;
    vp::ClockEvent event;
    vp::Trace trace;
    std::array<std::deque<Transfer *>, 2> queues;
    std::deque<Transfer *> finished;
    std::vector<bool> ids;
    std::array<bool, 2> input_denied{};
    Transfer *denied = nullptr;
    unsigned turn = 0, next_id = 0;
    int64_t last_offer = -1;
    uint64_t memory_base, alias_base, image_size, interleave, max_bytes;

    void prepare(Transfer *t) {
        auto &r = t->child;
        r.prepare();
        uint64_t addr = t->address + t->offset;
        uint64_t bytes = std::min({t->original->get_size() - t->offset,
            max_bytes, interleave - (addr - memory_base) % interleave,
            uint64_t(4096) - addr % 4096});
        r.set_addr(addr); r.set_size(bytes);
        r.set_data(t->original->get_data() + t->offset);
        r.set_is_write(t->original->get_is_write());
        r.set_strb(t->original->get_strb() ? t->original->get_strb() + t->offset : nullptr);
        r.initiator = t; r.parent = t->original; r.burst_id = t->id;
        r.is_first = r.is_last = true; r.set_second_data(nullptr);
        r.memcheck_data = nullptr; r.second_memcheck_data = nullptr; r.memcheck_data_id = 0;
        t->received = 0;
    }
    void complete(Transfer *t, vp::IoReq *r) {
        t->error |= r->get_resp_status() != vp::IO_RESP_OK;
        t->ready = std::max(t->ready, clock.get_cycles() + r->get_full_latency());
        if (!t->original->get_is_write() && !t->error) {
            if (!r->get_data() || t->received + r->get_size() > t->child.get_size())
                trace.fatal("Invalid instruction/DMA response size\n");
            auto dest = t->original->get_data() + t->offset + t->received;
            if (dest != r->get_data()) std::copy_n(r->get_data(), r->get_size(), dest);
            t->received += r->get_size();
        }
        bool last = r->is_last;
        if (r != &t->child) r->free();
        if (!last) return;
        if (!t->error && !t->original->get_is_write() && t->received != t->child.get_size())
            trace.fatal("Incomplete instruction/DMA response\n");
        t->offset += t->child.get_size();
        finished.push_back(t);
        event.enqueue(std::max<int64_t>(1, t->ready - clock.get_cycles()));
    }
    void offer(bool retry = false) {
        if (denied && !retry) return;
        if (!retry && last_offer == clock.get_cycles()) { event.enqueue(); return; }
        Transfer *t = denied;
        if (!t) {
            for (unsigned p = 0; p < inputs.size(); ++p) {
                unsigned port = (turn + p) % inputs.size();
                if (!queues[port].empty()) { t = queues[port].front(); break; }
            }
        }
        if (!t) return;
        auto status = output.req(&t->child);
        if (status == vp::IO_REQ_DENIED) { denied = t; return; }
        denied = nullptr; last_offer = clock.get_cycles();
        queues[t->port].pop_front(); turn = (t->port + 1) % inputs.size();
        if (status == vp::IO_REQ_DONE) complete(t, &t->child);
        for (auto &q : queues) if (!q.empty()) { event.enqueue(); break; }
    }
    static vp::IoReqStatus request(vp::Block *block, vp::IoReq *r, int port) {
        auto self = static_cast<I3dPort *>(block);
        uint64_t addr = r->get_addr(), size = r->get_size();
        if (!size || !r->get_data() || !r->is_first || !r->is_last ||
            (r->get_opcode() != vp::IoReqOpcode::READ && r->get_opcode() != vp::IoReqOpcode::WRITE) ||
            (port && (addr < self->alias_base || addr - self->alias_base >= self->image_size ||
                       size > self->image_size - (addr - self->alias_base))) ||
            (port == 1 && r->get_is_write())) {
            r->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE;
        }
        unsigned id = self->next_id, searched = 0;
        while (searched < self->ids.size() && self->ids[id]) {
            id = (id + 1) % self->ids.size(); ++searched;
        }
        if (searched == self->ids.size()) {
            self->input_denied[port] = true; return vp::IO_REQ_DENIED;
        }
        self->ids[id] = true; self->next_id = (id + 1) % self->ids.size();
        if (port == 1 && self->cache_refills.is_bound()) self->cache_refills.sync(++self->refill_count);
        auto t = new Transfer;
        t->original = r; t->port = port; t->id = id;
        t->address = port ? self->memory_base + addr - self->alias_base : addr;
        self->prepare(t); self->queues[port].push_back(t); self->offer();
        return vp::IO_REQ_GRANTED;
    }
    static vp::IoRespAck response(vp::Block *block, vp::IoReq *r) {
        auto self = static_cast<I3dPort *>(block);
        auto t = static_cast<Transfer *>(r->initiator);
        if (r->burst_id != t->id) self->trace.fatal("I3D response AXI ID mismatch\n");
        self->complete(t, r);
        return vp::IO_RESP_ACCEPTED;
    }
    static void retry(vp::Block *block, vp::IoRetryChannel) {
        static_cast<I3dPort *>(block)->offer(true);
    }
    static void response_retry(vp::Block *block, int, vp::IoRetryChannel) {
        tick(block, nullptr);
    }
    static void tick(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<I3dPort *>(block);
        auto completed = std::move(self->finished); self->finished.clear();
        for (auto t : completed) {
            if (t->ready > self->clock.get_cycles()) {
                self->finished.push_back(t);
                self->event.enqueue(t->ready - self->clock.get_cycles());
            } else if (!t->error && t->offset < t->original->get_size()) {
                self->prepare(t); self->queues[t->port].push_back(t);
            } else {
                auto r = t->original;
                r->set_resp_status(t->error ? vp::IO_RESP_INVALID : vp::IO_RESP_OK);
                r->latency = r->duration = 0;
                if (self->inputs[t->port]->resp(r) == vp::IO_RESP_DENIED) self->finished.push_back(t);
                else { self->ids[t->id] = false; delete t; }
            }
        }
        for (unsigned p = 0; p < self->inputs.size(); ++p) {
            if (self->input_denied[p]) {
                self->input_denied[p] = false; self->inputs[p]->retry();
            }
        }
        self->offer();
    }
public:
    explicit I3dPort(vp::ComponentConf &config)
        : vp::Component(config), output(retry, response), event(this, tick) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        auto cfg = get_js_config();
        memory_base = cfg->get_uint("memory_base"); alias_base = cfg->get_uint("alias_base");
        image_size = cfg->get_uint("image_size"); interleave = cfg->get_uint("interleave");
        max_bytes = cfg->get_uint("max_bytes"); ids.resize(1u << cfg->get_uint("id_width"));
        for (unsigned i = 0; i < inputs.size(); ++i) {
            inputs[i] = std::make_unique<vp::IoSlave>(i, request, response_retry);
            new_slave_port("input_" + std::to_string(i), inputs[i].get());
        }
        new_master_port("output", &output);
        new_master_port("cache_refills", &cache_refills);
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new I3dPort(config); }
