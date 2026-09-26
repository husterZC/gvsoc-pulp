// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/itf/wire.hpp>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include "dma_access.hpp"

class I3dDmaAxi : public vp::Component {
    struct Transaction {
        Arche3dAccess *access;
        vp::IoReq *request;
        uint64_t received = 0;
        int64_t ready = 0;
    };
    vp::Trace trace;
    vp::IoMaster axi;
    vp::WireSlave<Arche3dAccess *> request;
    vp::WireMaster<Arche3dAccess *> done;
    vp::WireMaster<Arche3dDmaEvent> activity;
    vp::ClockEvent event;
    std::deque<Transaction *> queued, finished;
    std::map<uint32_t, Transaction *> live;
    bool denied = false;
    unsigned capacity, id_count;
    bool check_pattern;
    uint64_t memory_base, interleave, terminals;
    void notify(Transaction *txn, bool completed) {
        if (activity.is_bound()) {
            auto a = txn->access;
            activity.sync({a->address, a->size, a->id, a->write, completed});
        }
    }
    void send() {
        if (queued.empty()) return;
        auto txn = queued.front();
        auto status = axi.req(txn->request);
        denied = status == vp::IO_REQ_DENIED;
        if (!denied) {
            queued.pop_front();
            notify(txn, false);
            if (status == vp::IO_REQ_DONE) complete(txn, txn->request);
            if (!queued.empty()) event.enqueue();
        }
    }
    void complete(Transaction *txn, vp::IoReq *response) {
        txn->access->error |= response->get_resp_status() != vp::IO_RESP_OK;
        txn->ready = std::max<int64_t>(txn->ready,
            clock.get_cycles() + response->get_full_latency());
        if (!txn->access->write && !txn->access->error) {
            if (!response->get_data() || txn->received + response->get_size() > txn->access->size)
                trace.fatal("Invalid I3D DMA read response size or data\n");
            auto dest = txn->access->data + txn->received;
            if (dest != response->get_data()) std::memcpy(dest, response->get_data(), response->get_size());
            txn->received += response->get_size();
        }
        bool last = response->is_last;
        if (response != txn->request) response->free();
        if (last) {
            if (!txn->access->write && !txn->access->error && txn->received != txn->access->size)
                trace.fatal("Incomplete I3D DMA read response\n");
            if (check_pattern && !txn->access->write && !txn->access->error) {
                uint64_t relative = txn->access->address - memory_base;
                uint64_t destination = (relative / interleave) % terminals;
                uint64_t local = relative / (interleave * terminals) * interleave + relative % interleave;
                for (uint64_t i = 0; i < txn->access->size; ++i) {
                    uint64_t a = local + i;
                    if (txn->access->data[i] != uint8_t(17 * destination + 13 * a + (a >> 8)))
                        trace.fatal("I3D DMA benchmark data mismatch at 0x%llx\n", txn->access->address + i);
                }
            }
            notify(txn, true);
            finished.push_back(txn);
            event.enqueue(std::max<int64_t>(1, txn->ready - clock.get_cycles()));
        }
    }
    static void input(vp::Block *block, Arche3dAccess *access) {
        auto self = static_cast<I3dDmaAxi *>(block);
        if (self->live.size() >= self->capacity || access->id >= self->id_count || self->live.count(access->id))
            self->trace.fatal("I3D DMA capacity exceeded or AXI ID reused while live\n");
        auto req = vp::IoReqAllocator::get(0)->alloc();
        req->prepare(); req->set_addr(access->address); req->set_size(access->size);
        req->set_data(access->data); req->set_is_write(access->write);
        req->set_second_data(nullptr); req->parent = nullptr;
        req->memcheck_data = nullptr; req->second_memcheck_data = nullptr;
        req->memcheck_data_id = 0;
        req->is_first = req->is_last = true; req->burst_id = access->id;
        auto txn = new Transaction{access, req}; req->initiator = txn;
        self->live.emplace(access->id, txn); self->queued.push_back(txn);
        self->event.enqueue();
    }
    static void retry(vp::Block *block, vp::IoRetryChannel) {
        auto self = static_cast<I3dDmaAxi *>(block);
        if (self->denied) { self->denied = false; self->send(); }
    }
    static vp::IoRespAck response(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<I3dDmaAxi *>(block);
        auto txn = static_cast<Transaction *>(req->initiator);
        auto it = self->live.find(req->burst_id);
        if (it == self->live.end() || it->second != txn)
            self->trace.fatal("I3D DMA response ID or identity mismatch\n");
        self->complete(txn, req);
        return vp::IO_RESP_ACCEPTED;
    }
    static void tick(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<I3dDmaAxi *>(block);
        for (auto it = self->finished.begin(); it != self->finished.end();) {
            auto txn = *it;
            if (txn->ready > self->clock.get_cycles()) { ++it; continue; }
            it = self->finished.erase(it);
            self->live.erase(txn->access->id);
            txn->request->free();
            self->done.sync(txn->access);
            delete txn;
        }
        if (!self->denied) self->send();
        if (!self->finished.empty()) {
            int64_t ready = self->finished.front()->ready;
            for (auto txn : self->finished) ready = std::min(ready, txn->ready);
            self->event.enqueue(std::max<int64_t>(1, ready - self->clock.get_cycles()));
        }
    }
public:
    explicit I3dDmaAxi(vp::ComponentConf &config)
        : vp::Component(config), axi(retry, response), event(this, tick) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        capacity = get_js_config()->get_uint("capacity");
        id_count = 1u << get_js_config()->get_uint("id_width");
        check_pattern = get_js_config()->get("check_pattern")->get_bool();
        memory_base = get_js_config()->get_uint("memory_base");
        interleave = get_js_config()->get_uint("interleave");
        terminals = get_js_config()->get_uint("terminals");
        request.set_sync_meth(input);
        new_slave_port("request", &request); new_master_port("done", &done);
        new_master_port("axi", &axi); new_master_port("activity", &activity);
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new I3dDmaAxi(config); }
