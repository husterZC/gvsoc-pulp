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

/*
 * Authors: Germain Haugou, ETH (germain.haugou@iis.ee.ethz.ch)
 */

#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <cstring>
#include "floonoc.hpp"
#include "floonoc_router.hpp"
#include "floonoc_network_interface.hpp"



Router::Router(FlooNoc *noc, int x, int y, int queue_size)
    : vp::Block(noc, "router_" + std::to_string(x) + "_" + std::to_string(y)),
    fsm_event(this, &Router::fsm_handler)
{
    this->traces.new_trace("trace", &trace, vp::DEBUG);

    this->noc = noc;
    this->x = x;
    this->y = y;
    this->queue_size = queue_size;

    for (int i=0; i<5; i++)
    {
        this->input_queues[i] = new vp::Queue(this, "input_queue_" + std::to_string(i),
            &this->fsm_event);
        this->collective_generated_queues[i] = new std::queue<vp::IoReq*>();
        this->stalled_queues[i] = false;
    }
}



bool Router::handle_request(vp::IoReq *req, int from_x, int from_y)
{
    this->trace.msg(vp::Trace::LEVEL_DEBUG, "Handle request (req: %p, from: (%d, %d)\n", req, from_x, from_y);

    // Each direction has its own input queue to properly implement the round-robin
    // Get the one for the router or network interface which sent this request
    int queue_index = this->get_req_queue(from_x, from_y);

    this->trace.msg(vp::Trace::LEVEL_DEBUG, "Pushed request to input queue (req: %p, queue: %d)\n", req, queue_index);

    // And push it to the queue. The queue will automatically trigger the FSM if needed
    vp::Queue *queue = this->input_queues[queue_index];
    queue->push_back(req);

    // We let the source enqueue one more request than what is possible to model the fact the fact
    // the request is stalled. This will then stall the source which will not send any request there
    // anymore until we unstall it
    return queue->size() > this->queue_size;
}



