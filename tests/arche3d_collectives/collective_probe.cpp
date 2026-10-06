// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

// Exercise real endpoint MMIO, L1 timing, router queues, and unicast contention.
// Expected values below use integers or exactly representable FP constants;
// they do not call the implementation's reduction helper.
class CollectiveProbe : public vp::Component
{
    struct Job
    {
        unsigned slot, column, root, op, epoch, bytes;
        bool posted = false;
        bool masked = false;
        unsigned root_line = 0, x_mask = 0, y_mask = 0;
    };
    struct Delayed
    {
        vp::IoReq *req;
        int node;
        int64_t due;
    };
    std::vector<vp::IoMaster> control, out;
    std::vector<vp::IoSlave> memory;
    std::vector<std::vector<uint8_t>> data;
    std::vector<vp::IoReq *> writes;
    std::deque<Delayed> delayed;
    std::vector<Job> jobs;
    vp::ClockEvent event;
    vp::Trace trace;
    unsigned nx, ny, l1_base, phase = 0, backgrounds = 0;
    int64_t start = 0;
    bool stream_perf = false;
    bool launched = false, delayed_send = false, corrupted = false;
    uint32_t reg(unsigned node, unsigned slot, unsigned offset, bool write, uint32_t value = 0)
    {
        vp::IoReq req;
        req.init();
        req.set_addr(slot * 64 + offset);
        req.set_size(4);
        req.set_is_write(write);
        req.set_data(reinterpret_cast<uint8_t *>(&value));
        if (control[node].req(&req) != vp::IO_REQ_OK)
        {
            trace.fatal("Collective MMIO failed\n");
        }
        return value;
    }
    unsigned position(const Job &j, unsigned node) { return j.column ? node / nx : node % nx; }
    unsigned line(const Job &j, unsigned node)
    {
        return j.masked ? j.root_line : (j.column ? node % nx : node / nx);
    }
    unsigned length(const Job &j) { return j.column ? ny : nx; }
    bool member(const Job &j, unsigned node)
    {
        if (!j.masked)
        {
            return true;
        }
        unsigned rx = j.column ? j.root_line : j.root, ry = j.column ? j.root : j.root_line;
        return ((node % nx) & j.x_mask) == (rx & j.x_mask) &&
               ((node / nx) & j.y_mask) == (ry & j.y_mask);
    }
    bool root(const Job &j, unsigned node)
    {
        return position(j, node) == j.root &&
               (!j.masked || (j.column ? node % nx : node / nx) == j.root_line);
    }
    bool sends(const Job &j, unsigned node)
    {
        return member(j, node) && (j.op != 1 || root(j, node));
    }
    bool receives(const Job &j, unsigned node)
    {
        return member(j, node) && (j.op == 1 || root(j, node));
    }
    unsigned source(const Job &j, unsigned node)
    {
        return 0x1000 + j.slot * 512 + (j.op == 1 || j.op >= 10 ? 1 : 0) + (node % 7) * 2;
    }
    unsigned destination(const Job &j, unsigned node) { return source(j, node) + 0x7000; }
    uint16_t operand(const Job &j, unsigned pos)
    {
        switch (j.op)
        {
        case 2:
        case 5:
            return 60000 + pos;
        case 3:
        case 6:
            return uint16_t(-32 + int(pos));
        case 4:
        case 7:
            return pos == 0 ? 0x3c00 : 0;
        case 8:
        case 9:
            return pos == 0 ? 0x3f80 : 0;
        case 10:
        case 11:
            return pos == 0 ? 0x3c : 0;
        default:
            return pos == 0 ? 0x38 : 0;
        }
    }
    uint16_t expected(const Job &j)
    {
        if (j.masked && j.op == 2)
        {
            unsigned value = 0;
            for (unsigned node = 0; node < nx * ny; ++node)
            {
                if (member(j, node))
                {
                    value += node + 1;
                }
            }
            return uint16_t(value);
        }
        unsigned n = length(j);
        switch (j.op)
        {
        case 2:
            return uint16_t(n * 60000 + n * (n - 1) / 2);
        case 3:
            return uint16_t(-32 * int(n) + int(n * (n - 1) / 2));
        case 5:
            return 60000 + n - 1;
        case 6:
            return uint16_t(-32 + int(n) - 1);
        default:
            return operand(j, 0);
        }
    }
    void add(unsigned slot, bool column, unsigned root, unsigned op, unsigned bytes, bool post,
             bool masked = false, unsigned root_line = 0, unsigned x_mask = 0, unsigned y_mask = 0)
    {
        Job j{slot, unsigned(column), root, op, phase * 32 + slot + 1, bytes, post};
        j.masked = masked;
        j.root_line = root_line;
        j.x_mask = x_mask;
        j.y_mask = y_mask;
        jobs.push_back(j);
        for (unsigned node = 0; node < nx * ny; ++node)
        {
            unsigned src = source(j, node), dst = destination(j, node);
            std::fill(data[node].begin() + src - 1, data[node].begin() + src + bytes + 1, 0xa5);
            std::fill(data[node].begin() + dst - 1, data[node].begin() + dst + bytes + 1, 0xa5);
            if (op == 1)
            {
                for (unsigned b = 0; b < bytes; ++b)
                {
                    data[node][src + b] = uint8_t(17 * b + line(j, node) + slot);
                }
            }
            else
            {
                uint16_t value = masked ? uint16_t(node + 1) : operand(j, position(j, node));
                unsigned elem = op >= 10 ? 1 : 2;
                for (unsigned b = 0; b < bytes; b += elem)
                {
                    std::memcpy(&data[node][src + b], &value, elem);
                }
            }
            if (!member(j, node))
            {
                continue;
            }
            reg(node, slot, 0, true,
                op | (unsigned(column) << 8) | (unsigned(masked) << 9) | (root << 16));
            reg(node, slot, 4, true, j.epoch);
            reg(node, slot, 8, true, l1_base + src);
            reg(node, slot, 12, true, l1_base + dst);
            reg(node, slot, 16, true, bytes);
            if (masked)
            {
                reg(node, slot, 28, true, x_mask | (y_mask << 16));
                reg(node, slot, 44, true, root_line);
            }
            if (post && receives(j, node))
            {
                reg(node, slot, 20, true, 1);
            }
        }
        for (unsigned node = 0; node < nx * ny; ++node)
        {
            if (sends(j, node) && !((phase == 24 || phase == 82) && position(j, node) == 0))
            {
                reg(node, slot, 20, true, 2);
            }
        }
    }
    static vp::IoReqStatus access(vp::Block *block, vp::IoReq *req, int node)
    {
        auto *self = static_cast<CollectiveProbe *>(block);
        if (req->get_addr() + req->get_size() > 0x10000)
        {
            return vp::IO_REQ_INVALID;
        }
        if (self->phase == 23 || self->phase == 83 || self->phase == 84)
        {
            int64_t delay = 7 + (node % 3);
            if (self->phase >= 83)
            {
                delay += static_cast<int64_t>((req->get_addr() / 128) % 7);
            }
            self->delayed.push_back({req, node, self->clock.get_cycles() + delay});
            return self->phase == 84 ? vp::IO_REQ_PENDING : vp::IO_REQ_DENIED;
        }
        self->copy(req, node);
        req->set_latency(2);
        return vp::IO_REQ_OK;
    }
    void copy(vp::IoReq *req, unsigned node)
    {
        auto *mem = data[node].data() + req->get_addr();
        if (req->get_is_write())
        {
            std::memcpy(mem, req->get_data(), req->get_size());
        }
        else
        {
            std::memcpy(req->get_data(), mem, req->get_size());
        }
    }
    static void response(vp::Block *block, vp::IoReq *req)
    {
        auto *self = static_cast<CollectiveProbe *>(block);
        if (req->status == vp::IO_REQ_INVALID)
        {
            self->trace.fatal("Contending unicast failed\n");
        }
        ++self->backgrounds;
    }
    static void grant(vp::Block *, vp::IoReq *) {}
    void begin()
    {
        start = clock.get_cycles();
        jobs.clear();
        delayed_send = corrupted = false;
        if (phase < 8)
        {
            bool column = phase / 4;
            unsigned n = column ? ny : nx;
            add(0, column, (phase % 4) / 2 ? n / 2 : 0, phase % 2 ? 12 : 1, 1, true);
        }
        else if (phase < 20)
        {
            unsigned op = phase - 6;
            bool column = phase & 1;
            unsigned n = column ? ny : nx;
            add(0, column, n / 2, op, op >= 10 ? 127 : 128, true);
        }
        else if (phase == 20)
        {
            add(0, false, nx / 2, 1, 128, false);
        }
        else if (phase == 24)
        {
            add(0, false, nx / 2, 12, 1, true);
        }
        else if (phase < 25)
        {
            for (unsigned slot = 0; slot < 16; ++slot)
            {
                bool column = slot & 1;
                unsigned n = column ? ny : nx;
                add(slot, column, slot % n, slot % 3 ? 12 : 1, 127, true);
            }
        }
        else if (phase < 42 || phase == 79)
        {
            const unsigned masks[8][2] = {{0, 0xffff}, {0xffff, 0},      {1, 0xffff},  {0xffff, 1},
                                          {1, 1},      {0xffff, 0xffff}, {0x18, 0x18}, {0, 0}};
            unsigned first = phase < 41 ? (phase - 25) / 2 : 0;
            unsigned count = phase < 41 ? 1 : 16;
            for (unsigned slot = 0; slot < count; ++slot)
            {
                unsigned item = (first + slot) % 8;
                bool column = phase >= 41 && (slot & 1);
                unsigned rx = item & 1 ? nx - 1 : nx / 2;
                unsigned ry = item % 3 ? ny / 2 : ny - 1;
                unsigned op = phase < 41 ? ((phase - 25) & 1 ? 2 : 1) : (slot % 3 ? 2 : 1);
                add(slot, column, column ? ry : rx, op, phase == 79 ? 256 : 128, true, true,
                    column ? rx : ry, masks[item][0], masks[item][1]);
            }
        }
        if (phase >= 42 && phase < 66)
        {
            const unsigned sizes[] = {128, 256, 129, 2049, 8192, 8193};
            bool column = ((phase - 42) / 2) & 1;
            add(0, column, (column ? ny : nx) / 2, (phase & 1) ? 12 : 1, sizes[(phase - 42) / 4],
                true);
        }
        else if (phase >= 66 && phase < 78)
        {
            unsigned op = phase - 64;
            bool column = phase & 1;
            add(0, column, (column ? ny : nx) / 2, op, op >= 10 ? 511 : 512, true);
        }
        else if (phase == 78 || phase == 83 || phase == 84)
        {
            for (unsigned slot = 0; slot < 16; ++slot)
            {
                bool column = slot & 1;
                add(slot, column, slot % (column ? ny : nx), slot % 3 ? 12 : 1, 257, true);
            }
        }
        else if (phase == 80 || phase == 81)
        {
            add(0, false, nx / 2, 1, phase == 80 ? 8193 : 256, false);
        }
        else if (phase == 82)
        {
            add(0, false, nx / 2, 12, 8193, true);
        }
        if (phase == 23 || phase == 83 || phase == 84)
        {
            backgrounds = 0;
            for (unsigned node = 0; node < nx * ny; ++node)
            {
                auto *req = new vp::IoReq();
                req->init();
                req->set_addr(0x30000000ULL + ((node + 1) % (nx * ny)) * 0x10000 + 0x6000);
                req->set_size(128);
                req->set_is_write(true);
                std::fill(data[node].begin() + 0x5000, data[node].begin() + 0x5080, uint8_t(node));
                req->set_data(data[node].data() + 0x5000);
                writes.push_back(req);
                auto status = out[node].req(req);
                if (status == vp::IO_REQ_OK || status == vp::IO_REQ_INVALID)
                {
                    req->status = status;
                    response(this, req);
                }
            }
        }
    }
    static void tick(vp::Block *block, vp::ClockEvent *)
    {
        auto *self = static_cast<CollectiveProbe *>(block);
        if (!self->launched)
        {
            self->launched = true;
            self->begin();
        }
        int64_t now = self->clock.get_cycles();
        for (auto it = self->delayed.begin(); it != self->delayed.end();)
        {
            if (it->due > now)
            {
                ++it;
                continue;
            }
            auto item = *it;
            it = self->delayed.erase(it);
            self->copy(item.req, item.node);
            item.req->status = vp::IO_REQ_OK;
            item.req->prepare();
            item.req->get_resp_port()->grant(item.req);
            item.req->get_resp_port()->resp(item.req);
        }
        if (now - self->start > 10000)
        {
            self->trace.fatal("Native collective deadlock phase=%u\n", self->phase);
        }
        if ((self->phase == 20 || self->phase == 81) && now - self->start >= 80 && !self->corrupted)
        {
            auto &j = self->jobs[0];
            for (unsigned node = 0; node < self->nx * self->ny; ++node)
            {
                if (self->sends(j, node) && !(self->reg(node, 0, 24, false) & 4))
                {
                    self->trace.fatal("Multicast send waited for unposted receivers\n");
                }
                // The source is reusable as soon as send completion is visible.
                std::fill(self->data[node].begin() + self->source(j, node),
                          self->data[node].begin() + self->source(j, node) + j.bytes, 0xee);
            }
            self->corrupted = true;
        }
        if ((self->phase == 20 || self->phase == 80 || self->phase == 81) &&
            now - self->start >= 120 && !self->jobs[0].posted)
        {
            for (unsigned node = 0; node < self->nx * self->ny; ++node)
            {
                self->reg(node, 0, 20, true, 1);
            }
            self->jobs[0].posted = true;
        }
        if ((self->phase == 24 || self->phase == 82) && now - self->start >= 80 &&
            !self->delayed_send)
        {
            auto &j = self->jobs[0];
            for (unsigned node = 0; node < self->nx * self->ny; ++node)
            {
                if (self->receives(j, node) && (self->reg(node, 0, 24, false) & 8))
                {
                    self->trace.fatal("Reduction completed without a contributor\n");
                }
                if (self->position(j, node) == 0)
                {
                    self->reg(node, 0, 20, true, 2);
                }
            }
            self->delayed_send = true;
        }
        bool done = (self->phase != 23 && self->phase != 83 && self->phase != 84) ||
                    self->backgrounds == self->nx * self->ny;
        for (auto &j : self->jobs)
        {
            for (unsigned node = 0; node < self->nx * self->ny; ++node)
            {
                unsigned status = self->reg(node, j.slot, 24, false);
                done &= (!self->sends(j, node) || (status & 4)) &&
                        (!self->receives(j, node) || (status & 8));
            }
        }
        if (done)
        {
            for (auto &j : self->jobs)
            {
                uint32_t first = UINT32_MAX, last = 0;
                uint16_t want = self->expected(j);
                for (unsigned node = 0; node < self->nx * self->ny; ++node)
                {
                    unsigned dst = self->destination(j, node), elem = j.op >= 10 ? 1 : 2;
                    if (self->sends(j, node))
                    {
                        first = std::min(first, self->reg(node, j.slot, 48, false));
                    }
                    if (self->receives(j, node))
                    {
                        last = std::max(last, self->reg(node, j.slot, 36, false));
                    }
                    if (self->data[node][dst - 1] != 0xa5 ||
                        self->data[node][dst + j.bytes] != 0xa5)
                    {
                        self->trace.fatal("Collective overwrote guards\n");
                    }
                    for (unsigned b = 0; b < j.bytes; ++b)
                    {
                        uint8_t byte = j.op == 1 ? uint8_t(17 * b + self->line(j, node) + j.slot)
                                                 : uint8_t(want >> (8 * (b % elem)));
                        if (!self->receives(j, node))
                        {
                            byte = 0xa5;
                        }
                        if (self->data[node][dst + b] != byte)
                        {
                            self->trace.fatal("Incorrect collective result phase=%u node=%u "
                                              "byte=%u got=%x expected=%x\n",
                                              self->phase, node, b, self->data[node][dst + b],
                                              byte);
                        }
                    }
                }
                if (self->stream_perf && self->phase >= 42 && self->phase < 66)
                {
                    unsigned hops = std::max(j.root, self->length(j) - 1 - j.root);
                    unsigned beats = (j.bytes + 127) / 128, expected = 2 * hops + 3 + beats - 1;
                    if (last - first != expected)
                    {
                        self->trace.fatal("Unexpected bulk latency phase=%u got=%u expected=%u\n",
                                          self->phase, last - first, expected);
                    }
                    printf("ARCHE3D_NATIVE_STREAM column=%u op=%u bytes=%u beats=%u "
                           "capture_to_receive=%u\n",
                           j.column, j.op, j.bytes, beats, last - first);
                }
                if (self->phase < 8)
                {
                    unsigned hops = std::max(j.root, self->length(j) - 1 - j.root);
                    if (last - first != 2 * hops + 3)
                    {
                        self->trace.fatal("Unexpected isolated collective latency %u expected %u\n",
                                          last - first, 2 * hops + 3);
                    }
                    printf("ARCHE3D_NATIVE_LATENCY column=%u op=%u root=%u hops=%u "
                           "capture_to_receive=%u mesh_hops=%u\n",
                           j.column, j.op, j.root, hops, last - first, 2 * hops);
                }
                if (self->phase >= 25 && self->phase < 41)
                {
                    unsigned hops = 0;
                    unsigned rx = j.column ? j.root_line : j.root,
                             ry = j.column ? j.root : j.root_line;
                    for (unsigned node = 0; node < self->nx * self->ny; ++node)
                    {
                        if (self->member(j, node))
                        {
                            unsigned x = node % self->nx, y = node / self->nx;
                            unsigned distance =
                                (x > rx ? x - rx : rx - x) + (y > ry ? y - ry : ry - y);
                            hops = std::max(hops, distance);
                        }
                    }
                    if (last - first != 2 * hops + 3)
                    {
                        self->trace.fatal("Unexpected masked collective latency %u expected %u\n",
                                          last - first, 2 * hops + 3);
                    }
                    printf("ARCHE3D_NATIVE_MASKED_LATENCY x_mask=%u y_mask=%u op=%u hops=%u "
                           "capture_to_receive=%u\n",
                           j.x_mask, j.y_mask, j.op, hops, last - first);
                }
            }
            if ((self->phase == 23 || self->phase == 83 || self->phase == 84))
            {
                for (unsigned node = 0; node < self->nx * self->ny; ++node)
                {
                    for (unsigned b = 0; b < 128; ++b)
                    {
                        if (self->data[node][0x6000 + b] !=
                            uint8_t((node + self->nx * self->ny - 1) % (self->nx * self->ny)))
                        {
                            self->trace.fatal("Contending unicast data mismatch\n");
                        }
                    }
                }
            }
            if (++self->phase == 85)
            {
                printf("ARCHE3D_NATIVE_COLLECTIVE_RESULT PASS nx=%u ny=%u phases=85\n", self->nx,
                       self->ny);
                self->time.get_engine()->quit(0);
                return;
            }
            self->begin();
        }
        self->event.enqueue();
    }

  public:
    explicit CollectiveProbe(vp::ComponentConf &config) : vp::Component(config), event(this, tick)
    {
        nx = get_js_config()->get_uint("nx");
        ny = get_js_config()->get_uint("ny");
        l1_base = get_js_config()->get_uint("l1_base");
        stream_perf = get_js_config()->get("stream_perf")->get_bool();
        traces.new_trace("trace", &trace, vp::DEBUG);
        control.resize(nx * ny);
        out.resize(nx * ny);
        memory.resize(nx * ny);
        data.resize(nx * ny);
        for (unsigned node = 0; node < nx * ny; ++node)
        {
            new_master_port("control_" + std::to_string(node), &control[node]);
            out[node].set_resp_meth(response);
            out[node].set_grant_meth(grant);
            new_master_port("out_" + std::to_string(node), &out[node]);
            memory[node].set_req_meth_muxed(access, node);
            new_slave_port("mem_" + std::to_string(node), &memory[node]);
            data[node].resize(0x10000, 0xa5);
        }
    }
    ~CollectiveProbe()
    {
        for (auto *req : writes)
        {
            delete req;
        }
    }
    void reset(bool active) override
    {
        if (!active)
        {
            event.enqueue();
        }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new CollectiveProbe(config); }
