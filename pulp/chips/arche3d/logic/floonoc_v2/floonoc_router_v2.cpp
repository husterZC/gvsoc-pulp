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

#include <vp/vp.hpp>
#include "floonoc_v2.hpp"
#include "floonoc_router_v2.hpp"
#include "collective_reduction.hpp"

static const char *dir_names[SoftHierRouterV2::DIR_NB] = {"right", "left", "up", "down", "local"};

SoftHierRouterV2::SoftHierRouterV2(vp::ComponentConf &config)
    : vp::Component(config), fsm_event(this, &SoftHierRouterV2::fsm_handler),
      signal_req(*this, "req", 64, vp::SignalCommon::ResetKind::HighZ),
      signal_req_size(*this, "req_size", 64, vp::SignalCommon::ResetKind::HighZ),
      signal_req_is_write(*this, "req_is_write", 1, vp::SignalCommon::ResetKind::HighZ),
      stalled_queues{{vp::Signal<bool>(*this, "stalled_queue_right", 1),
                      vp::Signal<bool>(*this, "stalled_queue_left", 1),
                      vp::Signal<bool>(*this, "stalled_queue_up", 1),
                      vp::Signal<bool>(*this, "stalled_queue_down", 1),
                      vp::Signal<bool>(*this, "stalled_queue_local", 1)}},
      input_ports{{SoftHierFloonocLinkSlave(DIR_RIGHT, &SoftHierRouterV2::link_req),
                   SoftHierFloonocLinkSlave(DIR_LEFT, &SoftHierRouterV2::link_req),
                   SoftHierFloonocLinkSlave(DIR_UP, &SoftHierRouterV2::link_req),
                   SoftHierFloonocLinkSlave(DIR_DOWN, &SoftHierRouterV2::link_req),
                   SoftHierFloonocLinkSlave(DIR_LOCAL, &SoftHierRouterV2::link_req)}},
      output_ports{{SoftHierFloonocLinkMaster(DIR_RIGHT, &SoftHierRouterV2::link_unstall),
                    SoftHierFloonocLinkMaster(DIR_LEFT, &SoftHierRouterV2::link_unstall),
                    SoftHierFloonocLinkMaster(DIR_UP, &SoftHierRouterV2::link_unstall),
                    SoftHierFloonocLinkMaster(DIR_DOWN, &SoftHierRouterV2::link_unstall),
                    SoftHierFloonocLinkMaster(DIR_LOCAL, &SoftHierRouterV2::link_unstall)}}
{
    this->traces.new_trace("trace", &trace, vp::DEBUG);

    this->x = get_js_config()->get_int("x");
    this->y = get_js_config()->get_int("y");
    this->dim_x = get_js_config()->get_int("dim_x");
    this->dim_y = get_js_config()->get_int("dim_y");
    this->queue_size = get_js_config()->get_int("router_input_queue_size");

    for (int i = 0; i < DIR_NB; i++)
    {
        this->input_queues[i] =
            new vp::Queue(this, "input_queue_" + std::to_string(i), &this->fsm_event);

        this->new_slave_port(std::string("input_") + dir_names[i], &this->input_ports[i]);
        this->new_master_port(std::string("output_") + dir_names[i], &this->output_ports[i]);

        this->stalled_queues[i] = false;
    }
}

SoftHierRouterV2::~SoftHierRouterV2()
{
    for (int i = 0; i < DIR_NB; i++)
    {
        delete this->input_queues[i];
    }
}

bool SoftHierRouterV2::link_req(vp::Block *__this, SoftHierFloonocReqV2 *req, int queue_index)
{
    SoftHierRouterV2 *_this = (SoftHierRouterV2 *)__this;

    _this->trace.msg(vp::Trace::LEVEL_DEBUG,
                     "Handle request (req: %p, base: 0x%lx, size: 0x%lx, queue: %d)\n", req,
                     req->get_addr(), req->get_size(), queue_index);

    _this->signal_req.set_and_release(req->initiator_addr);
    _this->signal_req_size.set_and_release(req->get_size());
    _this->signal_req_is_write.set_and_release(req->get_is_write());

    _this->trace.msg(vp::Trace::LEVEL_DEBUG, "Pushed request to input queue (req: %p, queue: %d)\n",
                     req, queue_index);

    vp::Queue *queue = _this->input_queues[queue_index];
    queue->push_back(req, arche3d_collective::HOP_CYCLES - 1);

    return queue->size() > _this->queue_size;
}

