// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <algorithm>
#include <array>
#include <deque>
#include <vector>

// Requests retain ownership until their response. No memory is touched at
// submission: arbitration runs on the next eligible edge, independent of the
// order in which masters were called. Each bank serves one word per cycle.
class L1Fabric : public vp::Component {
    struct Fragment { uint64_t offset, bytes; bool done = false; };
    struct Pending {
        vp::IoReq *req;
        int64_t ready;
        std::vector<Fragment> fragments;
        unsigned left;
        vp::IoReqStatus status = vp::IO_REQ_OK;
    };
    vp::Trace trace;
    vp::ClockEvent event;
    std::vector<vp::IoSlave> inputs;
    std::vector<vp::IoMaster> outputs;
    std::vector<std::deque<Pending>> queues;
    std::array<std::vector<unsigned>, 3> priorities;
    std::array<unsigned, 3> next{};
    uint64_t size;
    unsigned banks, word, hwpe_bandwidth;

    static vp::IoReqStatus request(vp::Block *block, vp::IoReq *req, int port) {
        auto self = static_cast<L1Fabric *>(block);
        const uint64_t addr = req->get_addr(), bytes = req->get_size();
        if (!bytes || !req->get_data() || addr >= self->size || bytes > self->size - addr)
            return vp::IO_REQ_INVALID;
        // Atomics must stay within one bank word; never split a reservation.
        if (req->get_opcode() > vp::WRITE &&
            (bytes > self->word || addr % bytes || addr % self->word + bytes > self->word))
            return vp::IO_REQ_INVALID;
        Pending p{req, self->clock.get_cycles() + std::max<uint64_t>(1, req->get_latency()), {}, 0};
        for (uint64_t offset = 0; offset < bytes;) {
            uint64_t chunk = std::min<uint64_t>(bytes - offset, self->word - (addr + offset) % self->word);
            p.fragments.push_back({offset, chunk});
            offset += chunk;
        }
        p.left = p.fragments.size();
        self->queues[port].push_back(std::move(p));
        // Latency already consumed before service must not be charged again.
        req->set_exact_latency(0);
        self->event.enqueue();
        return vp::IO_REQ_PENDING;
    }

    static void step(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<L1Fabric *>(block);
        std::vector<bool> used(self->banks, false);
        unsigned hwpe_left = self->hwpe_bandwidth;
        std::vector<vp::IoReq *> complete;
        for (unsigned priority = 0; priority < 3; ++priority) {
            auto &ports = self->priorities[priority];
            for (unsigned n = 0; n < ports.size(); ++n) {
                auto &queue = self->queues[ports[(self->next[priority] + n) % ports.size()]];
                while (!queue.empty() && queue.front().ready <= self->clock.get_cycles()) {
                    auto &p = queue.front();
                    auto req = p.req;
                    for (auto &f : p.fragments) {
                        if (f.done) continue;
                        const uint64_t addr = req->get_addr() + f.offset;
                        const unsigned bank = (addr / self->word) % self->banks;
                        if (used[bank] || (priority == 1 && hwpe_left < self->word)) continue;
                        used[bank] = true;
                        if (priority == 1) hwpe_left -= self->word;
                        vp::IoReq part;
                        part.init();
                        part.set_addr((addr / (self->word * self->banks)) * self->word + addr % self->word);
                        part.set_size(f.bytes);
                        part.set_opcode(req->get_opcode());
                        part.set_initiator(req->get_initiator());
                        part.set_data(req->get_data() + f.offset);
                        part.set_second_data(req->get_second_data() ? req->get_second_data() + f.offset : nullptr);
                        part.set_memcheck_data(req->get_memcheck_data() ? req->get_memcheck_data() + f.offset : nullptr);
                        part.set_second_memcheck_data(req->get_second_memcheck_data() ? req->get_second_memcheck_data() + f.offset : nullptr);
                        auto status = self->outputs[bank].req(&part);
                        if (status == vp::IO_REQ_PENDING || status == vp::IO_REQ_DENIED)
                            self->trace.fatal("L1Fabric requires synchronous storage banks\n");
                        if (status != vp::IO_REQ_OK) p.status = vp::IO_REQ_INVALID;
                        f.done = true;
                        --p.left;
                    }
                    if (!p.left) {
                        req->status = p.status;
                        complete.push_back(req);
                        queue.pop_front();
                    } else break;
                }
            }
            if (!ports.empty()) self->next[priority] = (self->next[priority] + 1) % ports.size();
        }
        // Callbacks may submit new traffic; it is eligible on the next edge.
        for (auto req : complete) req->get_resp_port()->resp(req);
        for (const auto &queue : self->queues)
            if (!queue.empty()) { self->event.enqueue(); break; }
    }
public:
    explicit L1Fabric(vp::ComponentConf &config) : vp::Component(config), event(this, step) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        banks = get_js_config()->get_int("banks");
        word = get_js_config()->get_int("bank_width");
        size = get_js_config()->get_uint("size");
        hwpe_bandwidth = get_js_config()->get_int("hwpe_bandwidth");
        unsigned low = get_js_config()->get_int("low_ports");
        unsigned hwpe = get_js_config()->get_int("hwpe_ports");
        inputs.resize(low + 3 + hwpe);
        queues.resize(inputs.size());
        outputs.resize(banks);
        for (unsigned i = 0; i < inputs.size(); ++i) {
            std::string name = i < low ? "in_" + std::to_string(i) :
                i == low ? "dma_input" : i == low + 1 ? "bus_input" :
                i == low + 2 ? "sync_input" : "hwpe_" + std::to_string(i - low - 3);
            inputs[i].set_req_meth_muxed(request, i);
            new_slave_port(name, &inputs[i]);
            priorities[i < low ? 2 : i < low + 3 ? 0 : 1].push_back(i);
        }
        for (unsigned i = 0; i < banks; ++i) new_master_port("out_" + std::to_string(i), &outputs[i]);
    }
    void reset(bool active) override {
        if (active) { event.cancel(); for (auto &q : queues) q.clear(); next.fill(0); }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new L1Fabric(config); }
