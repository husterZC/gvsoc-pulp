// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <systemc.h>
#include <dlfcn.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <unordered_set>
#include <vector>

// Native completions form FIFO streams. GVSoC's DRAMSys library uses
// add_dram(..., GvsocMemspec*).
// Passing nullptr requests no geometry output, which the Python component
// already derives from the selected memspec.
struct GvsocMemspec;
class DramsysEndpoint : public vp::Component {
    struct Monitor : sc_core::sc_module {
        DramsysEndpoint &owner;
        Monitor(sc_core::sc_module_name name,DramsysEndpoint &owner): sc_module(name),owner(owner) {}
        void end_of_simulation() override { owner.stop(); }
    } monitor;
    struct Job {
        uint64_t address=0, base=0, size=0, sent=0, received=0, emitted=0;
        int64_t burst_id=0;
        void *initiator=nullptr;
        vp::IoReq *parent=nullptr;
        bool write=false;
        std::vector<uint8_t> data, strobes;
    };
    vp::IoSlave input;
    vp::ClockEvent event;
    void *library;
    int id, width, chunk;
    uint64_t size;
    bool read_denied=false, write_denied=false, in_tick=false, reported=false;
    Job *submitting=nullptr, *writer=nullptr;
    // AXI response arbitration rotates reads independently of the native
    // completion FIFO. A held read_beat always belongs to reads.front().
    std::deque<Job*> reads, receiving, writes;
    std::unordered_set<Job*> jobs;
    vp::IoReq *read_beat=nullptr, *write_ack=nullptr;
    int64_t last_read=-1, last_write=-1;
    uint64_t read_requests=0, write_requests=0, native_reads=0, native_writes=0;
    uint64_t read_bytes=0, write_bytes=0, response_denials=0, request_denials=0, peak_reads=0;

    int (*can_accept)(int), (*has_read)(int), (*has_write)(int), (*get_write)(int);
    void (*get_read)(int,uint64_t,void*);
    void (*put_byte)(int,int,int), (*put_strobe)(int,int,int);
    void (*send_req)(int,uint64_t,uint64_t,uint64_t,uint64_t);
    void (*preload)(int,uint64_t,int);
    void (*callbacks)(int,void*,void(*)(void*,int),void(*)(void*));

    template<typename T> void symbol(T &ptr,const char *name) {
        ptr=reinterpret_cast<T>(dlsym(library,name));
        if (!ptr) throw std::runtime_error(std::string("DRAMSys missing C ABI symbol: ")+name);
    }
    void wake() { if (!in_tick) event.enable(); }
    void sync_sc() {
        time.get_engine()->update(sc_core::sc_time_stamp().value());
        clock.get_engine()->sync();
    }
    Job *make_job(vp::IoReq *req) {
        auto job=new Job;
        job->address=req->get_addr(); job->base=job->address & ~uint64_t(chunk-1);
        job->burst_id=req->burst_id; job->initiator=req->initiator;
        job->parent=req->parent; job->write=req->get_is_write(); jobs.insert(job);
        return job;
    }
    void destroy(Job *job) { jobs.erase(job); delete job; }
    bool available() { return !submitting && !writer && can_accept(id); }
    bool read_ready(const Job *job) const {
        auto end=job->address-job->base+std::min<uint64_t>(job->size,job->emitted+width);
        return job->received>=end;
    }

