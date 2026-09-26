// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include "i3d.hpp"
#include <memory>
#include <queue>
#include <unordered_set>
#include <cstring>
#include <array>

class Interconnect3d : public vp::Component {
    struct Transfer {
        network3d::Transaction tx;
        vp::IoReq *original=nullptr;
        vp::IoReq child;
        vp::IoReq *beat=nullptr;
        std::vector<uint8_t> data,strobes,shadow;
        bool submitted=false, accepted=false, response_blocked=false, responded=false;
        bool canceled=false, memory_started=false, memory_done=false, write_aborted=false;
        int submitted_sequence=-2;
        uint64_t read_offset=0;
    };
    struct MemoryResponse {
        vp::IoReq *req=nullptr;
        Transfer *transfer=nullptr;
        int64_t due=0;
        uint64_t offset=0;
        bool offered=false, accepting=false, owned=false;
    };
    struct Ready {
        int64_t cycle; Transfer *transfer;
        bool operator<(const Ready &b) const { return cycle>b.cycle; }
    };
    network3d::I3dConfig cfg;
    std::unique_ptr<network3d::Network> net;
    std::unique_ptr<network3d::I3d> i3d;
    std::vector<std::unique_ptr<vp::IoSlave>> inputs;
    std::vector<std::unique_ptr<vp::IoMaster>> outputs;
    std::vector<bool> denied;
    std::vector<std::vector<Transfer*>> downstream_denied, upstream_denied;
    std::vector<std::array<MemoryResponse,2>> memory_responses;
    std::vector<MemoryResponse> waiting_memory_responses;
    std::unordered_set<Transfer*> live;
    std::vector<Transfer*> garbage;
    std::priority_queue<Ready> ready;
    vp::ClockEvent event;
    vp::Trace trace;
    int n=0,dst_bits=0;
    size_t active_transfers=0;
    bool in_tick=false,reset_active=false;

