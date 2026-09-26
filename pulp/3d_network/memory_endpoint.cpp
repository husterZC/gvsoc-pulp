// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include "memory_endpoint.hpp"
#include <cstring>
#include <unordered_set>

class MemoryEndpoint : public vp::Component {
    struct Job : network3d::MemoryRequest {
        vp::IoReq backend;
        std::vector<uint8_t> data,strobes,shadow;
        uint64_t address=0;
        int64_t burst_id=0;
        void *initiator=nullptr;
        vp::IoReq *parent=nullptr;
        bool write=false,submitted=false,accepted=false,canceled=false;
    };
    network3d::MemoryEndpointTiming timing;
    vp::IoSlave input;
    vp::IoMaster output;
    vp::ClockEvent event;
    int width;
    uint64_t size;
    std::vector<uint8_t> storage;
    std::unordered_set<Job*> jobs;
    std::vector<Job*> backend_denied;
    Job *writer=nullptr,*read_job=nullptr,*write_job=nullptr;
    vp::IoReq *read_beat=nullptr,*write_ack=nullptr;
    bool read_denied=false,write_denied=false,reset_active=false,in_tick=false;

    void wake() { if (!reset_active && !in_tick) event.enable(); }
    Job *make_job(vp::IoReq *req) {
        auto job=new Job;
        job->address=req->get_addr(); job->burst_id=req->burst_id;
        job->initiator=req->initiator; job->parent=req->parent; job->write=req->get_is_write();
        job->backend.prepare(); job->backend.latency=req->latency; job->backend.duration=req->duration;
        job->backend.memcheck_data_id=req->memcheck_data_id;
        jobs.insert(job); return job;
    }
    void destroy(Job *job) { jobs.erase(job); delete job; }
    void complete(Job *job) {
        if (job->canceled) { destroy(job); return; }
        job->ready=true;
        job->ready_cycle=clock.get_cycles()+std::max<int64_t>(0,job->backend.get_full_latency());
        wake();
    }
    void access_storage(Job *job) {
        if (job->address>size || job->data.size()>size-job->address) {
            job->backend.set_resp_status(vp::IO_RESP_INVALID); return;
        }
        if (job->write) {
            for (size_t i=0;i<job->data.size();++i)
                if (job->strobes.empty() || job->strobes[i]) storage[job->address+i]=job->data[i];
        } else std::memcpy(job->data.data(),storage.data()+job->address,job->data.size());
    }
    void submit(Job *job) {
        if (!job->submitted) {
            auto &req=job->backend;
            req.set_addr(job->address); req.set_size(job->data.size()); req.set_data(job->data.data());
            req.set_opcode(job->write?vp::IoReqOpcode::WRITE:vp::IoReqOpcode::READ);
            req.set_strb(job->strobes.empty()?nullptr:job->strobes.data());
            req.memcheck_data=job->shadow.empty()?nullptr:job->shadow.data();
            req.second_data=nullptr; req.parent=job->parent; req.initiator=job;
            req.burst_id=job->burst_id; req.is_first=req.is_last=true;
            job->submitted=true;
        }
        auto status=vp::IO_REQ_DONE;
        if (output.is_bound()) status=output.req(&job->backend); else access_storage(job);
        job->accepted=status!=vp::IO_REQ_DENIED;
        if (!job->accepted) backend_denied.push_back(job);
        else if (status==vp::IO_REQ_DONE) complete(job);
    }
    static vp::IoReqStatus request(vp::Block *block,vp::IoReq *req) {
        auto &self=*static_cast<MemoryEndpoint*>(block);
        bool write=req->get_opcode()==vp::IoReqOpcode::WRITE;
        if ((req->get_opcode()!=vp::IoReqOpcode::READ && !write) || !req->get_size() ||
            (write && (req->get_size()>unsigned(self.width) || !req->get_data())) ||
            (!write && (!req->is_first || !req->is_last))) {
            req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE;
        }
        uint64_t now=self.clock.get_cycles();
        if (self.reset_active) {
            (write?self.write_denied:self.read_denied)=true; return vp::IO_REQ_DENIED;
        }
        if (!write) {
            if (!self.timing.can_read(now)) {
                self.read_denied=true; self.wake(); return vp::IO_REQ_DENIED;
            }
            auto job=self.make_job(req);
            job->data.resize(req->get_size()); job->beats=(req->get_size()+self.width-1)/self.width;
            if (req->memcheck_data) job->shadow.resize(req->get_size());
            self.timing.read(*job,now);
            req->free(); self.submit(job); self.wake(); return vp::IO_REQ_GRANTED;
        }
        if (!self.writer) {
            if (!req->is_first) { req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE; }
            if (self.timing.can_write(now)) {
                self.writer=self.make_job(req);
                self.timing.start_write(*self.writer,now);
            }
            // AW is accepted first. WREADY is registered, as in axi_sim_mem.
            self.write_denied=true; self.wake(); return vp::IO_REQ_DENIED;
        }
        auto job=self.writer;
        if (job->initiator!=req->initiator || job->burst_id!=req->burst_id ||
            !self.timing.can_write_beat(now)) {
            self.write_denied=true; self.wake(); return vp::IO_REQ_DENIED;
        }
        uint64_t offset=job->data.size(),bytes=req->get_size();
        if (req->is_first!=(offset==0) || req->get_addr()!=job->address+offset) {
            req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE;
        }
        job->data.insert(job->data.end(),req->get_data(),req->get_data()+bytes);
        if (req->get_strb() || !job->strobes.empty()) {
            job->strobes.resize(offset+bytes,1);
            if (req->get_strb()) std::memcpy(job->strobes.data()+offset,req->get_strb(),bytes);
        }
        if (req->memcheck_data || !job->shadow.empty()) {
            job->shadow.resize(offset+bytes);
            if (req->memcheck_data) std::memcpy(job->shadow.data()+offset,req->memcheck_data,bytes);
        }
        bool last=req->is_last;
        self.timing.write_beat(last,now); req->free();
        if (last) self.submit(job);
        self.wake(); return vp::IO_REQ_GRANTED;
    }
    vp::IoReq *response_beat(Job *job,int sequence,bool write) {
        auto req=vp::IoReqAllocator::get(write?0:width)->alloc(); req->prepare();
        uint64_t offset=write?0:uint64_t(sequence)*width;
        uint64_t bytes=write?job->data.size():std::min<uint64_t>(width,job->data.size()-offset);
        req->set_opcode(write?vp::IoReqOpcode::WRITE:vp::IoReqOpcode::READ);
        req->set_addr(job->address+offset); req->set_size(bytes);
        if (write) req->set_data(nullptr);
        else std::memcpy(req->get_data(),job->data.data()+offset,bytes);
        req->is_first=write || sequence==0; req->is_last=write || sequence+1==job->beats;
        req->burst_id=job->burst_id; req->initiator=job->initiator; req->parent=job->parent;
        req->second_data=nullptr; req->set_resp_status(job->backend.get_resp_status());
        req->memcheck_data_id=job->backend.memcheck_data_id;
        req->memcheck_data=job->shadow.empty()?nullptr:job->shadow.data()+offset;
        return req;
    }
    void send(bool write) {
        auto &beat=write?write_ack:read_beat;
        auto job=write?write_job:read_job;
        if (!beat) return;
        bool last=beat->is_last;
        if (input.resp(beat)==vp::IO_RESP_DENIED) return;
        // The consumer has freed the beat; do not inspect it after resp().
        beat=nullptr;
        if (write) { timing.write_accepted(clock.get_cycles()); writer=write_job=nullptr; }
        else { timing.read_accepted(clock.get_cycles()); read_job=nullptr; }
        if (last) destroy(job);
    }
    static void resp_retry(vp::Block *block,vp::IoRetryChannel channel) {
        auto &self=*static_cast<MemoryEndpoint*>(block);
        if (channel!=vp::IO_RETRY_WRITE) self.send(false);
        if (channel!=vp::IO_RETRY_READ) self.send(true);
        self.wake();
    }
    static void backend_retry(vp::Block *block,vp::IoRetryChannel channel) {
        auto &self=*static_cast<MemoryEndpoint*>(block);
        auto pending=std::move(self.backend_denied); self.backend_denied.clear();
        for (auto job:pending) {
            if (channel!=vp::IO_RETRY_ANY && int(channel)!=int(job->write)) self.backend_denied.push_back(job);
            else self.submit(job);
        }
    }
    static vp::IoRespAck backend_response(vp::Block *block,vp::IoReq *req) {
        static_cast<MemoryEndpoint*>(block)->complete(static_cast<Job*>(req->initiator));
        return vp::IO_RESP_ACCEPTED;
    }
    static void tick(vp::Block *block,vp::ClockEvent*) {
        auto &self=*static_cast<MemoryEndpoint*>(block);
        self.in_tick=true;
        // Enabled events run before the NI's delayed edge and avoid repeatedly
        // inserting thousands of endpoints into the engine's sorted event list.
        // Slots freed by the NI become reusable on the next cycle.
        if (self.jobs.empty() && !self.read_denied && !self.write_denied) self.event.disable();
        uint64_t now=self.clock.get_cycles();
        if (self.read_denied && self.timing.can_read(now)) {
            self.read_denied=false; self.input.retry(vp::IO_RETRY_READ);
        }
        if (self.write_denied && (self.timing.can_write(now) || self.timing.can_write_beat(now))) {
            self.write_denied=false; self.input.retry(vp::IO_RETRY_WRITE);
        }
        if (!self.read_beat) {
            auto response=self.timing.read_response(now);
            if (response.request) {
                self.read_job=static_cast<Job*>(response.request);
                self.read_beat=self.response_beat(self.read_job,response.sequence,false); self.send(false);
            }
        }
        if (!self.write_ack) if (auto job=self.timing.write_response(now)) {
            self.write_job=static_cast<Job*>(job);
            self.write_ack=self.response_beat(self.write_job,0,true); self.send(true);
        }
        self.in_tick=false;
    }
public:
    explicit MemoryEndpoint(vp::ComponentConf &conf) : vp::Component(conf),
        timing(get_js_config()->get_child_int("read_slots")),input(request,resp_retry),
        output(backend_retry,backend_response),event(this,tick) {
        auto js=get_js_config(); width=js->get_child_int("data_width")/8; size=js->get_child_int("size");
        if (width<1 || width>128 || (width&(width-1)) || !size)
            throw std::invalid_argument("invalid memory endpoint width/size");
        storage.resize(size);
        if (js->get_child_bool("benchmark_init")) {
            int id=js->get_child_int("endpoint_id");
            for (uint64_t a=0;a<size;++a) storage[a]=(id*17+a*13+(a>>8))&255;
        }
        new_slave_port("input",&input); new_master_port("output",&output);
    }
    void reset(bool active) override {
        reset_active=active;
        if (active) {
            event.disable(); timing.reset(); backend_denied.clear();
            if (read_beat) read_beat->free(); if (write_ack) write_ack->free();
            read_beat=write_ack=nullptr; writer=read_job=write_job=nullptr;
            read_denied=write_denied=false;
            auto pending=jobs;
            for (auto job:pending) {
                job->canceled=true;
                if (!job->accepted || job->ready) destroy(job);
            }
        } else if (read_denied || write_denied) wake();
    }
    ~MemoryEndpoint() {
        if (read_beat) read_beat->free(); if (write_ack) write_ack->free();
        for (auto job:jobs) delete job;
    }
};

extern "C" vp::Component *gv_new(vp::ComponentConf &conf) { return new MemoryEndpoint(conf); }