void Router::fsm_handler(vp::Block *__this, vp::ClockEvent *event)
{
    Router *_this = (Router *)__this;
    _this->trace.msg(vp::Trace::LEVEL_DEBUG, "Checking pending requests\n");

    // Get the currently active queue and update it to implement the round-robin
    int queue_index = _this->current_queue;

    _this->current_queue += 1;
    if (_this->current_queue == 5)
    {
        _this->current_queue = 0;
    }

    // Then go through the 5 queues until we find a request which can be propagated
    for (int i=0; i<5; i++)
    {
        vp::Queue *queue = _this->input_queues[queue_index];
        // if (!queue->empty())
        if (queue->size())
        {
            vp::IoReq *req = (vp::IoReq *)queue->head();

            // Extract the destination from the request, that was filled in the network interface
            // when the request was created
            int to_x = req->get_int(FlooNoc::REQ_DEST_X);
            int to_y = req->get_int(FlooNoc::REQ_DEST_Y);

            // Get the next position in the grid. This takes care of deciding which path is taken
            // to go to the destination
            int next_x, next_y;
            _this->get_next_router_pos(to_x, to_y, next_x, next_y);

            /**************************************/
            /*   Deal With Collective Primitives  */
            /**************************************/
            if (req->get_int(FlooNoc::REQ_COLL_TYPE))
            {
                std::queue<int> analyze_result;
                //1. analyze
                _this->collective_analyze(req, &analyze_result, _this->x, _this->y);
                _this->trace.msg(vp::Trace::LEVEL_DEBUG, "[Collective] analyze_result has %d elements\n",analyze_result.size());

                //2. process
                if (analyze_result.size() == 0)
                {
                    //2.1 No action needed, directly response
                    _this->noc->handle_request_end(req);
                    _this->pop_input_queue(queue_index);
                    queue_index += 1; if (queue_index == 5) queue_index = 0;
                    continue;
                } else
                if (analyze_result.size() == 1)
                {
                    //2.2 Only one request is needed, modfiy the req accordingly
                    req->set_int(FlooNoc::REQ_MOMENTUM, analyze_result.front());
                    to_x = _this->x; to_y = _this->y; next_x = _this->x; next_y = _this->y;
                    if (analyze_result.front() == FlooNoc::MOMENTUM_RIGHT) {to_x += 1; next_x += 1;}
                    if (analyze_result.front() == FlooNoc::MOMENTUM_LEFT)  {to_x -= 1; next_x -= 1;}
                    if (analyze_result.front() == FlooNoc::MOMENTUM_UP)    {to_y += 1; next_y += 1;}
                    if (analyze_result.front() == FlooNoc::MOMENTUM_DOWN)  {to_y -= 1; next_y -= 1;}
                    if (analyze_result.front() == FlooNoc::MOMENTUM_ZERO)  {/*Do Nothing*/}
                    req->set_int(FlooNoc::REQ_DEST_X, to_x);
                    req->set_int(FlooNoc::REQ_DEST_Y, to_y);
                } else {
                    // Generate Kid Requests Accordingly
                    _this->collective_generate(req, &analyze_result, _this->x, _this->y);
                    _this->pop_input_queue(queue_index);
                    queue_index += 1; if (queue_index == 5) queue_index = 0;
                    continue;
                }
            }

            // In case the request goes to a queue which is stalled, skip it
            // we'll retry later
            int queue_id = _this->get_req_queue(next_x, next_y);
            if (_this->stalled_queues[queue_id])
            {
                queue_index += 1; if (queue_index == 5) queue_index = 0;
                continue;
            }

            _this->pop_input_queue(queue_index);

            // Now send to the next position
            if (to_x == _this->x && to_y == _this->y)
            {
                // If next position is the same as the current one, it means it arrived to
                // destination, we need to forward to the fina target
                _this->send_to_target(req, _this->x, _this->y);
            }
            else
            {
                // Otherwise forward to next position
                Router *router = _this->noc->get_router(next_x, next_y);

                if (router == NULL)
                {
                    // It is possible that we don't have any router at the destination if it is on
                    // the edge. In this case just forward to target
                    _this->send_to_target(req, next_x, next_y);
                }
                else
                {
                    _this->trace.msg(vp::Trace::LEVEL_DEBUG, "Forwarding request to next router (req: %p, next_position: (%d, %d))\n",
                        req, next_x, next_y);

                    // Send the request to next router, and in case it reports that its input queue
                    // is full, stall the corresponding output queue to make sure we stop sending
                    // there until the queue is unstalled
                    if (router->handle_request(req, _this->x, _this->y))
                    {
                        _this->stalled_queues[queue_id] = true;
                    }
                }
            }

            // Since we removed a request, check in next cycle if there is another one to handle
            _this->fsm_event.enqueue();

            // break;
        }

        // If we didn't any ready request, try with next queue
        queue_index += 1; if (queue_index == 5) queue_index = 0;
    }

    // Deal with collective primitives generated pending requests
    for (int i = 0; i < 5; ++i)
    {
        std::queue<vp::IoReq *> * queue = _this->collective_generated_queues[i];
        if (queue->size())
        {
            vp::IoReq *req = (vp::IoReq *)queue->front();
            int to_x = req->get_int(FlooNoc::REQ_DEST_X);
            int to_y = req->get_int(FlooNoc::REQ_DEST_Y);
            int next_x, next_y;
            _this->get_next_router_pos(to_x, to_y, next_x, next_y);
            int queue_id = _this->get_req_queue(next_x, next_y);
            if (_this->stalled_queues[queue_id]) continue;
            queue->pop();
            if (to_x == _this->x && to_y == _this->y)
            {
                _this->send_to_target(req, _this->x, _this->y);
            }
            else
            {
                Router *router = _this->noc->get_router(next_x, next_y);
                if (router == NULL)
                {
                    _this->send_to_target(req, next_x, next_y);
                }
                else
                {
                    _this->trace.msg(vp::Trace::LEVEL_DEBUG, "[Collective] Forwarding request to next router (req: %p, next_position: (%d, %d))\n",
                        req, next_x, next_y);
                    if (router->handle_request(req, _this->x, _this->y))
                    {
                        _this->stalled_queues[queue_id] = true;
                    }
                }
            }
            _this->fsm_event.enqueue();
        }
    }
}



void Router::pop_input_queue(int queue_index)
{
    auto queue = this->input_queues[queue_index];
    queue->pop();
    if (queue->size() == this->queue_size) {
        // Grant the predecessor of the input we just drained, including when
        // a multicast branches. The next-hop output direction is unrelated.
        int x, y;
        this->get_pos_from_queue(queue_index, x, y);
        if (x == this->x && y == this->y)
            this->noc->get_network_interface(x, y)->unstall_queue(this->x, this->y);
        else
            this->noc->get_router(x, y)->unstall_queue(this->x, this->y);
    }
    this->fsm_event.enqueue();
}

