// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <chrono>
#include <cstdio>
#include <memory>
#include <unordered_map>

static uint32_t philox(uint32_t seed,uint32_t source,uint32_t ordinal) {
    uint32_t a=ordinal,b=source,c=0,d=0,k0=seed,k1=0;
    for (int i=0;i<10;++i) {
        uint64_t p=uint64_t(a)*0xd2511f53u,q=uint64_t(c)*0xcd9e8d57u;
        a=(q>>32)^b^k0; b=uint32_t(q); c=(p>>32)^d^k1; d=uint32_t(p);
        k0+=0x9e3779b9u; k1+=0xbb67ae85u;
    }
    return a;
}
class Driver : public vp::Component {
    struct Request {
        vp::IoReq req;
        std::vector<uint8_t> bytes,strobes,memory_data;
        uint64_t id;
        uint64_t address=0,local=0;
        int source,destination;
        bool invalid=false;
        int64_t ready_cycle=0,denied_until=0,blocked_until=0;
    };
    struct Delayed { vp::IoReq *req; int destination; int64_t cycle; };
    std::vector<std::unique_ptr<vp::IoMaster>> out;
    std::vector<std::unique_ptr<vp::IoSlave>> in;
    std::vector<Request*> pending;
    std::vector<bool> denied,seen,mem_seen;
    std::vector<uint64_t> sent;
    std::vector<uint64_t> completed;
    std::vector<std::vector<uint8_t>> storage;
    std::unordered_map<vp::IoReq*,std::unique_ptr<Request>> live;
    std::vector<Delayed> delayed;
    std::vector<int64_t> request_retry,response_retry;
    std::vector<bool> target_denied,response_denied;
    vp::ClockEvent event;
    bool soc,sparse,stress,functional,backing;
    int n,count,repeats,groups,burst,offset,bytes,endpoint;
    uint64_t interleave_bytes,memory_bytes;
    int64_t progress_cycles;
    uint32_t seed;
    uint64_t total,done=0,accepted=0,peak=0,checksum=0xcbf29ce484222325ULL;
    int64_t start=0,inject=0;
    std::chrono::steady_clock::time_point wall;
    uint64_t address(int destination,uint64_t local) const {
        return (local/interleave_bytes*n+destination)*interleave_bytes+local%interleave_bytes;
    }
    int destination(int s,int k) const {
        if (functional) return s;
        if (soc) return (s+k)%n;
        return sparse ? philox(seed,s,k/repeats)%n : (s+k/repeats)%n;
    }
    void fail(const char *reason) { fprintf(stderr,"NETWORK3D_FAIL %s\n",reason); time.get_engine()->quit(1); }
    void issue(int s) {
        if (sent[s]>=unsigned(count)) return;
        if (functional && completed[s]!=sent[s]) return;
        auto &r=pending[s];
        if (!r) {
            auto item=std::make_unique<Request>(); r=item.get();
            r->id=uint64_t(s)*count+sent[s]; r->source=s; r->destination=destination(s,sent[s]);
            r->bytes.resize(bytes*(soc?burst:1));
            r->req.set_addr(soc ? address(r->destination,offset) : r->destination);
            r->req.set_size(r->bytes.size()); r->req.set_data(r->bytes.data());
            r->req.set_is_write(false); r->req.initiator=r; r->req.parent=nullptr;
            r->req.burst_id=r->id;
            r->local=soc?offset:r->destination;
            if (functional) {
                int k=sent[s];
                uint64_t size=bytes*3,local=64;
                if (k==2 || k==3) { local=memory_bytes-bytes; size=bytes; }
                if (k==4) { local=memory_bytes; r->invalid=true; }
                if (k==5) { local=interleave_bytes-1; size=2; r->invalid=true; }
                if (k==6) { size=0; r->invalid=true; }
                if (k==7) r->invalid=true;
                if (k==8) { size=bytes*256+1; local=0; r->invalid=true; }
                if (k==9) { size=3; local=65; }
                r->bytes.resize(size); r->strobes.assign(size,1);
                r->local=local;
                r->req.set_addr(k==4 ? uint64_t(n)*memory_bytes : address(s,local));
                r->req.set_size(size); r->req.set_data(r->bytes.data());
                r->req.set_is_write(k==0 || k==2);
                if (k==7) r->req.set_opcode(vp::IoReqOpcode::SWAP);
                if (k==0 || k==2) {
                    for (uint64_t i=0;i<size;++i) {
                        r->bytes[i]=(i*7+s)&255;
                        if (k==0) r->strobes[i]=i%2;
                    }
                    r->req.set_strb(r->strobes.data());
                }
            }
            r->address=r->req.get_addr();
            if (soc && endpoint==2) r->invalid=true;
            live.emplace(&r->req,std::move(item));
        }
        auto status=out[s]->req(&r->req);
        denied[s]=status==vp::IO_REQ_DENIED;
        if (!denied[s]) {
            ++sent[s]; ++accepted; inject=clock.get_cycles();
            if (status==vp::IO_REQ_DONE) response(this,&r->req,s);
            r=nullptr;
        }
    }
    static void retry(vp::Block *b,int s,vp::IoRetryChannel) {
        auto self=static_cast<Driver*>(b);
        if (self->denied[s]) { self->denied[s]=false; self->issue(s); }
    }
    static vp::IoRespAck response(vp::Block *b,vp::IoReq *req,int s) {
        auto &self=*static_cast<Driver*>(b);
        auto it=self.live.find(req);
        if (it==self.live.end()) { self.fail("unknown response identity"); return vp::IO_RESP_ACCEPTED; }
        auto &r=*it->second;
        if (self.clock.get_cycles()<r.blocked_until || self.clock.get_cycles()<r.ready_cycle)
            self.fail("response before retry or annotated completion time");
        if (self.stress && !self.response_denied[s] && r.id%3==0) {
            self.response_denied[s]=true; self.response_retry[s]=self.clock.get_cycles()+3;
            r.blocked_until=self.response_retry[s];
            return vp::IO_RESP_DENIED;
        }
        if (r.source!=s || req->initiator!=&r || self.seen[r.id] ||
            ((self.backing || !self.soc) && self.mem_seen[r.id]==r.invalid) || req->get_resp_status()!=
            (r.invalid ? vp::IO_RESP_INVALID : vp::IO_RESP_OK) || req->get_addr()!=r.address)
            self.fail("response address/status/identity scoreboard");
        if (!self.backing && self.functional && !r.invalid && req->get_is_write()) {
            for (size_t i=0;i<r.bytes.size();++i)
                if (r.strobes.empty() || r.strobes[i]) self.storage[r.destination][r.local+i]=r.bytes[i];
        }
        for (size_t i=0;!r.invalid && !req->get_is_write() && i<r.bytes.size();++i) {
            uint64_t a=self.soc?self.offset+i:i;
            uint8_t expected=self.functional ? self.storage[r.destination][r.local+i]
                : (17*r.destination+13*a+(a>>8))&255;
            if (r.bytes[i]!=expected) { self.fail("response data scoreboard"); break; }
        }
        self.seen[r.id]=true; ++self.done; ++self.completed[s]; self.live.erase(it);
        return vp::IO_RESP_ACCEPTED;
    }
    static vp::IoReqStatus memory(vp::Block *b,vp::IoReq *req,int d) {
        auto &self=*static_cast<Driver*>(b);
        auto it=self.live.find(req->parent);
        if (it==self.live.end()) { self.fail("unknown downstream parent"); return vp::IO_REQ_DONE; }
        auto &r=*it->second;
        if (self.soc && self.endpoint==0 && !req->get_data()) {
            // A legal inline full-packet read response; its buffer remains
            // alive until the original transaction completes through the NoC.
            r.memory_data.resize(req->get_size()); req->set_data(r.memory_data.data());
        }
        if (self.clock.get_cycles()<r.denied_until) self.fail("request resubmitted before retry");
        if (self.stress && !self.target_denied[d] && r.id%2==0) {
            self.target_denied[d]=true; self.request_retry[d]=self.clock.get_cycles()+2;
            r.denied_until=self.request_retry[d];
            return vp::IO_REQ_DENIED;
        }
        if (r.invalid || r.destination!=d || self.mem_seen[r.id] || req->get_addr()!=r.local ||
            bool(req->get_strb())!=bool(r.req.get_strb())) self.fail("memory routing/strobe scoreboard");
        if (req->get_strb()) for (uint64_t i=0;i<req->get_size();++i)
            if (req->get_strb()[i]!=r.req.get_strb()[i]) self.fail("byte strobe changed");
        self.mem_seen[r.id]=true;
        for (uint64_t i=0;i<req->get_size();++i) {
            uint64_t a=self.soc?self.offset+i:i;
            if (self.functional) {
                if (req->get_is_write()) {
                    if (!req->get_strb() || req->get_strb()[i]) self.storage[d][r.local+i]=req->get_data()[i];
                } else req->get_data()[i]=self.storage[d][r.local+i];
            } else req->get_data()[i]=(17*d+13*a+(a>>8))&255;
        }
        if (self.stress) {
            r.ready_cycle=self.clock.get_cycles()+4;
            if (r.id%2 || self.endpoint==0) { req->set_latency(4); return vp::IO_REQ_DONE; }
            self.delayed.push_back({req,d,self.clock.get_cycles()+4}); return vp::IO_REQ_GRANTED;
        }
        return vp::IO_REQ_DONE;
    }
    static void tick(vp::Block *b,vp::ClockEvent*) {
        auto &self=*static_cast<Driver*>(b);
        int64_t now=self.clock.get_cycles();
        if (now-self.start>10000000) { self.fail("watchdog"); return; }
        if (self.progress_cycles && now>self.start && (now-self.start)%self.progress_cycles==0) {
            double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-self.wall).count();
            printf("NETWORK3D_PROGRESS {\"cycles\":%ld,\"completed\":%lu,\"total\":%lu,"
                   "\"wall_seconds\":%.3f}\n",now-self.start,self.done,self.total,elapsed);
            fflush(stdout);
        }
        // Requeue before calling the network, fixing input evaluation before
        // the network edge also on the next cycle.
        self.event.enqueue(1);
        self.peak=std::max(self.peak,self.accepted-self.done);
        for (int s=0;s<self.n;++s) if (!self.denied[s]) self.issue(s);
        for (int s=0;s<self.n;++s) {
            if (self.request_retry[s]==now) self.in[s]->retry();
            if (self.response_retry[s]==now) self.out[s]->resp_retry();
        }
        for (size_t i=0;i<self.delayed.size();) {
            auto r=self.delayed[i];
            if (r.cycle>now) { ++i; continue; }
            self.in[r.destination]->resp(r.req);
            self.delayed.erase(self.delayed.begin()+i);
        }
        if (self.done==self.total) {
            // Responses occurred on the preceding network edge. RTL starts
            // its timer half a cycle before its first accepting edge.
            double runtime=now-self.start-1+0.5;
            double injection=self.inject-self.start+0.5;
            double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-self.wall).count();
            printf("NETWORK3D_RESULT {\"status\":\"PASS\",\"runtime_cycles\":%.1f,"
                   "\"injection_cycles\":%.1f,\"drain_cycles\":%.1f,\"transactions\":%lu,"
                   "\"peak_outstanding\":%lu,\"wall_seconds\":%.9f,\"workload_checksum\":\"%016lx\"}\n",
                   runtime,injection,runtime-injection,self.total,self.peak,elapsed,self.checksum);
            self.time.get_engine()->quit(0);
        }
    }
