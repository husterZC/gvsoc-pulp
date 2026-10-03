// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include "io_bridge.hpp"

class IoBridge : public vp::Component {
    vp::IoSlave input;
    vp::WireMaster<Arche3dIoAccess *> request;
    vp::WireSlave<Arche3dIoAccess *> done;
    uint64_t base, size;
    static vp::IoReqStatus access(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<IoBridge *>(block);
        uint64_t addr = req->get_addr(), bytes = req->get_size();
        if (!bytes || !req->get_data() || addr < self->base ||
            addr - self->base >= self->size || bytes > self->size - (addr - self->base) ||
            req->get_is_write()) return vp::IO_REQ_INVALID;
        auto a = new Arche3dIoAccess{addr, bytes, req->get_data(), req};
        self->request.sync(a);
        if (a->pending) return vp::IO_REQ_PENDING;
        req->inc_latency(a->latency);
        auto status = a->error ? vp::IO_REQ_INVALID : vp::IO_REQ_OK;
        delete a;
        return status;
    }
    static void complete(vp::Block *, Arche3dIoAccess *a) {
        auto req = static_cast<vp::IoReq *>(a->request);
        req->inc_latency(a->latency);
        req->status = a->error ? vp::IO_REQ_INVALID : vp::IO_REQ_OK;
        delete a;
        req->get_resp_port()->resp(req);
    }
public:
    explicit IoBridge(vp::ComponentConf &config) : vp::Component(config) {
        base = get_js_config()->get_uint("base");
        size = get_js_config()->get_uint("size");
        input.set_req_meth(access); done.set_sync_meth(complete);
        new_slave_port("input", &input); new_slave_port("done", &done);
        new_master_port("request", &request);
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new IoBridge(config); }
