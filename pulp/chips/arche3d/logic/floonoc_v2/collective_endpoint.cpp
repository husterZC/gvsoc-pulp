// SPDX-License-Identifier: Apache-2.0
// Local, two-sided streaming collective DMA. No remote memory reads or ACK tree.
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <array>
#include <algorithm>
#include <cstring>
#include "collective_types.hpp"

class CollectiveEndpoint : public vp::Component
{
    static constexpr unsigned SLOT_BYTES = 64;
    static constexpr unsigned MAX_BYTES_OFFSET = 0x800, BEAT_BYTES_OFFSET = 0x804;
    enum Register : unsigned
    {
        REG_CONFIG = 0x00,
        REG_EPOCH = 0x04,
        REG_SOURCE = 0x08,
        REG_DESTINATION = 0x0c,
        REG_BYTES = 0x10,
        REG_COMMAND = 0x14,
        REG_STATUS = 0x18,
        REG_MATCH_MASKS = 0x1c,
        REG_SEND_CYCLE = 0x20,
        REG_RECEIVE_CYCLE = 0x24,
        REG_RECEIVE_DONE_CYCLE = 0x28,
        REG_ROOT_LINE = 0x2c,
        REG_FIRST_SEND_CYCLE = 0x30,
        REG_FIRST_RECEIVE_CYCLE = 0x34,
    };
    enum
    {
        POST = 1,
        SEND = 2,
        TX_BUSY = 1,
        RX_BUSY = 2,
        TX_DONE = 4,
        RX_DONE = 8
    };
    struct Slot
    {
        uint32_t config = 0, epoch = 0, source = 0, destination = 0, bytes = 0;
        uint32_t match_masks = 0, root_line = 0;
        uint32_t status = 0, tx_cycle = 0, rx_cycle = 0, rx_done_cycle = 0;
        uint32_t tx_first_cycle = 0, rx_first_cycle = 0;
        unsigned tx_issued = 0, tx_sent = 0, rx_received = 0, rx_written = 0;
    };
    // Requests and their buffers remain stable until the annotated latency or
    // asynchronous response has elapsed. Neither window scales with message size.
    struct Beat
    {
        enum State
        {
            FREE,
            RECEIVED,
            MEMORY
        } state = FREE;
        vp::IoReq req;
        Arche3dCollectivePacket packet;
        int64_t due = -1;
        void release()
        {
            state = FREE;
            due = -1;
            packet.data.clear();
        }
    };
    std::array<Slot, arche3d_collective::SLOTS> slots{};
    std::array<Beat, arche3d_collective::PIPELINE_BEATS> reads{}, writes{};
    vp::IoSlave input_itf;
    vp::IoMaster memory_itf;
    vp::WireMaster<Arche3dCollectiveOffer *> send_itf;
    vp::WireSlave<Arche3dCollectiveOffer *> receive_itf;
    vp::WireMaster<bool> ready_itf;
    vp::ClockEvent event;
    vp::Trace trace;
    unsigned x, y, nx, ny, width, l1_size, l1_base;
    unsigned next_read = 0, next_write = 0, next_send = 0;