public:
    explicit Driver(vp::ComponentConf &conf) : vp::Component(conf),event(this,tick) {
        auto j=get_js_config(); soc=j->get_child_int("soc"); sparse=j->get_child_int("sparse");
        stress=j->get_child_int("stress"); n=j->get_child_int("nx")*j->get_child_int("ny");
        functional=j->get_child_int("functional");
        backing=j->get_child_int("backing");
        endpoint=j->get_child_int("endpoint");
        interleave_bytes=j->get_child_int("interleave_bytes");
        memory_bytes=j->get_child_int("memory_bytes");
        progress_cycles=j->get_child_int("progress_cycles");
        if (!interleave_bytes || !memory_bytes || memory_bytes%interleave_bytes || progress_cycles<0)
            throw std::invalid_argument("invalid benchmark memory mapping or progress interval");
        if (soc && endpoint==0 && functional) throw std::invalid_argument("inline protocol probe uses reads only");
        if (functional && !soc) throw std::invalid_argument("functional suite requires soc=1");
        groups=j->get_child_int("groups"); repeats=j->get_child_int("repeats"); seed=j->get_child_int("seed");
        burst=j->get_child_int("burst"); offset=j->get_child_int("offset"); bytes=j->get_child_int("datawidth")/8;
        count=functional?10:soc?n:(sparse?groups:n)*repeats; total=uint64_t(n)*count;
        pending.resize(n); denied.resize(n); sent.resize(n); seen.resize(total); mem_seen.resize(total);
        completed.resize(n);
        if (functional) {
            storage.resize(n,std::vector<uint8_t>(memory_bytes));
            for (int d=0;d<n;++d) for (uint64_t a=0;a<memory_bytes;++a) storage[d][a]=(17*d+13*a+(a>>8))&255;
        }
        request_retry.assign(n,-1); response_retry.assign(n,-1);
        target_denied.resize(n); response_denied.resize(n);
        for (int s=0;s<n;++s) {
            out.emplace_back(new vp::IoMaster(s,retry,response));
            in.emplace_back(new vp::IoSlave(s,memory));
            new_master_port("output_"+std::to_string(s),out.back().get());
            new_slave_port("input_"+std::to_string(s),in.back().get());
            for (int k=0;k<count;++k) {
                int d=destination(s,k);
                for (int byte:{d&255,d>>8}) checksum=(checksum^byte)*0x100000001b3ULL;
            }
        }
    }
    void reset(bool active) override {
        if (!active) { start=clock.get_cycles()+1; wall=std::chrono::steady_clock::now(); event.enqueue(1); }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &conf) { return new Driver(conf); }
