// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <unordered_map>
#include "matrix_bridge.hpp"

class MatrixBridge : public vp::Component {
    uint64_t l1_base;
    vp::IoSlave input;
    vp::IoMaster memory;
    vp::WireMaster<MatrixAccess *> config_request, memory_done;
    vp::WireSlave<MatrixAccess *> config_done, memory_request;
    std::unordered_map<vp::IoReq *, MatrixAccess *> outstanding;
    static vp::IoReqStatus config(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<MatrixBridge *>(block);
        auto a = new MatrixAccess{req->get_addr(), req->get_size(), req->get_data(),
                                   req->get_is_write(), req};
        self->config_request.sync(a);
        if (a->pending) return vp::IO_REQ_PENDING;
        req->inc_latency(a->latency);
        auto status = a->error ? vp::IO_REQ_INVALID : vp::IO_REQ_OK;
        delete a;
        return status;
    }
    static void config_complete(vp::Block *, MatrixAccess *a) {
        auto req = static_cast<vp::IoReq *>(a->original);
        req->status = a->error ? vp::IO_REQ_INVALID : vp::IO_REQ_OK;
        req->inc_latency(a->latency);
        delete a;
        req->get_resp_port()->resp(req);
    }
    static void access(vp::Block *block, MatrixAccess *a) {
        auto self = static_cast<MatrixBridge *>(block);
        auto req = new vp::IoReq;
        req->init(); req->set_addr(a->addr - self->l1_base); req->set_size(a->bytes);
        req->set_data(a->data); req->set_is_write(a->write);
        auto status = self->memory.req(req);
        a->pending = status == vp::IO_REQ_PENDING || status == vp::IO_REQ_DENIED;
        if (a->pending) self->outstanding[req] = a;
        else {
            a->error = status != vp::IO_REQ_OK;
            a->latency = req->get_full_latency();
            delete req;
        }
    }
    static void response(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<MatrixBridge *>(block);
        auto a = self->outstanding.at(req);
        self->outstanding.erase(req);
        a->error = req->status != vp::IO_REQ_OK;
        a->latency = req->get_latency();
        delete req;
        self->memory_done.sync(a);
    }
    static void grant(vp::Block *, vp::IoReq *) {} // v1 retains ownership
public:
    explicit MatrixBridge(vp::ComponentConf &config) : vp::Component(config) {
        l1_base = get_js_config()->get_uint("l1_base");
        input.set_req_meth(MatrixBridge::config);
        config_done.set_sync_meth(config_complete);
        memory_request.set_sync_meth(access);
        memory.set_resp_meth(response); memory.set_grant_meth(grant);
        new_slave_port("input", &input); new_master_port("memory", &memory);
        new_master_port("config_request", &config_request);
        new_slave_port("config_done", &config_done);
        new_slave_port("memory_request", &memory_request);
        new_master_port("memory_done", &memory_done);
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new MatrixBridge(config); }