    void pump() {
        // Exactly one BEGIN_REQ at a time; END_REQ releases DRAMSys capacity.
        // Native chunks can issue at the DRAM clock, independently of the
        // 1 GHz AXI port. No externally chosen outstanding-read limit.
        while (submitting && can_accept(id)) {
            auto job=submitting;
            auto offset=job->sent;
            if (job->write) for (int i=0;i<chunk;++i) {
                put_byte(id,job->data[offset+i],i);
                put_strobe(id,job->strobes[offset+i],i);
            }
            job->sent+=chunk;
            if (job->sent==job->data.size()) submitting=nullptr;
            (job->write?native_writes:native_reads)++;
            send_req(id,job->base+offset,chunk,job->write,job->write);
        }
    }
    static void capacity(void *instance) {
        auto &self=*static_cast<DramsysEndpoint*>(instance);
        self.sync_sc(); self.pump(); self.wake();
    }
    static void completion(void *instance,int write) {
        auto &self=*static_cast<DramsysEndpoint*>(instance);
        self.sync_sc();
        if (write) {
            while (self.has_write(self.id)) {
                self.get_write(self.id);
                auto it=std::find_if(self.writes.begin(),self.writes.end(),
                    [](Job *j) { return j->received<j->data.size(); });
                if (it==self.writes.end()) throw std::runtime_error("unexpected DRAMSys write response");
                (*it)->received+=self.chunk;
            }
        } else {
            int bytes=self.has_read(self.id);
            while (bytes) {
                if (self.receiving.empty()) throw std::runtime_error("unexpected DRAMSys read response");
                auto job=self.receiving.front();
                auto count=std::min<uint64_t>(bytes,job->data.size()-job->received);
                self.get_read(self.id,count,job->data.data()+job->received);
                job->received+=count; bytes-=count;
                if (job->received==job->data.size()) self.receiving.pop_front();
            }
        }
        self.wake();
    }
    static vp::IoReqStatus request(vp::Block *block,vp::IoReq *req) {
        auto &self=*static_cast<DramsysEndpoint*>(block);
        bool write=req->get_opcode()==vp::IoReqOpcode::WRITE;
        uint64_t addr=req->get_addr(), bytes=req->get_size();
        if ((!write && req->get_opcode()!=vp::IoReqOpcode::READ) || !bytes ||
            addr>self.size || bytes>self.size-addr ||
            (write && (bytes>unsigned(self.width) || !req->get_data())) ||
            (!write && (!req->is_first || !req->is_last))) {
            req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE;
        }
        if (!write) {
            if (!self.available()) {
                self.read_denied=true; self.request_denials++; return vp::IO_REQ_DENIED;
            }
            auto job=self.make_job(req); job->size=bytes;
            auto length=(addr-job->base+bytes+self.chunk-1)&~uint64_t(self.chunk-1);
            job->data.resize(length);
            self.reads.push_back(job); self.receiving.push_back(job); self.submitting=job;
            self.read_requests++; self.peak_reads=std::max<uint64_t>(self.peak_reads,self.reads.size());
            req->free(); self.pump(); return vp::IO_REQ_GRANTED;
        }
        if (!self.writer) {
            if (!req->is_first) { req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE; }
            if (!self.available()) {
                self.write_denied=true; self.request_denials++; return vp::IO_REQ_DENIED;
            }
            self.writer=self.make_job(req); self.write_requests++;
        }
        auto job=self.writer;
        if (req->initiator!=job->initiator || req->burst_id!=job->burst_id) {
            self.write_denied=true; self.request_denials++; return vp::IO_REQ_DENIED;
        }
        if (addr!=job->address+job->size || req->is_first!=(job->size==0)) {
            req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE;
        }
        auto pos=addr-job->base, length=(pos+bytes+self.chunk-1)&~uint64_t(self.chunk-1);
        job->data.resize(length); job->strobes.resize(length,0);
        std::memcpy(job->data.data()+pos,req->get_data(),bytes);
        for (uint64_t i=0;i<bytes;++i) job->strobes[pos+i]=!req->get_strb() || req->get_strb()[i];
        job->size+=bytes;
        bool last=req->is_last; req->free();
        if (last) {
            self.writer=nullptr; self.writes.push_back(job); self.submitting=job; self.pump();
        }
        return vp::IO_REQ_GRANTED;
    }
    vp::IoReq *beat(Job *job) {
        auto req=vp::IoReqAllocator::get(job->write?0:width)->alloc(); req->prepare();
        auto bytes=job->write?job->size:std::min<uint64_t>(width,job->size-job->emitted);
        req->set_opcode(job->write?vp::IoReqOpcode::WRITE:vp::IoReqOpcode::READ);
        req->set_addr(job->address+job->emitted); req->set_size(bytes);
        if (job->write) req->set_data(nullptr);
        else std::memcpy(req->get_data(),job->data.data()+job->address-job->base+job->emitted,bytes);
        req->initiator=job->initiator; req->parent=job->parent; req->burst_id=job->burst_id;
        req->is_first=job->emitted==0; req->is_last=job->write || job->emitted+bytes==job->size;
        req->set_resp_status(vp::IO_RESP_OK); return req;
    }
    void respond(bool write) {
        auto &req=write?write_ack:read_beat;
        if (!req) return;
        auto job=(write?writes:reads).front();
        uint64_t bytes=req->get_size(); bool last=req->is_last;
        if (input.resp(req)==vp::IO_RESP_DENIED) { response_denials++; return; }
        req=nullptr; (write?last_write:last_read)=clock.get_cycles();
        (write?write_bytes:read_bytes)+=bytes; job->emitted+=bytes;
        if (last) { (write?writes:reads).pop_front(); destroy(job); }
        else if (!write) { reads.pop_front(); reads.push_back(job); }
    }
    static void retry(vp::Block *block,vp::IoRetryChannel channel) {
        auto &self=*static_cast<DramsysEndpoint*>(block);
        if (channel!=vp::IO_RETRY_WRITE) self.respond(false);
        if (channel!=vp::IO_RETRY_READ) self.respond(true);
        self.wake();
    }
    static void tick(vp::Block *block,vp::ClockEvent*) {
        auto &self=*static_cast<DramsysEndpoint*>(block); self.in_tick=true;
        if (self.available()) {
            if (self.read_denied) { self.read_denied=false; self.input.retry(vp::IO_RETRY_READ); }
            if (self.write_denied && self.available()) {
                self.write_denied=false; self.input.retry(vp::IO_RETRY_WRITE);
            }
        }
        bool ready=false;
        if (!self.read_beat && !self.reads.empty()) {
            // Interleave ready beats across independent IO_v2 transactions.
            // Do not change the selected job while its beat is backpressured.
            auto it=std::find_if(self.reads.begin(),self.reads.end(),
                [&](Job *job) { return self.read_ready(job); });
            if (it!=self.reads.end()) {
                std::rotate(self.reads.begin(),it,self.reads.end());
                ready=true;
                if (self.last_read<self.clock.get_cycles()) {
                    self.read_beat=self.beat(self.reads.front()); self.respond(false);
                }
            }
        }
        if (!self.write_ack && !self.writes.empty()) {
            auto job=self.writes.front();
            if (job->received==job->data.size()) {
                ready=true;
                if (self.last_write<self.clock.get_cycles()) { self.write_ack=self.beat(job); self.respond(true); }
            }
        }
        if (!ready) self.event.disable(); // Completion/retry/capacity callbacks wake it.
        self.in_tick=false;
    }
public:
    explicit DramsysEndpoint(vp::ComponentConf &conf): vp::Component(conf),
        monitor(sc_core::sc_gen_unique_name("endpoint_monitor"),*this),
        input(request,retry),event(this,tick) {
        auto js=get_js_config(); width=js->get_child_int("data_width")/8;
        chunk=js->get_child_int("burst_bytes"); size=js->get_child_int("size");
        if (!chunk || chunk>2048 || (chunk&(chunk-1))) throw std::invalid_argument("invalid native DRAM burst size");
        if (sc_core::sc_get_time_resolution()!=sc_core::sc_time(1,sc_core::SC_PS))
            throw std::runtime_error("DRAMSys endpoint requires 1 ps SystemC resolution");
        // Match memory.dramsys: unused optional library facilities (such as
        // ELF loading) need not be resolved for a request/response endpoint.
        library=dlopen(js->get_child_str("library").c_str(),RTLD_LAZY|RTLD_GLOBAL);
        if (!library) throw std::runtime_error(dlerror());
        symbol(can_accept,"dram_can_accept_req");
        symbol(has_read,"dram_has_read_rsp"); symbol(has_write,"dram_has_write_rsp");
        symbol(get_read,"dram_get_read_rsp"); symbol(get_write,"dram_get_write_rsp");
        symbol(put_byte,"dram_write_buffer"); symbol(put_strobe,"dram_write_strobe");
        symbol(send_req,"dram_send_req"); symbol(preload,"dram_preload_byte");
        symbol(callbacks,"dram_register_async_callback");
        auto resources=js->get_child_str("resources"), config=js->get_child_str("config");
        int (*add_dram)(char*,char*,GvsocMemspec*);
        symbol(add_dram,"add_dram");
        id=add_dram(const_cast<char*>(resources.c_str()),const_cast<char*>(config.c_str()),nullptr);
        callbacks(id,this,completion,capacity);
        if (js->get_child_bool("benchmark_init")) {
            int endpoint=js->get_child_int("endpoint_id");
            for (uint64_t a=0;a<uint64_t(js->get_child_int("init_size"));++a)
                preload(id,a,(17*endpoint+13*a+(a>>8))&255);
        }
        new_slave_port("input",&input);
    }
    void reset(bool active) override {
        if (active && !jobs.empty()) throw std::runtime_error("Reset with active DRAMSys traffic is unsupported");
    }
    void stop() override {
        if (reported) return;
        reported=true;
        printf("DRAMSYS_ENDPOINT_RESULT {\"channel\":%d,\"read_requests\":%lu,\"write_requests\":%lu,"
               "\"native_reads\":%lu,\"native_writes\":%lu,\"read_bytes\":%lu,\"write_bytes\":%lu,"
               "\"request_denials\":%lu,\"response_denials\":%lu,\"peak_reads\":%lu,\"pending\":%zu}\n",
               id,read_requests,write_requests,native_reads,native_writes,read_bytes,write_bytes,
               request_denials,response_denials,peak_reads,jobs.size());
    }
    ~DramsysEndpoint() {
        if (read_beat) read_beat->free(); if (write_ack) write_ack->free();
        for (auto job:jobs) delete job;
        // The bundled C ABI has no close_dram. SystemC modules live until
        // process exit; never unload code still referenced by its kernel.
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &conf) { return new DramsysEndpoint(conf); }
