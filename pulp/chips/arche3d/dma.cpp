// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <algorithm>
#include <deque>
#include <memory>
#include <cstring>
#include "dma_access.hpp"
#include <pulp/chips/arche3d/logic/idma/fe/idma_fe_xdma.hpp>
#include <pulp/chips/arche3d/logic/idma/me/idma_me_2d.hpp>

class I3dDma : public vp::Component, public IdmaTransferConsumer {
    struct Row {
        IdmaTransfer *transfer;
        uint64_t offset = 0;
        unsigned pending = 0;
        bool submitted = false;
    };
    struct Burst {
        Row *row;
        Arche3dAccess access;
        uint64_t local, offset = 0;
        std::vector<uint8_t> data;
    };
    IDmaFeXdma fe;
    IDmaMe2D me;
    vp::Trace trace;
    vp::ClockEvent event;
    vp::IoMaster tcdm;
    vp::IoReq local_req;
    vp::WireMaster<Arche3dAccess *> request;
    vp::WireSlave<Arche3dAccess *> done;
    std::deque<Row *> rows;
    std::deque<Burst *> local;
    std::deque<unsigned> ids;
    uint64_t loc_base, loc_size, memory_base, memory_size, interleave, axi_bytes, max_burst;
    unsigned capacity, row_capacity, tcdm_width;
    bool local_pending = false;
    int64_t local_ready = -1;

    static bool inside(uint64_t addr, uint64_t size, uint64_t base, uint64_t length) {
        return addr >= base && size <= length && addr - base <= length - size;
    }
    void finish(Burst *burst) {
        Row *row = burst->row;
        ids.push_back(burst->access.id);
        --row->pending;
        delete burst;
        if (row->submitted && row->pending == 0) {
            me.ack_transfer(row->transfer);
            delete row;
        }
        me.update();
        event.enqueue();
    }
    static void completed(vp::Block *block, Arche3dAccess *access) {
        auto self = static_cast<I3dDma *>(block);
        auto burst = static_cast<Burst *>(access->owner);
        if (access->error)
            self->trace.fatal("I3D DMA request failed at 0x%llx (ID %u)\n", access->address, access->id);
        if (access->write) self->finish(burst);
        else { self->local.push_back(burst); self->event.enqueue(); }
    }
    static void local_response(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<I3dDma *>(block);
        if (req->status == vp::IO_REQ_INVALID)
            self->trace.fatal("I3D DMA TCDM request failed\n");
        self->local_ready = self->clock.get_cycles() + std::max<uint64_t>(1, req->get_full_latency());
        self->event.enqueue(std::max<uint64_t>(1, req->get_full_latency()));
    }
    static void local_grant(vp::Block *, vp::IoReq *) {}
    static void tick(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<I3dDma *>(block);
        const int64_t now = self->clock.get_cycles();
        if (self->local_pending && self->local_ready >= 0 && self->local_ready <= now) {
            Burst *burst = self->local.front();
            burst->offset += self->local_req.get_size();
            self->local_pending = false;
            self->local_ready = -1;
            if (burst->offset == burst->access.size) {
                self->local.pop_front();
                if (burst->access.write) self->request.sync(&burst->access);
                else self->finish(burst);
            }
        }
        if (!self->local_pending && !self->local.empty()) {
            Burst *burst = self->local.front();
            auto &req = self->local_req;
            req.prepare();
            req.status = vp::IO_REQ_OK;
            req.set_addr(burst->local + burst->offset);
            req.set_size(std::min<uint64_t>(burst->access.size - burst->offset,
                self->tcdm_width - req.get_addr() % self->tcdm_width));
            req.set_data(burst->data.data() + burst->offset);
            req.set_is_write(!burst->access.write);
            self->local_pending = true;
            auto status = self->tcdm.req(&req);
            if (status == vp::IO_REQ_OK) local_response(self, &req);
            else if (status == vp::IO_REQ_INVALID) self->trace.fatal("Invalid I3D DMA L1 access\n");
            // v1 DENIED transfers ownership until the response, just like PENDING.
        }
        if (!self->rows.empty() && !self->ids.empty()) {
            Row *row = self->rows.front();
            auto transfer = row->transfer;
            bool write = inside(transfer->src, transfer->size, self->loc_base, self->loc_size);
            uint64_t addr = (write ? transfer->dst : transfer->src) + row->offset;
            uint64_t length = std::min({transfer->size - row->offset,
                self->max_burst - addr % self->axi_bytes,
                uint64_t(4096) - addr % 4096,
                self->interleave - (addr - self->memory_base) % self->interleave});
            auto burst = new Burst;
            burst->row = row;
            burst->local = (write ? transfer->src : transfer->dst) + row->offset;
            burst->data.resize(length);
            burst->access = {addr, length, burst->data.data(), self->ids.front(), write, false, burst};
            self->ids.pop_front();
            row->offset += length;
            ++row->pending;
            if (row->offset == transfer->size) {
                row->submitted = true;
                self->rows.pop_front();
                self->me.update();
            }
            if (write) self->local.push_back(burst);
            else self->request.sync(&burst->access);
        }
        if ((!self->rows.empty() && !self->ids.empty()) ||
            (!self->local.empty() && !self->local_pending)) self->event.enqueue();
        if (self->local_pending && self->local_ready >= 0)
            self->event.enqueue(std::max<int64_t>(1, self->local_ready - now));
    }
public:
    explicit I3dDma(vp::ComponentConf &config)
        : vp::Component(config), fe(this, &me), me(this, &fe, this), event(this, tick) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        auto cfg = get_js_config();
        loc_base = cfg->get_uint("loc_base"); loc_size = cfg->get_uint("loc_size");
        memory_base = cfg->get_uint("memory_base"); memory_size = cfg->get_uint("memory_size");
        interleave = cfg->get_uint("interleave_bytes"); axi_bytes = cfg->get_uint("axi_bytes");
        max_burst = axi_bytes * cfg->get_uint("max_burst_beats");
        capacity = cfg->get_uint("burst_queue_size"); row_capacity = cfg->get_uint("transfer_queue_size");
        tcdm_width = cfg->get_uint("tcdm_width");
        local_req.init();
        local_req.set_second_data(nullptr);
        std::memset(local_req.get_payload(), 0, IO_REQ_PAYLOAD_SIZE);
        for (unsigned id = 0; id < capacity; ++id) ids.push_back(id);
        tcdm.set_resp_meth(local_response); tcdm.set_grant_meth(local_grant);
        new_master_port("tcdm", &tcdm);
        new_master_port("request", &request);
        done.set_sync_meth(completed); new_slave_port("done", &done);
    }
    bool can_accept_transfer() override { return rows.size() < row_capacity; }
    void enqueue_transfer(IdmaTransfer *transfer) override {
        if (transfer->parent->collective_type)
            trace.fatal("I3D DMA does not support collective transactions\n");
        bool read = inside(transfer->src, transfer->size, memory_base, memory_size)
                 && inside(transfer->dst, transfer->size, loc_base, loc_size);
        bool write = inside(transfer->src, transfer->size, loc_base, loc_size)
                  && inside(transfer->dst, transfer->size, memory_base, memory_size);
        if (!read && !write)
            trace.fatal("I3D DMA requires one L1 and one 3D DRAM range (src 0x%llx, dst 0x%llx, size %llu)\n",
                transfer->src, transfer->dst, transfer->size);
        if (transfer->size == 0) { me.ack_transfer(transfer); return; }
        rows.push_back(new Row{transfer});
        event.enqueue();
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new I3dDma(config); }
