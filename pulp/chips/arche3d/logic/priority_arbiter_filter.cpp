/*
 * Copyright (C) 2024 ETH Zurich and University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Author: Chi Zhang, ETHz <chizhang@iis.ee.ethz.ch>
 * Adapted from gvsoc-pulp c84bf3cdfc5ce3b34d89afca8fe0b8acdabd51bd.
 */
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>

class PriorityArbiterFilter : public vp::Component
{
public:
    explicit PriorityArbiterFilter(vp::ComponentConf &config);
    void reset(bool active) override;
    static vp::IoReqStatus req(vp::Block *block, vp::IoReq *req);

private:
    vp::Trace trace;
    vp::IoMaster output_port;
    vp::IoSlave input_port;
    int bank_width;
    int64_t last_access_cycle = 0;
    int64_t acc_latency = 0;
};

PriorityArbiterFilter::PriorityArbiterFilter(vp::ComponentConf &config)
    : vp::Component(config)
{
    traces.new_trace("trace", &trace, vp::DEBUG);
    bank_width = get_js_config()->get_child_int("bank_width");
    new_master_port("out", &output_port);
    new_slave_port("input", &input_port);
    input_port.set_req_meth(&PriorityArbiterFilter::req);
}

void PriorityArbiterFilter::reset(bool active)
{
    if (active) {
        last_access_cycle = 0;
        acc_latency = 0;
    }
}

vp::IoReqStatus PriorityArbiterFilter::req(vp::Block *block, vp::IoReq *req)
{
    auto self = static_cast<PriorityArbiterFilter *>(block);
    uint64_t size = req->get_size();
    // Arche3D interleavers also emit partial words for unaligned/short DMA
    // and narrow accelerator tails. Each still occupies one bank cycle.
    if (size == 0 || size > static_cast<uint64_t>(self->bank_width) ||
        req->get_addr() % self->bank_width + size > static_cast<uint64_t>(self->bank_width)) {
        self->trace.fatal("Invalid bank fragment: address=0x%llx size=%llu width=%d\n",
            static_cast<unsigned long long>(req->get_addr()),
            static_cast<unsigned long long>(size), self->bank_width);
        return vp::IO_REQ_INVALID;
    }

    // Commit the access to the real bank so its data and core-side timing
    // account for all traffic. Discard that bank's core-induced queue latency
    // on this bulk path, exactly as in the upstream priority refinement.
    vp::IoReq bank_req;
    bank_req.init();
    bank_req.set_addr(req->get_addr());
    bank_req.set_size(size);
    bank_req.set_data(req->get_data());
    bank_req.set_is_write(req->get_is_write());
    vp::IoReqStatus status = self->output_port.req_forward(&bank_req);
    if (status != vp::IO_REQ_OK) return status;

    int64_t current = self->clock.get_cycles();
    if (current < self->last_access_cycle) {
        self->trace.fatal("Bank priority clock moved backwards\n");
        return vp::IO_REQ_INVALID;
    }
    int64_t elapsed = current - self->last_access_cycle;
    self->acc_latency = elapsed < self->acc_latency ? self->acc_latency - elapsed : 0;
    req->inc_latency(self->acc_latency);
    self->last_access_cycle = current;
    ++self->acc_latency;
    // HWPE propagates this bulk-only latency. DMA/bus interleavers already
    // discard it, preserving the existing DMA > HWPE > core timing policy.
    return vp::IO_REQ_OK;
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new PriorityArbiterFilter(config);
}