void Router::send_to_target(vp::IoReq *req, int pos_x, int pos_y)
{
    vp::IoMaster *target = this->noc->get_target(pos_x, pos_y);

    this->trace.msg(vp::Trace::LEVEL_DEBUG, "Sending request to target (req: %p, position: (%d, %d))\n",
        req, pos_x, pos_y);

    vp::IoReqStatus result = target->req(req);
    if (result == vp::IO_REQ_OK || result == vp::IO_REQ_INVALID)
    {
        // If the request is processed synchronously, immediately notify the network interface

        // We need to store the status in the request so that it is properly propagated to the
        // initiator request
        req->status = result;
        this->noc->handle_request_end(req);
    }
    else if (result == vp::IO_REQ_DENIED)
    {
        int queue = this->get_req_queue(pos_x, pos_y);

        // In case it is denied, the request has been queued in the target, we just need to make
        // sure we don't send any other request there until we reveive the grant callback
        this->stalled_queues[queue] = true;

        // Store the router in the request. Since the grant is received by top noc,
        // it will use this argument to notify the router about the grant
        *(Router **)req->arg_get(FlooNoc::REQ_ROUTER) = this;
        // Also store the queue, the router will use it to know which queue to unstall
        *(int *)req->arg_get(FlooNoc::REQ_QUEUE) = queue;
    }
    else
    {
        // In case of asynchronous response, the network interface will be notified by the
        // the response callback
    }
}

const char * get_momentum_name(int momentum){
    if (momentum == FlooNoc::MOMENTUM_RIGHT) return "Right";
    if (momentum == FlooNoc::MOMENTUM_LEFT) return "Left";
    if (momentum == FlooNoc::MOMENTUM_UP) return "Up";
    if (momentum == FlooNoc::MOMENTUM_DOWN) return "Down";
    if (momentum == FlooNoc::MOMENTUM_ZERO) return "Zero";
    return "Unknow";
}

void Router::collective_analyze(vp::IoReq *req, std::queue<int> *queue,
                                int router_x, int router_y)
{
    unsigned x = router_x - 1, y = router_y - 1;
    uint32_t xs = req->get_int(FlooNoc::REQ_COL_MASK);
    uint32_t ys = req->get_int(FlooNoc::REQ_ROW_MASK);
    int direction = req->get_int(FlooNoc::REQ_MOMENTUM);
    bool selected_x = xs & (1U << x);
    if (selected_x && (ys & (1U << y))) queue->push(FlooNoc::MOMENTUM_ZERO);

    // A horizontal spine through the source row branches vertically at each
    // selected column. This also reaches rectangles excluding the source row
    // and column, without duplicate deliveries or destinations on the borders.
    bool horizontal = direction == FlooNoc::MOMENTUM_ZERO ||
        direction == FlooNoc::MOMENTUM_LEFT || direction == FlooNoc::MOMENTUM_RIGHT;
    if (horizontal && ys) {
        if (direction != FlooNoc::MOMENTUM_LEFT && (uint64_t(xs) >> (x + 1)))
            queue->push(FlooNoc::MOMENTUM_RIGHT);
        if (direction != FlooNoc::MOMENTUM_RIGHT && (xs & ((1ULL << x) - 1)))
            queue->push(FlooNoc::MOMENTUM_LEFT);
    }
    if (selected_x) {
        if ((horizontal || direction == FlooNoc::MOMENTUM_UP) &&
            (uint64_t(ys) >> (y + 1))) queue->push(FlooNoc::MOMENTUM_UP);
        if ((horizontal || direction == FlooNoc::MOMENTUM_DOWN) &&
            (ys & ((1ULL << y) - 1))) queue->push(FlooNoc::MOMENTUM_DOWN);
    }
}