// Contributions enter bounded per-axis/slot storage before output arbitration.
// Matching + reduction occupy the existing two-cycle input pipeline: there is
// no request wave and no second join queue after that pipeline.
void SoftHierRouterV2::fsm_handler(vp::Block *block, vp::ClockEvent *)
{
    auto *self = static_cast<SoftHierRouterV2 *>(block);
    auto pop = [self](int input)
    {
        auto *q = self->input_queues[input];
        q->pop();
        if (q->size() == self->queue_size)
        {
            self->input_ports[input].unstall();
        }
    };
    for (int input = 0; input < DIR_NB; ++input)
    {
        auto *q = self->input_queues[input];
        if (q->empty())
        {
            continue;
        }
        auto *req = static_cast<SoftHierFloonocReqV2 *>(q->head());
        if (req->collective.type > arche3d_collective::BROADCAST &&
            self->collective_reduce(req, input))
        {
            pop(input);
        }
    }
    bool output_full[DIR_NB] = {};
    int first = self->current_queue;
    for (int n = 0; n <= DIR_NB; ++n)
    {
        int input = (first + n) % (DIR_NB + 1);
        if (input == DIR_NB)
        {
            unsigned first_reduction = self->next_reduction;
            for (unsigned i = 0; i < self->reductions.size(); ++i)
            {
                unsigned index = (first_reduction + i) % self->reductions.size();
                auto &state = self->reductions[index];
                auto *req = state.result;
                if (!req)
                {
                    continue;
                }
                int output = self->reduction_output(req->collective);
                if (output_full[output] || self->stalled_queues[output] ||
                    self->output_owner[output] != -1)
                {
                    continue;
                }
                output_full[output] = true;
                self->trace.msg(vp::Trace::LEVEL_DEBUG,
                                "COL_REDUCE_FORWARD at=(%d,%d) slot=%u epoch=%u output=%d\n",
                                self->x - 1, self->y - 1, req->collective.slot,
                                req->collective.epoch, output);
                for (auto *part : state.inputs)
                {
                    if (part && part != req)
                    {
                        SoftHierFloonocReqV2Allocator::get()->free(part);
                    }
                }
                state = {};
                self->stalled_queues[output] = self->output_ports[output].req(req);
                self->next_reduction = (index + 1) % self->reductions.size();
                self->current_queue = 0;
            }
            continue;
        }
        auto *q = self->input_queues[input];
        if (q->empty())
        {
            continue;
        }
        auto *req = static_cast<SoftHierFloonocReqV2 *>(q->head());
        if (req->collective.type > arche3d_collective::BROADCAST)
        {
            continue;
        }
        if (req->collective.type == arche3d_collective::BROADCAST)
        {
            if (self->collective_forward(req, input, output_full))
            {
                pop(input);
                SoftHierFloonocReqV2Allocator::get()->free(req);
                self->current_queue = (input + 1) % (DIR_NB + 1);
            }
            continue;
        }
        int nx, ny;
        self->get_next_router_pos(req->dest_x, req->dest_y, nx, ny);
        int output = self->get_req_queue(nx, ny);
        if (output_full[output] || self->stalled_queues[output] ||
            (self->output_owner[output] != -1 && self->output_owner[output] != input))
        {
            continue;
        }
        output_full[output] = true;
        self->output_owner[output] = req->is_last ? -1 : input;
        self->trace.msg(vp::Trace::LEVEL_DEBUG, "NOC_V2_HOP req=%p from=(%d,%d) to=(%d,%d)\n", req,
                        self->x, self->y, nx, ny);
        pop(input);
        self->current_queue = (input + 1) % (DIR_NB + 1);
        self->stalled_queues[output] = self->output_ports[output].req(req);
    }
    bool pending = false;
    for (auto *q : self->input_queues)
    {
        pending |= q->size() != 0;
    }
    for (auto &state : self->reductions)
    {
        pending |= state.result != nullptr;
    }
    if (pending)
    {
        self->fsm_event.enqueue();
    }
}

