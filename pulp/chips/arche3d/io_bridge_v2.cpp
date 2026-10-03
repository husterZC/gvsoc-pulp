// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/itf/wire.hpp>
#include <deque>
#include "io_bridge.hpp"

class IoBridgeV2 : public vp::Component {
    vp::IoMaster output;
    vp::WireSlave<Arche3dIoAccess *> request;
    vp::WireMaster<Arche3dIoAccess *> done;
    std::deque<vp::IoReq *> denied;
    static void input(vp::Block *block, Arche3dIoAccess *a) {
        auto self = static_cast<IoBridgeV2 *>(block);
        auto req = vp::IoReqAllocator::get(0)->alloc();
        req->prepare(); req->set_addr(a->address); req->set_size(a->size);
        req->set_data(a->data); req->set_is_write(false);
        req->initiator = a; req->parent = nullptr; req->burst_id = 0;
        req->is_first = req->is_last = true;
        req->set_second_data(nullptr); req->memcheck_data = nullptr;
        req->second_memcheck_data = nullptr; req->memcheck_data_id = 0;
        auto status = self->output.req(req);
        a->pending = status != vp::IO_REQ_DONE;
        if (status == vp::IO_REQ_DENIED) self->denied.push_back(req);
        if (status == vp::IO_REQ_DONE) {
            a->error = req->get_resp_status() != vp::IO_RESP_OK;
            a->latency = req->get_full_latency(); req->free();
        }
    }
    static vp::IoRespAck response(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<IoBridgeV2 *>(block);
        auto a = static_cast<Arche3dIoAccess *>(req->initiator);
        a->error = req->get_resp_status() != vp::IO_RESP_OK;
        a->latency = req->get_full_latency(); req->free();
        self->done.sync(a);
        return vp::IO_RESP_ACCEPTED;
    }
    static void retry(vp::Block *block, vp::IoRetryChannel) {
        auto self = static_cast<IoBridgeV2 *>(block);
        while (!self->denied.empty()) {
            auto req = self->denied.front();
            auto status = self->output.req(req);
            if (status == vp::IO_REQ_DENIED) break;
            self->denied.pop_front();
            if (status == vp::IO_REQ_DONE) response(self, req);
        }
    }
public:
    explicit IoBridgeV2(vp::ComponentConf &config)
        : vp::Component(config), output(retry, response) {
        request.set_sync_meth(input);
        new_slave_port("request", &request); new_master_port("done", &done);
        new_master_port("output", &output);
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new IoBridgeV2(config); }