void Router::collective_generate(vp::IoReq * req, std::queue<int> * queue, int router_x, int router_y)
{
    int num_req = queue->size();
    for (int i = 0; i < num_req; ++i)
    {
        int momentum = queue->front();
        this->trace.msg(vp::Trace::LEVEL_DEBUG, "[Collective] Generate New Req in Momentum of %s\n",get_momentum_name(momentum));

        //New request
        vp::IoReq *kid = new vp::IoReq();
        kid->init();
        kid->status = vp::IO_REQ_OK;
        kid->arg_alloc(FlooNoc::REQ_NB_ARGS);
        *kid->arg_get(FlooNoc::REQ_DEST_NI) = *req->arg_get(FlooNoc::REQ_DEST_NI);
        *kid->arg_get(FlooNoc::REQ_DEST_BURST) = *req->arg_get(FlooNoc::REQ_DEST_BURST);
        *kid->arg_get(FlooNoc::REQ_DEST_BASE) = *req->arg_get(FlooNoc::REQ_DEST_BASE);
        kid->set_size(req->get_size());
        uint8_t * data_ptr = new uint8_t[req->get_size()];
        std::memcpy(data_ptr, req->get_data(), req->get_size());
        kid->set_data(data_ptr);
        kid->set_is_write(req->get_is_write());
        if (this->noc->atomics)
        {
            kid->set_opcode(req->get_opcode());
            kid->set_second_data(req->get_second_data());
        }
        *kid->arg_get(FlooNoc::REQ_PARENT) = (void *)req;
        *kid->arg_get(FlooNoc::REQ_COLL_TYPE) = *req->arg_get(FlooNoc::REQ_COLL_TYPE);
        *kid->arg_get(FlooNoc::REQ_ROW_MASK) = *req->arg_get(FlooNoc::REQ_ROW_MASK);
        *kid->arg_get(FlooNoc::REQ_COL_MASK) = *req->arg_get(FlooNoc::REQ_COL_MASK);
        *kid->arg_get(FlooNoc::REQ_PEND_KIDS) = (void *)0;
        kid->set_int(FlooNoc::REQ_MOMENTUM, momentum);
        kid->set_addr(req->get_addr());
        *kid->arg_get(FlooNoc::REQ_SRC_X) = *req->arg_get(FlooNoc::REQ_SRC_X);
        *kid->arg_get(FlooNoc::REQ_SRC_Y) = *req->arg_get(FlooNoc::REQ_SRC_Y);
        int to_x = router_x;
        int to_y = router_y;
        if (queue->front() == FlooNoc::MOMENTUM_RIGHT) {to_x += 1;}
        if (queue->front() == FlooNoc::MOMENTUM_LEFT)  {to_x -= 1;}
        if (queue->front() == FlooNoc::MOMENTUM_UP)    {to_y += 1;}
        if (queue->front() == FlooNoc::MOMENTUM_DOWN)  {to_y -= 1;}
        if (queue->front() == FlooNoc::MOMENTUM_ZERO)  {/*Do Nothing*/}
        kid->set_int(FlooNoc::REQ_DEST_X, to_x);
        kid->set_int(FlooNoc::REQ_DEST_Y, to_y);

        //Modify parent
        req->set_int(FlooNoc::REQ_PEND_KIDS, req->get_int(FlooNoc::REQ_PEND_KIDS) + 1);

        //Push to collective pending queue
        this->collective_generated_queues[momentum]->push(kid);

        //The next
        queue->pop();
    }
}

void Router::grant(vp::IoReq *req)
{
    // Now that the stalled request has been granted, we need to unstall the queue
    int queue = *(int *)req->arg_get(FlooNoc::REQ_QUEUE);
    this->stalled_queues[queue] = false;

    // And check in next cycle if another request can be sent
    this->fsm_event.enqueue(1);
}



void Router::get_next_router_pos(int dest_x, int dest_y, int &next_x, int &next_y)
{
    // XY routing algorithm
    int eff_dest_x = dest_x;

    if (dest_x == 0 && dest_y != this->y)
    {
        eff_dest_x = 1;
    } else
    if (dest_x == (this->noc->dim_x - 1) && dest_y != this->y)
    {
        eff_dest_x = this->noc->dim_x - 2;
    }

    if (eff_dest_x == this->x && dest_y == this->y)
    {
        next_x = this->x;
        next_y = this->y;
    }
    else if (eff_dest_x == this->x)
    {
        next_x = this->x;
        next_y = dest_y < this->y ? this->y - 1 : this->y + 1;
    }
    else
    {
        next_x = eff_dest_x < this->x ? this->x - 1 : this->x + 1;
        next_y = this->y;
    }

}



void Router::unstall_queue(int from_x, int from_y)
{
    // This gets called when an output queue gets unstalled because the denied request gets granted.
    // Just unstall the queue and trigger the fsm, in case we can now send a new request
    int queue = this->get_req_queue(from_x, from_y);
    this->stalled_queues[queue] = false;
    this->fsm_event.enqueue();
}



void Router::get_pos_from_queue(int queue, int &pos_x, int &pos_y)
{
    switch (queue)
    {
        // Inverse of get_req_queue: names denote travel direction, so a
        // right-going input came from the neighbor on the left.
        case FlooNoc::DIR_RIGHT: pos_x = this->x-1; pos_y = this->y; break;
        case FlooNoc::DIR_LEFT: pos_x = this->x+1; pos_y = this->y; break;
        case FlooNoc::DIR_UP: pos_x = this->x; pos_y = this->y-1; break;
        case FlooNoc::DIR_DOWN: pos_x = this->x; pos_y = this->y+1; break;
        case FlooNoc::DIR_LOCAL: pos_x = this->x; pos_y = this->y; break;
    }
}



int Router::get_req_queue(int from_x, int from_y)
{
    int queue_index = 0;
    if (from_x != this->x)
    {
        queue_index = from_x < this->x ? FlooNoc::DIR_RIGHT : FlooNoc::DIR_LEFT;
    }
    else if (from_y != this->y)
    {
        queue_index = from_y < this->y ? FlooNoc::DIR_UP : FlooNoc::DIR_DOWN;
    }
    else
    {
        queue_index = FlooNoc::DIR_LOCAL;
    }

    return queue_index;
}



void Router::reset(bool active)
{
    if (active)
    {
        this->current_queue = 0;
    }
}