int SoftHierRouterV2::reduction_output(const Arche3dCollectivePacket &p)
{
    int pos = p.column ? y - 1 : x - 1, line = p.column ? x - 1 : y - 1;
    // Reverse of the multicast tree: reduce each secondary-axis branch into
    // the root's primary-axis trunk, then reduce that trunk toward the root.
    if (line != p.line)
    {
        if (p.column)
        {
            return line < p.line ? DIR_RIGHT : DIR_LEFT;
        }
        return line < p.line ? DIR_UP : DIR_DOWN;
    }
    if (pos == p.root)
    {
        return DIR_LOCAL;
    }
    if (p.column)
    {
        return pos < p.root ? DIR_UP : DIR_DOWN;
    }
    return pos < p.root ? DIR_RIGHT : DIR_LEFT;
}

int SoftHierRouterV2::collective_routes(const Arche3dCollectivePacket &p)
{
    int pos = p.column ? y - 1 : x - 1, line = p.column ? x - 1 : y - 1;
    int count = (p.column ? dim_y : dim_x) - 2;
    int lines = (p.column ? dim_x : dim_y) - 2;
    unsigned primary_mask = p.column ? p.y_mask : p.x_mask;
    unsigned secondary_mask = p.column ? p.x_mask : p.y_mask;
    auto matches = [](unsigned value, unsigned root, unsigned mask)
    { return (value & mask) == (root & mask); };
    auto beyond = [&](int value, int end, int step, unsigned root, unsigned mask)
    {
        for (int next = value + step; next != end; next += step)
        {
            if (matches(next, root, mask))
            {
                return true;
            }
        }
        return false;
    };
    bool primary_selected = matches(pos, p.root, primary_mask);
    if (line != p.line && !primary_selected)
    {
        trace.fatal("Collective packet outside its selected routing tree\n");
    }
    int routes = p.selects(x - 1, y - 1) ? 1 << DIR_LOCAL : 0;
    int primary_low = p.column ? DIR_DOWN : DIR_LEFT;
    int primary_high = p.column ? DIR_UP : DIR_RIGHT;
    int secondary_low = p.column ? DIR_LEFT : DIR_DOWN;
    int secondary_high = p.column ? DIR_RIGHT : DIR_UP;
    if (line == p.line)
    {
        if (pos <= p.root && beyond(pos, -1, -1, p.root, primary_mask))
        {
            routes |= 1 << primary_low;
        }
        if (pos >= p.root && beyond(pos, count, 1, p.root, primary_mask))
        {
            routes |= 1 << primary_high;
        }
    }
    if (primary_selected)
    {
        if (line <= p.line && beyond(line, -1, -1, p.line, secondary_mask))
        {
            routes |= 1 << secondary_low;
        }
        if (line >= p.line && beyond(line, lines, 1, p.line, secondary_mask))
        {
            routes |= 1 << secondary_high;
        }
    }
    return routes;
}

bool SoftHierRouterV2::collective_reduce(SoftHierFloonocReqV2 *req, int input)
{
    auto &p = req->collective;
    if (p.slot >= arche3d_collective::SLOTS || p.root_x() >= unsigned(dim_x - 2) ||
        p.root_y() >= unsigned(dim_y - 2))
    {
        trace.fatal("Invalid collective route or slot\n");
    }
    auto &state = reductions[p.column * arche3d_collective::SLOTS + p.slot];
    if (state.result)
    {
        return false;
    }
    if (state.mask &&
        (state.epoch != p.epoch || state.root != p.root || state.line != p.line ||
         state.x_mask != p.x_mask || state.y_mask != p.y_mask || state.offset != p.offset))
    {
        return false;
    }
    int expected = collective_routes(p);
    if (!(expected & (1 << input)) || (state.mask & (1 << input)))
    {
        trace.fatal("Duplicate or unexpected collective contribution\n");
    }
    if (state.mask && (state.type != p.type || state.bytes != p.data.size() ||
                       state.total_bytes != p.total_bytes))
    {
        trace.fatal("Collective operation/length mismatch\n");
    }
    state.root = p.root;
    state.line = p.line;
    state.x_mask = p.x_mask;
    state.y_mask = p.y_mask;
    state.epoch = p.epoch;
    state.type = p.type;
    state.bytes = p.data.size();
    state.offset = p.offset;
    state.total_bytes = p.total_bytes;
    state.inputs[input] = req;
    state.mask |= 1 << input;
    if (state.mask != expected)
    {
        return true;
    }
    // Transit routers need no local operand. Seed from the first selected
    // input, preserving fixed order independently of contribution timing.
    SoftHierFloonocReqV2 *result = nullptr;
    for (int dir : {DIR_LOCAL, DIR_RIGHT, DIR_LEFT, DIR_UP, DIR_DOWN})
    {
        auto *other = state.inputs[dir];
        if (!other)
        {
            continue;
        }
        if (!result)
        {
            result = other;
            continue;
        }
        arche3d_collective::combine(p.type, result->collective.data.data(),
                                    other->collective.data.data(), p.data.size());
    }
    state.result = result;
    return true;
}