    Arche3dCollectivePacket descriptor(const Slot &s) const
    {
        Arche3dCollectivePacket p;
        p.type = s.config & 255;
        p.column = s.config & 256;
        p.root = s.config >> 16;
        p.epoch = s.epoch;
        p.total_bytes = s.bytes;
        bool masked = s.config & 512;
        p.line = masked ? s.root_line : (p.column ? x : y);
        p.x_mask = masked ? s.match_masks & 0xffff : (p.column ? 0xffff : 0);
        p.y_mask = masked ? s.match_masks >> 16 : (p.column ? 0 : 0xffff);
        return p;
    }
    void validate(const Slot &s, bool sending)
    {
        auto p = descriptor(s);
        unsigned type = p.type;
        unsigned elem = arche3d_collective::element_bytes(type);
        uint64_t addr = sending ? s.source : s.destination;
        if (!arche3d_collective::valid(type) || (s.config & 0xfc00) || p.root_x() >= nx ||
            p.root_y() >= ny || ((s.config & 512) && s.root_line >= (p.column ? nx : ny)) ||
            !p.selects(x, y) || !s.bytes || s.bytes > l1_size || addr < l1_base ||
            addr + s.bytes > uint64_t(l1_base) + l1_size ||
            (type != arche3d_collective::BROADCAST &&
             (width % elem || addr % elem || s.bytes % elem)))
        {
            trace.fatal("Invalid collective descriptor (config=0x%x bytes=%u addr=0x%lx)\n",
                        s.config, s.bytes, addr);
        }
        bool is_root = p.root_x() == x && p.root_y() == y;
        if (type == arche3d_collective::BROADCAST ? (sending && !is_root) : (!sending && !is_root))
        {
            trace.fatal("Invalid collective endpoint role\n");
        }
    }
    static vp::IoReqStatus input(vp::Block *block, vp::IoReq *req)
    {
        auto *self = static_cast<CollectiveEndpoint *>(block);
        unsigned addr = req->get_addr(), value = 0;
        if (req->get_size() != 4 || addr % 4)
        {
            return vp::IO_REQ_INVALID;
        }
        if ((addr == MAX_BYTES_OFFSET || addr == BEAT_BYTES_OFFSET) && !req->get_is_write())
        {
            value = addr == MAX_BYTES_OFFSET ? self->l1_size : self->width;
            std::memcpy(req->get_data(), &value, 4);
            return vp::IO_REQ_OK;
        }
        if (addr >= arche3d_collective::SLOTS * SLOT_BYTES)
        {
            return vp::IO_REQ_INVALID;
        }
        unsigned index = addr / SLOT_BYTES, reg = addr % SLOT_BYTES;
        auto &s = self->slots[index];
        if (!req->get_is_write())
        {
            switch (reg)
            {
            case REG_CONFIG:
                value = s.config;
                break;
            case REG_EPOCH:
                value = s.epoch;
                break;
            case REG_SOURCE:
                value = s.source;
                break;
            case REG_DESTINATION:
                value = s.destination;
                break;
            case REG_BYTES:
                value = s.bytes;
                break;
            case REG_STATUS:
                value = s.status;
                break;
            case REG_MATCH_MASKS:
                value = s.match_masks;
                break;
            case REG_SEND_CYCLE:
                value = s.tx_cycle;
                break;
            case REG_RECEIVE_CYCLE:
                value = s.rx_cycle;
                break;
            case REG_RECEIVE_DONE_CYCLE:
                value = s.rx_done_cycle;
                break;
            case REG_ROOT_LINE:
                value = s.root_line;
                break;
            case REG_FIRST_SEND_CYCLE:
                value = s.tx_first_cycle;
                break;
            case REG_FIRST_RECEIVE_CYCLE:
                value = s.rx_first_cycle;
                break;
            default:
                return vp::IO_REQ_INVALID;
            }
            std::memcpy(req->get_data(), &value, 4);
            return vp::IO_REQ_OK;
        }
        std::memcpy(&value, req->get_data(), 4);
        if (reg != REG_COMMAND && (s.status & (TX_BUSY | RX_BUSY)))
        {
            self->trace.fatal("Reprogramming a busy collective slot\n");
        }
        switch (reg)
        {
        case REG_CONFIG:
            s.config = value;
            s.status = 0;
            s.tx_cycle = s.rx_cycle = s.rx_done_cycle = 0;
            s.tx_first_cycle = s.rx_first_cycle = 0;
            break;
        case REG_EPOCH:
            s.epoch = value;
            break;
        case REG_SOURCE:
            s.source = value;
            break;
        case REG_DESTINATION:
            s.destination = value;
            break;
        case REG_BYTES:
            s.bytes = value;
            break;
        case REG_MATCH_MASKS:
            s.match_masks = value;
            break;
        case REG_ROOT_LINE:
            s.root_line = value;
            break;
        case REG_COMMAND:
            if (!value || (value & ~(POST | SEND)))
            {
                return vp::IO_REQ_INVALID;
            }
            if (value & POST)
            {
                self->validate(s, false);
                if (s.status & RX_BUSY)
                {
                    self->trace.fatal("Collective receive already posted\n");
                }
                s.status = (s.status & ~RX_DONE) | RX_BUSY;
                s.rx_received = s.rx_written = 0;
                s.rx_cycle = s.rx_done_cycle = s.rx_first_cycle = 0;
                self->ready_itf.sync(true);
            }
            if (value & SEND)
            {
                self->validate(s, true);
                if (s.status & TX_BUSY)
                {
                    self->trace.fatal("Collective send already active\n");
                }
                s.status = (s.status & ~TX_DONE) | TX_BUSY;
                s.tx_issued = s.tx_sent = 0;
                s.tx_cycle = s.tx_first_cycle = 0;
            }
            self->event.enqueue();
            break;
        default:
            return vp::IO_REQ_INVALID;
        }
        return vp::IO_REQ_OK;
    }
    static void receive(vp::Block *block, Arche3dCollectiveOffer *offer)
    {
        auto *self = static_cast<CollectiveEndpoint *>(block);
        auto &p = *offer->packet;
        offer->accepted = false;
        if (p.slot >= arche3d_collective::SLOTS)
        {
            self->trace.fatal("Invalid received collective slot\n");
        }
        auto &s = self->slots[p.slot];
        if (!(s.status & RX_BUSY) || s.epoch != p.epoch)
        {
            return;
        }
        auto expected = self->descriptor(s);
        if (expected.type != p.type || expected.column != p.column || expected.root != p.root ||
            expected.line != p.line || expected.x_mask != p.x_mask || expected.y_mask != p.y_mask ||
            s.bytes != p.total_bytes || p.offset >= s.bytes || p.offset % self->width ||
            p.data.size() != std::min(self->width, s.bytes - p.offset) ||
            !p.selects(self->x, self->y))
        {
            self->trace.fatal("Posted collective receive does not match incoming beat\n");
        }
        if (p.offset < s.rx_received)
        {
            self->trace.fatal("Duplicate collective beat\n");
        }
        if (p.offset != s.rx_received)
        {
            return;
        }
        for (auto &b : self->writes)
        {
            if (b.state == Beat::FREE)
            {
                b.packet = p;
                b.state = Beat::RECEIVED;
                s.rx_cycle = self->clock.get_cycles();
                if (!s.rx_received)
                {
                    s.rx_first_cycle = s.rx_cycle;
                }
                s.rx_received += p.data.size();
                offer->accepted = true;
                self->event.enqueue();
                return;
            }
        }
    }
    void memory_finished(Beat &b)
    {
        if (b.req.status == vp::IO_REQ_INVALID)
        {
            trace.fatal("Collective local memory access failed\n");
        }
        b.due = clock.get_cycles() + std::max<uint64_t>(1, b.req.get_full_latency());
        event.enqueue(std::max<int64_t>(1, b.due - clock.get_cycles()));
    }
    void issue(Beat &b, bool write)
    {
        auto &s = slots[b.packet.slot];
        b.state = Beat::MEMORY;
        b.due = -1;
        b.req.init();
        b.req.set_addr((write ? s.destination : s.source) - l1_base + b.packet.offset);
        b.req.set_size(b.packet.data.size());
        b.req.set_is_write(write);
        b.req.set_data(b.packet.data.data());
        auto status = memory_itf.req(&b.req);
        if (status == vp::IO_REQ_OK || status == vp::IO_REQ_INVALID)
        {
            b.req.status = status;
            memory_finished(b);
        }
        // PENDING/DENIED retain the request until response(); never resubmit on grant.
    }
    static void response(vp::Block *block, vp::IoReq *req)
    {
        auto *self = static_cast<CollectiveEndpoint *>(block);
        for (auto *window : {&self->reads, &self->writes})
        {
            for (auto &b : *window)
            {
                if (&b.req == req && b.state == Beat::MEMORY && b.due < 0)
                {
                    self->memory_finished(b);
                    return;
                }
            }
        }
        self->trace.fatal("Unknown collective memory response\n");
    }
    static void grant(vp::Block *, vp::IoReq *)
    {
    }
    void retire_writes(int64_t now)
    {
        bool freed = false;
        for (auto &b : writes)
        {
            if (b.state == Beat::MEMORY && b.due >= 0 && b.due <= now)
            {
                auto &s = slots[b.packet.slot];
                s.rx_written += b.packet.data.size();
                if (s.rx_written == s.bytes)
                {
                    s.status = (s.status & ~RX_BUSY) | RX_DONE;
                    s.rx_done_cycle = now;
                    trace.msg(vp::Trace::LEVEL_DEBUG,
                              "COL_RECEIVE_DONE at=(%u,%u) slot=%u epoch=%u cycle=%ld\n", x, y,
                              b.packet.slot, s.epoch, now);
                }
                b.release();
                freed = true;
            }
        }
        if (freed)
        {
            ready_itf.sync(true);
        }
    }
    void send_ready_beat(int64_t now)
    {
        // Fair across descriptors, ordered within each stream even if L1
        // responses complete out of order. At most one NI capture per cycle.
        for (unsigned i = 0; i < slots.size(); ++i)
        {
            unsigned index = (next_send + i) % slots.size();
            auto &s = slots[index];
            for (auto &b : reads)
            {
                if (b.state == Beat::MEMORY && b.due >= 0 && b.due <= now &&
                    b.packet.slot == index && b.packet.offset == s.tx_sent)
                {
                    Arche3dCollectiveOffer offer{&b.packet};
                    send_itf.sync(&offer);
                    if (offer.accepted)
                    {
                        if (!s.tx_sent)
                        {
                            s.tx_first_cycle = now;
                        }
                        s.tx_sent += b.packet.data.size();
                        if (s.tx_sent == s.bytes)
                        {
                            s.status = (s.status & ~TX_BUSY) | TX_DONE;
                            s.tx_cycle = now;
                            trace.msg(vp::Trace::LEVEL_DEBUG,
                                      "COL_SEND_DONE at=(%u,%u) slot=%u epoch=%u cycle=%ld\n", x, y,
                                      index, s.epoch, now);
                        }
                        b.release();
                        next_send = (index + 1) % slots.size();
                    }
                    return; // At most one NI offer per cycle, including backpressure.
                }
            }
        }
    }
    void issue_read()
    {
        // Independent pipelined read/write issue, sharing the real L1 target
        // and its bank arbitration with ordinary DMA, cores and NoC traffic.
        for (auto &b : reads)
        {
            if (b.state == Beat::FREE)
            {
                for (unsigned i = 0; i < slots.size(); ++i)
                {
                    unsigned index = (next_read + i) % slots.size();
                    auto &s = slots[index];
                    if (!(s.status & TX_BUSY) || s.tx_issued == s.bytes)
                    {
                        continue;
                    }
                    b.packet = descriptor(s);
                    b.packet.slot = index;
                    b.packet.offset = s.tx_issued;
                    b.packet.data.resize(std::min(width, s.bytes - s.tx_issued));
                    s.tx_issued += b.packet.data.size();
                    issue(b, false);
                    next_read = (index + 1) % slots.size();
                    break;
                }
                break;
            }
        }
    }
    void issue_write()
    {
        for (unsigned i = 0; i < writes.size(); ++i)
        {
            unsigned index = (next_write + i) % writes.size();
            auto &b = writes[index];
            if (b.state != Beat::RECEIVED)
            {
                continue;
            }
            issue(b, true);
            next_write = (index + 1) % writes.size();
            break;
        }
    }
    void schedule(int64_t now)
    {
        bool free_read = false, pending = false;
        for (auto &b : reads)
        {
            free_read |= b.state == Beat::FREE;
        }
        for (auto &s : slots)
        {
            pending |= free_read && (s.status & TX_BUSY) && s.tx_issued < s.bytes;
        }
        for (auto *window : {&reads, &writes})
        {
            for (auto &b : *window)
            {
                pending |= b.state == Beat::RECEIVED ||
                           (b.state == Beat::MEMORY && b.due >= 0 && b.due <= now);
                if (b.state == Beat::MEMORY && b.due > now)
                {
                    event.enqueue(b.due - now);
                }
            }
        }
        if (pending)
        {
            event.enqueue();
        }
    }
    static void tick(vp::Block *block, vp::ClockEvent *)
    {
        auto *self = static_cast<CollectiveEndpoint *>(block);
        int64_t now = self->clock.get_cycles();
        self->retire_writes(now);
        self->send_ready_beat(now);
        self->issue_read();
        self->issue_write();
        self->schedule(now);
    }

  public:
    explicit CollectiveEndpoint(vp::ComponentConf &config)
        : vp::Component(config), event(this, tick)
    {
        auto cfg = get_js_config();
        x = cfg->get_uint("x");
        y = cfg->get_uint("y");
        nx = cfg->get_uint("nx");
        ny = cfg->get_uint("ny");
        width = cfg->get_uint("width");
        l1_size = cfg->get_uint("l1_size");
        l1_base = cfg->get_uint("l1_base");
        traces.new_trace("trace", &trace, vp::DEBUG);
        input_itf.set_req_meth(input);
        new_slave_port("input", &input_itf);
        memory_itf.set_resp_meth(response);
        memory_itf.set_grant_meth(grant);
        new_master_port("memory", &memory_itf);
        new_master_port("send", &send_itf);
        receive_itf.set_sync_meth(receive);
        new_slave_port("receive", &receive_itf);
        new_master_port("ready", &ready_itf);
    }
    void reset(bool active) override
    {
        if (active)
        {
            for (auto &s : slots)
            {
                s = {};
            }
            for (auto *window : {&reads, &writes})
            {
                for (auto &b : *window)
                {
                    b.release();
                }
            }
            next_read = next_write = next_send = 0;
        }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new CollectiveEndpoint(config);
}
