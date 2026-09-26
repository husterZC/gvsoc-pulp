/*
 * Copyright (C) 2020 GreenWaves Technologies, SAS, ETH Zurich and
 *                    University of Bologna
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

#include <vector>
#include <deque>
#include <cstring>
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <pulp/chips/arche3d/logic/snitch/snitch_cluster/cluster_periph_regfields.h>
#include <pulp/chips/arche3d/logic/snitch/snitch_cluster/cluster_periph_gvsoc.h>


using namespace std::placeholders;


class ClusterRegisters : public vp::Component
{

public:

    ClusterRegisters(vp::ComponentConf &config);

    void reset(bool active);

    static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);

    inline uint32_t get_nm_n() { return this->regmap.nm_config.format_n_get(); }
    inline uint32_t get_nm_m() { return this->regmap.nm_config.format_m_get(); }

private:
    static void barrier_sync(vp::Block *__this, bool value, int id);
    static vp::IoReqStatus wakeup_send(vp::Block *__this, vp::IoReq *req);
    static vp::IoReqStatus wakeup_recv(vp::Block *__this, vp::IoReq *req);
    static void wakeup_step(vp::Block *__this, vp::ClockEvent *event);
    static void grant(vp::Block *__this, vp::IoReq *req);
    static void response(vp::Block *__this, vp::IoReq *req);
    static void boot_ready_handler(vp::Block *__this, bool value);
    void cl_clint_set_req(uint64_t reg_offset, int size, uint8_t *value, bool is_write);
    void cl_clint_clear_req(uint64_t reg_offset, int size, uint8_t *value, bool is_write);
    void nm_config_req(uint64_t offset, int size, uint8_t *value, bool is_write);

    vp::Trace     trace;

    vp_regmap_cluster_periph regmap;

    vp::IoSlave in;
    uint32_t bootaddr;
    uint32_t status;
    int nb_cores;
    uint32_t cluster_id;
    vp::reg_32 barrier_status;
    uint32_t num_cluster_x;
    uint32_t num_cluster_y;

    std::vector<vp::WireSlave<bool>> barrier_req_itf;
    vp::WireMaster<bool> barrier_ack_itf;

    struct WakeupCommand {
        vp::IoReq *source;
        uint32_t x_mask, y_mask, value;
    };
    vp::IoSlave wakeup_send_itf, wakeup_recv_itf;
    vp::IoMaster wakeup_output_itf;
    vp::IoReq wakeup_request;
    vp::ClockEvent wakeup_event;
    std::deque<WakeupCommand> wakeup_commands;
    uint64_t sync_wakeup_addr;
    uint32_t wakeup_x_mask = 0, wakeup_y_mask = 0;
    uint32_t wakeup_pending = 0, wakeup_received = 0;
    vp::IoReq *wakeup_waiter = nullptr;
    bool wakeup_busy = false;

    vp::WireSlave<bool> boot_ready_itf;
    vp::WireMaster<bool> fetch_start_itf;
    uint32_t fetch_started;

    std::vector<vp::WireMaster<bool>> external_irq_itf;

    uint16_t global_sync_enable;
    uint64_t global_sync_timestamp;

};

ClusterRegisters::ClusterRegisters(vp::ComponentConf &config)
: vp::Component(config), regmap(*this, "regmap"), wakeup_event(this, wakeup_step)
{
    this->traces.new_trace("trace", &trace, vp::DEBUG);

    this->in.set_req_meth(&ClusterRegisters::req);
    this->new_slave_port("input", &this->in);

    this->bootaddr = this->get_js_config()->get("boot_addr")->get_int();
    this->nb_cores = this->get_js_config()->get("nb_cores")->get_int();
    this->cluster_id = this->get_js_config()->get("cluster_id")->get_int();
    this->num_cluster_x = this->get_js_config()->get("num_cluster_x")->get_int();
    this->num_cluster_y = this->get_js_config()->get("num_cluster_y")->get_int();

    this->wakeup_send_itf.set_req_meth(&ClusterRegisters::wakeup_send);
    this->wakeup_recv_itf.set_req_meth(&ClusterRegisters::wakeup_recv);
    this->new_slave_port("wakeup_send", &this->wakeup_send_itf);
    this->new_slave_port("wakeup_recv", &this->wakeup_recv_itf);
    this->new_master_port("wakeup_output", &this->wakeup_output_itf);
    this->sync_wakeup_addr = this->get_js_config()->get_uint("sync_wakeup_addr");
    this->wakeup_output_itf.set_resp_meth(&ClusterRegisters::response);
    this->wakeup_output_itf.set_grant_meth(&ClusterRegisters::grant);

    this->global_sync_enable = 0;
    this->global_sync_timestamp = 0;

    this->barrier_req_itf.resize(this->nb_cores);
    for (int i=0; i<this->nb_cores; i++)
    {
        this->barrier_req_itf[i].set_sync_meth_muxed(&ClusterRegisters::barrier_sync, i);
        this->new_slave_port("barrier_req_" + std::to_string(i), &this->barrier_req_itf[i]);
    }

    this->external_irq_itf.resize(this->nb_cores);
    for (int i=0; i<this->nb_cores; i++)
    {
        this->new_master_port("external_irq_" + std::to_string(i), &this->external_irq_itf[i]);
    }

    this->new_master_port("barrier_ack", &this->barrier_ack_itf);

    this->new_slave_port("boot_ready", &this->boot_ready_itf);
    this->new_master_port("fetch_start", &this->fetch_start_itf);
    this->boot_ready_itf.set_sync_meth(&ClusterRegisters::boot_ready_handler);
    this->fetch_started = 0;

    this->regmap.build(this, &this->trace, "regmap");
    this->regmap.cl_clint_set.register_callback(std::bind(&ClusterRegisters::cl_clint_set_req, this, _1, _2, _3, _4));
    this->regmap.cl_clint_clear.register_callback(std::bind(&ClusterRegisters::cl_clint_clear_req, this, _1, _2, _3, _4));
    this->regmap.nm_config.register_callback(std::bind(&ClusterRegisters::nm_config_req, this, _1, _2, _3, _4));
}

vp::IoReqStatus ClusterRegisters::req(vp::Block *block, vp::IoReq *req)
{
    auto self = static_cast<ClusterRegisters *>(block);
    self->trace.msg("Register access: offset=0x%lx size=%lu write=%d\n",
        req->get_addr(), req->get_size(), req->get_is_write());
    if (req->get_size() != 4 || req->get_addr() % 4) return vp::IO_REQ_INVALID;
    uint32_t value;
    std::memcpy(&value, req->get_data(), 4);
    unsigned offset = req->get_addr();
    if (!req->get_is_write()) {
        switch (offset) {
            case 0x00: value = self->cluster_id; break;
            case 0x04: value = 1; break;
            case 0x08: value = self->num_cluster_x * self->num_cluster_y; break;
            case 0x0c: value = self->num_cluster_x; break;
            case 0x10: value = self->num_cluster_y; break;
            case 0x18: value = 0; break;
            case 0x24: value = self->wakeup_x_mask; break;
            case 0x28: value = self->wakeup_y_mask; break;
            case 0x2c: value = self->wakeup_pending; break;
            case 0x30: value = self->wakeup_received; break;
            default: return vp::IO_REQ_INVALID;
        }
        std::memcpy(req->get_data(), &value, 4);
        return vp::IO_REQ_OK;
    }
    switch (offset) {
        case 0x14:
            if (!self->global_sync_enable) {
                self->global_sync_enable = 1;
                self->global_sync_timestamp = self->clock.get_cycles();
            } else {
                self->global_sync_enable = 0;
                self->trace.msg("Cluster sync interval: %ld cycles, type %u\n",
                    self->clock.get_cycles() - self->global_sync_timestamp, value);
            }
            return vp::IO_REQ_OK;
        case 0x20:
            // One designated core waits for cluster notifications. It can use
            // the existing local barrier to release the other cores afterwards.
            if (self->wakeup_pending) {
                --self->wakeup_pending;
                return vp::IO_REQ_OK;
            }
            if (self->wakeup_waiter) return vp::IO_REQ_INVALID;
            self->wakeup_waiter = req;
            return vp::IO_REQ_PENDING;
        case 0x24: self->wakeup_x_mask = value; return vp::IO_REQ_OK;
        case 0x28: self->wakeup_y_mask = value; return vp::IO_REQ_OK;
        default: return vp::IO_REQ_INVALID;
    }
}

vp::IoReqStatus ClusterRegisters::wakeup_send(vp::Block *block, vp::IoReq *req)
{
    auto self = static_cast<ClusterRegisters *>(block);
    if (!req->get_is_write() || req->get_addr() != 0 || req->get_size() != 4)
        return vp::IO_REQ_INVALID;
    uint32_t x = self->wakeup_x_mask & ((1ULL << self->num_cluster_x) - 1);
    uint32_t y = self->wakeup_y_mask & ((1ULL << self->num_cluster_y) - 1);
    if (!x || !y) return vp::IO_REQ_OK;
    uint32_t value;
    std::memcpy(&value, req->get_data(), 4);
    self->wakeup_commands.push_back({req, x, y, value});
    self->wakeup_event.enqueue();
    return vp::IO_REQ_PENDING;
}

void ClusterRegisters::wakeup_step(vp::Block *block, vp::ClockEvent *)
{
    auto self = static_cast<ClusterRegisters *>(block);
    if (self->wakeup_busy || self->wakeup_commands.empty()) return;
    auto &command = self->wakeup_commands.front();
    auto &req = self->wakeup_request;
    req.init();
    req.set_addr(self->sync_wakeup_addr);
    req.set_size(4);
    req.set_is_write(true);
    req.set_second_data(nullptr);
    req.set_data(reinterpret_cast<uint8_t *>(&command.value));
    req.status = vp::IO_REQ_OK;
    std::memset(req.get_payload(), 0, req.get_payload_size());
    req.get_payload()[0] = 1;
    std::memcpy(req.get_payload() + 1, &command.y_mask, 4);
    std::memcpy(req.get_payload() + 5, &command.x_mask, 4);
    self->wakeup_busy = true;
    vp::IoReqStatus status = self->wakeup_output_itf.req(&req);
    if (status == vp::IO_REQ_OK || status == vp::IO_REQ_INVALID) {
        req.status = status;
        response(self, &req);
    }
}

void ClusterRegisters::boot_ready_handler(vp::Block *block, bool value)
{
    auto self = static_cast<ClusterRegisters *>(block);
    if (value && !self->fetch_started) {
        self->fetch_started = 1;
        self->fetch_start_itf.sync(true);
    }
}

vp::IoReqStatus ClusterRegisters::wakeup_recv(vp::Block *block, vp::IoReq *req)
{
    auto self = static_cast<ClusterRegisters *>(block);
    if (!req->get_is_write() || req->get_addr() != 0 || req->get_size() != 4)
        return vp::IO_REQ_INVALID;
    ++self->wakeup_received;
    if (self->wakeup_waiter) {
        auto waiter = self->wakeup_waiter;
        self->wakeup_waiter = nullptr;
        waiter->status = vp::IO_REQ_OK;
        waiter->get_resp_port()->resp(waiter);
    } else {
        if (self->wakeup_pending == UINT32_MAX)
            self->trace.fatal("Cluster wakeup notification counter overflow\n");
        ++self->wakeup_pending;
    }
    return vp::IO_REQ_OK;
}

void ClusterRegisters::barrier_sync(vp::Block *__this, bool value, int id)
{
    ClusterRegisters *_this = (ClusterRegisters *)__this;
    _this->barrier_status.set(_this->barrier_status.get() | (value << id));

    // _this->trace.msg(vp::Trace::LEVEL_DEBUG, "Barrier sync (id: %d, status: 0x%x)\n", id, _this->barrier_status.get());

    if (_this->barrier_status.get() == (1ULL << _this->nb_cores) - 1)
    {
        // _this->trace.msg(vp::Trace::LEVEL_DEBUG, "Barrier reached\n");

        _this->barrier_status.set(0);
        _this->barrier_ack_itf.sync(1);
    }
}

void ClusterRegisters::reset(bool active)
{
    this->new_reg("barrier_status", &this->barrier_status, 0, true);
}


void ClusterRegisters::cl_clint_set_req(uint64_t reg_offset, int size, uint8_t *value, bool is_write)
{
    this->regmap.cl_clint_set.update(reg_offset, size, value, is_write);
    for (int i=0; i<this->nb_cores; i++)
    {
        int irq_status = (this->regmap.cl_clint_set.get() >> i) & 1;
        if (irq_status == 1)
        {
            this->external_irq_itf[i].sync(true);
        }
    }
}

void ClusterRegisters::cl_clint_clear_req(uint64_t reg_offset, int size, uint8_t *value, bool is_write)
{
    this->regmap.cl_clint_clear.update(reg_offset, size, value, is_write);
    for (int i=0; i<this->nb_cores; i++)
    {
        int irq_status = (this->regmap.cl_clint_clear.get() >> i) & 1;
        if (irq_status == 1)
        {
            this->external_irq_itf[i].sync(false);
        }
    }
}

void ClusterRegisters::nm_config_req(uint64_t offset, int size, uint8_t *value, bool is_write)
{
    if (is_write)
    {
        // Let the register update its internal value
        this->regmap.nm_config.update(offset, size, value, is_write);

        // Read back the fields using the auto-generated accessors
        uint32_t n = this->regmap.nm_config.format_n_get();
        uint32_t m = this->regmap.nm_config.format_m_get();

        // Trace it
        this->trace.msg(vp::DEBUG, "NM_CONFIG write: N=%u, M=%u\n", n, m);
    }
    else
    {
        // Just forward to the register’s default read behavior
        this->regmap.nm_config.update(offset, size, value, is_write);
    }
}


void ClusterRegisters::response(vp::Block *block, vp::IoReq *req)
{
    auto self = static_cast<ClusterRegisters *>(block);
    auto source = self->wakeup_commands.front().source;
    self->wakeup_commands.pop_front();
    self->wakeup_busy = false;
    source->status = req->status;
    source->get_resp_port()->resp(source);
    if (!self->wakeup_commands.empty()) self->wakeup_event.enqueue();
}


void ClusterRegisters::grant(vp::Block *__this, vp::IoReq *req)
{

}


extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new ClusterRegisters(config);
}