bool SoftHierRouterV2::collective_forward(SoftHierFloonocReqV2 *req, int input, bool *output_full)
{
    auto &p = req->collective;
    if (req->collective_outputs < 0)
    {
        if (input != reduction_output(p))
        {
            trace.fatal("Multicast packet arrived outside its routing tree\n");
        }
        req->collective_outputs = collective_routes(p);
        if (!req->collective_outputs)
        {
            trace.fatal("Multicast branch has no selected receivers\n");
        }
    }
    for (int dir = 0; dir < DIR_NB; ++dir)
    {
        if (!(req->collective_outputs & (1 << dir)) || output_full[dir] || stalled_queues[dir] ||
            (output_owner[dir] != -1 && output_owner[dir] != input))
        {
            continue;
        }
        auto *child = SoftHierFloonocReqV2Allocator::get()->clone(req);
        child->collective_outputs = -1;
        req->collective_outputs &= ~(1 << dir);
        output_full[dir] = true;
        trace.msg(vp::Trace::LEVEL_DEBUG,
                  "COL_MULTICAST_FORWARD at=(%d,%d) slot=%u epoch=%u output=%d\n", x - 1, y - 1,
                  p.slot, p.epoch, dir);
        stalled_queues[dir] = output_ports[dir].req(child);
    }
    return req->collective_outputs == 0;
}

void SoftHierRouterV2::get_next_router_pos(int dest_x, int dest_y, int &next_x, int &next_y)
{
    if (dest_x < 0)
    {
        switch (dest_x + 4)
        {
        case DIR_UP:
            next_x = this->x;
            next_y = this->y + 1;
            break;
        case DIR_DOWN:
            next_x = this->x;
            next_y = this->y - 1;
            break;
        case DIR_RIGHT:
            next_y = this->y;
            next_x = this->x + 1;
            break;
        case DIR_LEFT:
            next_y = this->y;
            next_x = this->x - 1;
            break;
        }
    }
    else
    {
        if (dest_x == this->x && dest_y == this->y)
        {
            next_x = this->x;
            next_y = this->y;
            return;
        }

        if (dest_x != this->x)
        {
            next_x = dest_x < this->x ? this->x - 1 : this->x + 1;
            next_y = this->y;

            if (next_x != 0 && next_x != this->dim_x - 1 || next_y == dest_y)
            {
                return;
            }
        }

        next_x = this->x;
        next_y = dest_y < this->y ? this->y - 1 : this->y + 1;
    }
}

void SoftHierRouterV2::link_unstall(vp::Block *__this, int output_id)
{
    SoftHierRouterV2 *_this = (SoftHierRouterV2 *)__this;
    _this->trace.msg(vp::Trace::LEVEL_TRACE, "Unstalling queue (queue: %d)\n", output_id);
    _this->stalled_queues[output_id] = false;
    _this->fsm_event.enqueue();
}

int SoftHierRouterV2::get_req_queue(int from_x, int from_y)
{
    int queue_index = 0;
    if (from_x != this->x)
    {
        queue_index = from_x < this->x ? DIR_LEFT : DIR_RIGHT;
    }
    else if (from_y != this->y)
    {
        queue_index = from_y < this->y ? DIR_DOWN : DIR_UP;
    }
    else
    {
        queue_index = DIR_LOCAL;
    }

    return queue_index;
}

void SoftHierRouterV2::reset(bool active)
{
    if (active)
    {
        this->current_queue = 0;
        this->next_reduction = 0;
        for (auto &state : reductions)
        {
            for (auto *req : state.inputs)
            {
                if (req)
                {
                    SoftHierFloonocReqV2Allocator::get()->free(req);
                }
            }
            state = {};
        }
        for (int i = 0; i < DIR_NB; i++)
        {
            this->stalled_queues[i] = false;
            this->output_owner[i] = -1;
        }
    }
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new SoftHierRouterV2(config);
}