    void wake() { if (!reset_active && !event.is_enqueued() && !in_tick) event.enqueue(0); }
    bool can_offer(int s) { return i3d ? i3d->can_offer(s) : net->can_offer(s); }
    void retire(Transfer *t) { live.erase(t); if (!t->canceled) --active_transfers; garbage.push_back(t); }
    void collect() { for (auto t:garbage) delete t; garbage.clear(); }
    void memory_complete(Transfer *t) {
        if (t->canceled) { retire(t); return; }
        t->memory_done=true;
        uint64_t delay=std::max<int64_t>(0,t->child.get_full_latency());
        ready.push({clock.get_cycles()+int64_t(delay),t});
        wake();
    }
    bool submit(Transfer *t) {
        if (t->submitted) return t->accepted;
        t->submitted=true;
        auto &port=outputs[t->tx.destination];
        auto status=vp::IO_REQ_DONE;
        if (port->is_bound()) status=port->req(&t->child);
        else t->child.set_resp_status(vp::IO_RESP_INVALID);
        t->accepted=status!=vp::IO_REQ_DENIED;
        t->memory_started=t->accepted;
        if (!t->accepted) downstream_denied[t->tx.destination].push_back(t);
        else if (status==vp::IO_REQ_DONE) memory_complete(t);
        return t->accepted;
    }
    void queue_response(Transfer *t,vp::IoReq *req,bool owned) {
        auto &r=memory_responses[t->tx.destination][t->tx.write];
        // DONE cannot be backpressured. Keep completed packets until the NI
        // can serialize them; this is transport buffering, not memory service.
        // A response retry may also resend other held responses on the channel.
        if (r.req==req) return;
        for (const auto &pending:waiting_memory_responses) if (pending.req==req) return;
        MemoryResponse response;
        response.req=req; response.transfer=t; response.owned=owned;
        response.due=clock.get_cycles()+std::max<int64_t>(0,req->get_full_latency());
        if (r.req) waiting_memory_responses.push_back(response); else r=response;
        wake();
    }
    bool submit_beat(Transfer *t,int sequence) {
        if (t->submitted && t->submitted_sequence==sequence) return t->accepted;
        if (t->write_aborted) {
            if (sequence+1==t->tx.beats) { queue_response(t,t->beat,true); t->beat=nullptr; }
            return true;
        }
        if (!t->beat) {
            auto req=vp::IoReqAllocator::get(0)->alloc(); req->prepare();
            uint64_t offset=t->tx.write?uint64_t(sequence)*(cfg.axi_data_width/8):0;
            req->set_addr(t->tx.local_address+offset);
            req->set_size(t->tx.write?std::min<uint64_t>(cfg.axi_data_width/8,t->tx.size-offset):t->tx.size);
            req->set_data(t->tx.write?t->data.data()+offset:nullptr);
            req->set_opcode(t->tx.write?vp::IoReqOpcode::WRITE:vp::IoReqOpcode::READ);
            req->is_first=!t->tx.write || sequence==0;
            req->is_last=!t->tx.write || sequence+1==t->tx.beats;
            req->burst_id=t->original->burst_id; req->initiator=t; req->parent=t->original;
            req->second_data=nullptr;
            req->set_strb(t->strobes.empty()?nullptr:t->strobes.data()+offset);
            req->memcheck_data=t->shadow.empty()?nullptr:t->shadow.data()+offset;
            req->memcheck_data_id=t->child.memcheck_data_id;
            if (req->is_first) { req->latency=t->child.latency; req->duration=t->child.duration; }
            t->beat=req;
        }
        t->submitted=true; t->submitted_sequence=sequence;
        auto req=t->beat; auto &port=outputs[t->tx.destination];
        vp::IoReqStatus status;
        if (port->is_bound()) status=port->req(req);
        else { req->set_resp_status(vp::IO_RESP_INVALID); status=vp::IO_REQ_DONE; }
        t->accepted=status!=vp::IO_REQ_DENIED;
        if (!t->accepted) downstream_denied[t->tx.destination].push_back(t);
        else {
            t->memory_started=true;
            if (status==vp::IO_REQ_DONE) {
                if (t->tx.write && sequence+1<t->tx.beats) {
                    if (req->get_resp_status()!=vp::IO_RESP_INVALID)
                        throw std::logic_error("non-last W returned DONE without error");
                    t->write_aborted=true; return true;
                }
                // Inline completions are packetized by the NI. Read data may
                // be absent for a decode error; the final status still returns.
                queue_response(t,req,true);
            }
            t->beat=nullptr; // GRANTED transferred ownership to the slave.
        }
        return t->accepted;
    }
    void offer_memory_responses() {
        for (size_t i=0;i<waiting_memory_responses.size();) {
            auto &pending=waiting_memory_responses[i]; auto t=pending.transfer;
            auto &r=memory_responses[t->tx.destination][t->tx.write];
            if (r.req) ++i;
            else { r=pending; waiting_memory_responses.erase(waiting_memory_responses.begin()+i); }
        }
        for (auto &channels:memory_responses) for (auto &r:channels)
            if (r.req && !r.offered && r.due<=clock.get_cycles()) {
                auto t=r.transfer; r.offered=true;
                i3d->memory_response(t->tx,t->tx.write?0:int(t->read_offset/(cfg.axi_data_width/8)));
            }
    }
    void consume_memory_response(network3d::Transaction &tx) {
        auto &r=memory_responses[tx.destination][tx.write];
        auto t=r.transfer; auto req=r.req;
        if (req->get_resp_status()!=vp::IO_RESP_OK) t->child.set_resp_status(req->get_resp_status());
        t->child.memcheck_data_id=req->memcheck_data_id;
        if (!tx.write) {
            uint64_t bytes=std::min<uint64_t>(cfg.axi_data_width/8,tx.size-t->read_offset);
            if (req->get_data() && req->get_resp_status()==vp::IO_RESP_OK) {
                if (r.offset+bytes>req->get_size()) throw std::logic_error("short IO_v2 read response");
                std::memcpy(t->data.data()+t->read_offset,req->get_data()+r.offset,bytes);
                if (!t->shadow.empty() && req->memcheck_data)
                    std::memcpy(t->shadow.data()+t->read_offset,req->memcheck_data+r.offset,bytes);
            }
            t->read_offset+=bytes; r.offset+=bytes;
            if (r.offset<req->get_size() && t->read_offset<tx.size) {
                r.offered=false; return; // A whole-packet response still needs NI serialization.
            }
        }
        t->memory_done=tx.write || t->read_offset==tx.size;
        if (r.owned) { req->free(); r={}; }
        else {
            r.accepting=true;
            outputs[tx.destination]->resp_retry(tx.write?vp::IO_RETRY_WRITE:vp::IO_RETRY_READ);
            if (r.req==req && r.accepting)
                throw std::logic_error("IO_v2 response retry must resend synchronously");
        }
    }
    bool respond(Transfer *t) {
        if (t->responded) return true;
        if (t->response_blocked) return false;
        auto req=t->original;
        req->set_resp_status(t->tx.error ? vp::IO_RESP_INVALID : t->child.get_resp_status());
        // All endpoint annotations have now been consumed by scheduled time.
        req->latency=req->duration=0;
        req->memcheck_data_id=t->child.memcheck_data_id;
        if (!t->tx.error && !t->tx.write && req->get_data()) {
            std::memcpy(req->get_data(),t->data.data(),t->data.size());
            if (req->memcheck_data && !t->shadow.empty())
                std::memcpy(req->memcheck_data,t->shadow.data(),t->shadow.size());
        }
        if (inputs[t->tx.source]->resp(req)==vp::IO_RESP_DENIED) {
            t->response_blocked=true;
            upstream_denied[t->tx.source].push_back(t);
            return false;
        }
        t->responded=true;
        // The kernel may still reference the transfer until this step ends.
        if (!i3d) retire(t);
        return true;
    }
    static vp::IoReqStatus request(vp::Block *block,vp::IoReq *req,int source) {
        auto &self=*static_cast<Interconnect3d*>(block);
        if (req->get_opcode()!=vp::IoReqOpcode::READ && req->get_opcode()!=vp::IoReqOpcode::WRITE) {
            req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE;
        }
        if (!req->get_size() || !req->get_data() || !req->is_first || !req->is_last ||
            (!self.i3d && req->get_size()>uint64_t((self.cfg.network.data_width+7)/8))) {
            req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE;
        }
        int destination=int(req->get_addr() & ((uint64_t(1)<<self.dst_bits)-1));
        if (!self.i3d && destination>=self.n) {
            req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE;
        }
        if (self.reset_active || !self.can_offer(source)) {
            self.denied[source]=true; self.wake(); return vp::IO_REQ_DENIED;
        }
        auto t=new Transfer;
        t->original=req; t->tx.opaque=t; t->tx.id=reinterpret_cast<uintptr_t>(t);
        t->tx.source=source; t->tx.destination=destination;
        t->tx.address=req->get_addr(); t->tx.size=req->get_size(); t->tx.write=req->get_is_write();
        if (self.i3d) self.i3d->offer(source,t->tx);
        else {
            network3d::Packet p; p.id=t->tx.id; p.src=source; p.dst=destination;
            self.net->offer(source,p);
        }
        // The parent belongs to the caller. Preserve its address and identity,
        // including its initiator; only our child carries private routing state.
        t->child.set_addr(self.i3d?t->tx.local_address:req->get_addr());
        if (!t->tx.error) {
            t->data.resize(req->get_size());
            if (req->get_is_write()) std::memcpy(t->data.data(),req->get_data(),t->data.size());
            if (req->get_strb()) t->strobes.assign(req->get_strb(),req->get_strb()+req->get_size());
            if (req->memcheck_data) t->shadow.assign(req->memcheck_data,req->memcheck_data+req->get_size());
        }
        t->child.set_size(req->get_size()); t->child.set_data(t->data.data());
        t->child.set_opcode(req->get_opcode());
        t->child.set_strb(t->strobes.empty()?nullptr:t->strobes.data());
        t->child.memcheck_data=t->shadow.empty()?nullptr:t->shadow.data();
        t->child.memcheck_data_id=req->memcheck_data_id;
        t->child.second_data=nullptr; t->child.parent=req; t->child.initiator=t;
        t->child.burst_id=req->burst_id;
        t->child.latency=req->latency; t->child.duration=req->duration;
        self.live.insert(t); ++self.active_transfers; self.wake(); return vp::IO_REQ_GRANTED;
    }
    static void retry(vp::Block *block,int destination,vp::IoRetryChannel channel) {
        auto &self=*static_cast<Interconnect3d*>(block);
        auto pending=std::move(self.downstream_denied[destination]);
        self.downstream_denied[destination].clear();
        for (auto t:pending) {
            if (channel!=vp::IO_RETRY_ANY && int(channel)!=int(t->tx.write))
                self.downstream_denied[destination].push_back(t);
            else {
                t->submitted=false;
                if (self.i3d) self.submit_beat(t,t->submitted_sequence); else self.submit(t);
            }
        }
        self.wake();
    }
    static vp::IoRespAck response(vp::Block *block,vp::IoReq *req,int destination) {
        auto &self=*static_cast<Interconnect3d*>(block);
        auto t=static_cast<Transfer*>(req->initiator);
        if (!self.i3d) { self.memory_complete(t); return vp::IO_RESP_ACCEPTED; }
        if (t->canceled) {
            bool last=req->is_last; req->free();
            if (last) self.retire(t);
            return vp::IO_RESP_ACCEPTED;
        }
        auto &r=self.memory_responses[destination][t->tx.write];
        if (r.accepting && r.req==req) { req->free(); r={}; return vp::IO_RESP_ACCEPTED; }
        self.queue_response(t,req,false);
        return vp::IO_RESP_DENIED;
    }
    static void resp_retry(vp::Block *block,int source,vp::IoRetryChannel channel) {
        auto &self=*static_cast<Interconnect3d*>(block);
        auto pending=std::move(self.upstream_denied[source]);
        self.upstream_denied[source].clear();
        for (auto t:pending) {
            if (channel!=vp::IO_RETRY_ANY && int(channel)!=int(t->tx.write))
                self.upstream_denied[source].push_back(t);
            else { t->response_blocked=false; self.respond(t); }
        }
        self.wake();
    }
    static void tick(vp::Block *block,vp::ClockEvent*) {
        auto &self=*static_cast<Interconnect3d*>(block);
        self.in_tick=true; self.collect();
        for (int s=0;s<self.n;++s) if (self.denied[s] && self.can_offer(s)) {
            self.denied[s]=false;
            self.inputs[s]->retry(vp::IO_RETRY_ANY);
        }
        if (self.i3d) { self.offer_memory_responses(); self.i3d->step(); }
        else {
            for (int d=0;d<self.n;++d) if (auto p=self.net->peek(d)) {
                auto t=reinterpret_cast<Transfer*>(p->id);
                if (self.submit(t)) self.net->take(d);
            }
            self.net->step();
            while (!self.ready.empty() && self.ready.top().cycle<=self.clock.get_cycles()) {
                auto t=self.ready.top().transfer; self.ready.pop(); self.respond(t);
            }
        }
        self.in_tick=false;
        if (self.active_transfers || !self.garbage.empty()) self.event.enqueue(1);
    }
public:
    explicit Interconnect3d(vp::ComponentConf &conf) : vp::Component(conf),event(this,tick) {
        traces.new_trace("trace",&trace,vp::DEBUG);
        auto js=get_js_config();
#define READ(field) cfg.field=js->get_child_int(#field)
#define NET(field) cfg.network.field=js->get_child_int(#field)
        NET(fabric); NET(num_x); NET(num_y); NET(num_levels); NET(routing_mode);
        NET(io_spill); NET(data_width); NET(addr_width);
        READ(source_contexts); READ(memory_contexts); READ(max_burst_beats);
        READ(axi_addr_width); READ(axi_data_width); READ(axi_id_width); READ(axi_len_width);
        READ(memory_base); READ(interleave_bytes); READ(memory_bytes);
#undef READ
#undef NET
        if (js->get_child_bool("i3d")) {
            i3d=std::make_unique<network3d::I3d>(cfg); n=i3d->size();
            i3d->issue=[this](network3d::Transaction &tx,int sequence) {
                return submit_beat(static_cast<Transfer*>(tx.opaque),sequence);
            };
            i3d->memory_response_accepted=[this](network3d::Transaction &tx,int) { consume_memory_response(tx); };
            i3d->respond=[this](network3d::Transaction &tx) {
                auto t=static_cast<Transfer*>(tx.opaque);
                if (!respond(t)) return false;
                retire(t); return true;
            };
        } else { net=std::make_unique<network3d::Network>(cfg.network); n=net->size(); }
        for (int v=n-1;v;v>>=1) ++dst_bits;
        denied.resize(n); downstream_denied.resize(n); upstream_denied.resize(n);
        memory_responses.resize(n);
        for (int i=0;i<n;++i) {
            inputs.emplace_back(new vp::IoSlave(i,request,resp_retry));
            outputs.emplace_back(new vp::IoMaster(i,retry,response));
            new_slave_port("input_"+std::to_string(i),inputs.back().get());
            new_master_port("output_"+std::to_string(i),outputs.back().get());
        }
    }
    void reset(bool active) override {
        reset_active=active;
        if (active) {
            event.cancel(); collect();
            if (i3d) i3d->reset(); else net->reset();
            ready={};
            active_transfers=0; std::fill(denied.begin(),denied.end(),false);
            for (auto t:live) {
                // Keep granted children alive until a late downstream response;
                // coordinated reset may suppress it, in which case teardown frees it.
                t->canceled=true;
                if (t->beat) { t->beat->free(); t->beat=nullptr; }
                if (!t->memory_started || t->memory_done) garbage.push_back(t);
            }
            for (auto t:garbage) live.erase(t);
            for (auto &v:downstream_denied) v.clear();
            for (auto &v:upstream_denied) v.clear();
            for (auto &channels:memory_responses) for (auto &r:channels) {
                if (r.owned && r.req) r.req->free();
                r={};
            }
            for (auto &r:waiting_memory_responses) if (r.owned) r.req->free();
            waiting_memory_responses.clear();
        } else {
            collect();
            if (std::find(denied.begin(),denied.end(),true)!=denied.end()) wake();
        }
    }
    ~Interconnect3d() {
        collect();
        for (auto &channels:memory_responses) for (auto &r:channels) if (r.owned && r.req) r.req->free();
        for (auto &r:waiting_memory_responses) if (r.owned) r.req->free();
        for (auto t:live) { if (t->beat) t->beat->free(); delete t; }
    }
};

extern "C" vp::Component *gv_new(vp::ComponentConf &conf) { return new Interconnect3d(conf); }
