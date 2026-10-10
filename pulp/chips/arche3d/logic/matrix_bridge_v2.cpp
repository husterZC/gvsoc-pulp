// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/itf/wire.hpp>
#include <deque>
#include "matrix_bridge.hpp"

class MatrixBridgeV2 : public vp::Component {
    vp::IoMaster config;
    vp::IoSlave memory;
    vp::WireSlave<MatrixAccess *> config_request, memory_done;
    vp::WireMaster<MatrixAccess *> config_done, memory_request;
    std::deque<vp::IoReq *> denied;
    static void access(vp::Block *block, MatrixAccess *a) {
        auto self = static_cast<MatrixBridgeV2 *>(block);
        auto req = vp::IoReqAllocator::get(0)->alloc();
        req->prepare(); req->set_addr(a->addr); req->set_size(a->bytes);
        req->set_data(a->data); req->set_is_write(a->write);
        req->initiator = a; req->parent = nullptr; req->burst_id = 0;
        req->is_first = req->is_last = true;
        req->set_second_data(nullptr); req->memcheck_data = nullptr;
        req->second_memcheck_data = nullptr; req->memcheck_data_id = 0;
        auto status = self->config.req(req);
        a->pending = status != vp::IO_REQ_DONE;
        if (status == vp::IO_REQ_DENIED) self->denied.push_back(req);
        if (status == vp::IO_REQ_DONE) {
            a->error = req->get_resp_status() != vp::IO_RESP_OK;
            a->latency = req->get_full_latency(); req->free();
        }
    }
    static vp::IoRespAck response(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<MatrixBridgeV2 *>(block);
        auto a = static_cast<MatrixAccess *>(req->initiator);
        a->error = req->get_resp_status() != vp::IO_RESP_OK;
        a->latency = req->get_full_latency(); req->free();
        self->config_done.sync(a);
        return vp::IO_RESP_ACCEPTED;
    }
    static void retry(vp::Block *block, vp::IoRetryChannel) {
        auto self = static_cast<MatrixBridgeV2 *>(block);
        while (!self->denied.empty()) {
            auto req = self->denied.front();
            auto status = self->config.req(req);
            if (status == vp::IO_REQ_DENIED) break;
            self->denied.pop_front();
            if (status == vp::IO_REQ_DONE) response(self, req);
        }
    }
    static vp::IoReqStatus memory_access(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<MatrixBridgeV2 *>(block);
        auto a = new MatrixAccess{req->get_addr(), req->get_size(), req->get_data(),
                                   req->get_is_write(), req};
        self->memory_request.sync(a);
        if (a->pending) return vp::IO_REQ_GRANTED;
        req->set_resp_status(a->error ? vp::IO_RESP_INVALID : vp::IO_RESP_OK);
        req->inc_latency(a->latency);
        delete a;
        return vp::IO_REQ_DONE;
    }
    static void memory_complete(vp::Block *block, MatrixAccess *a) {
        auto self = static_cast<MatrixBridgeV2 *>(block);
        auto req = static_cast<vp::IoReq *>(a->original);
        req->set_resp_status(a->error ? vp::IO_RESP_INVALID : vp::IO_RESP_OK);
        req->inc_latency(a->latency);
        delete a;
        // MXCore always accepts its single response; no split/beat conversion.
        self->memory.resp(req);
    }
public:
    explicit MatrixBridgeV2(vp::ComponentConf &conf)
        : vp::Component(conf), config(retry, response), memory(memory_access) {
        config_request.set_sync_meth(access); memory_done.set_sync_meth(memory_complete);
        new_slave_port("config_request", &config_request); new_master_port("config_done", &config_done);
        new_master_port("memory_request", &memory_request); new_slave_port("memory_done", &memory_done);
        new_master_port("config", &config); new_slave_port("memory", &memory);
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new MatrixBridgeV2(config); }
