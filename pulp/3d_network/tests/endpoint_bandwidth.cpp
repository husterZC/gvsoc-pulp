// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

class EndpointBandwidth : public vp::Component {
    vp::IoMaster output;
    vp::ClockEvent event;
    vp::IoReq *pending=nullptr;
    int count, window, burst, hbm, warmup;
    int sent=0, done=0, peak=0;
    bool denied=false, stress=false, check_interleaving=false, retrying=false;
    vp::IoReq *held=nullptr;
    int64_t retry_at=-1, held_id=-1, held_addr=-1, last_id=-1;
    uint64_t denials=0, beats=0, gaps=0, interleavings=0, response_denials=0;
    uint64_t steady_first_beat=0, steady_last_beat=0;
    int64_t start=-1, first=-1, last=-1, steady_start=-1, steady_end=-1, max_gap=0;
    std::vector<int> received;

    void issue() {
        if (denied || sent==count || sent-done>=window) return;
        if (!pending) {
            pending=vp::IoReqAllocator::get(0)->alloc(); pending->prepare();
            pending->set_opcode(vp::IoReqOpcode::READ);
            pending->set_addr(0); pending->set_size(burst*64);
            pending->initiator=this; pending->parent=nullptr;
            pending->burst_id=sent; pending->is_first=true; pending->is_last=true;
        }
        if (start<0) start=clock.get_cycles();
        auto status=output.req(pending);
        if (status==vp::IO_REQ_DENIED) { denied=true; ++denials; }
        else if (status==vp::IO_REQ_GRANTED) {
            pending=nullptr; ++sent; peak=std::max(peak,sent-done);
        } else throw std::runtime_error("direct endpoint read was not granted");
    }
    static void retry(vp::Block *block,vp::IoRetryChannel) {
        auto &self=*static_cast<EndpointBandwidth*>(block);
        if (self.denied) { self.denied=false; self.issue(); }
    }
    static vp::IoRespAck response(vp::Block *block,vp::IoReq *req) {
        auto &self=*static_cast<EndpointBandwidth*>(block);
        auto id=req->burst_id;
        if (id<0 || id>=self.sent || req->initiator!=&self || req->parent ||
            req->get_resp_status()!=vp::IO_RESP_OK || req->get_size()!=64 ||
            req->get_addr()!=uint64_t(self.received[id]*64) ||
            req->is_first!=(self.received[id]==0) ||
            req->is_last!=(self.received[id]+1==self.burst))
            throw std::runtime_error("direct endpoint response identity/sequence check failed");
        for (int i=0;i<64;++i) {
            uint64_t addr=req->get_addr()+i;
            if (req->get_data()[i]!=uint8_t(13*addr+(addr>>8)))
                throw std::runtime_error("direct endpoint response data check failed");
        }
        auto now=self.clock.get_cycles();
        if (self.held) {
            if (!self.retrying || now!=self.retry_at || req!=self.held ||
                id!=self.held_id || req->get_addr()!=uint64_t(self.held_addr))
                throw std::runtime_error("backpressured beat changed or was sent before retry");
            self.held=nullptr;
        } else if (self.stress && self.beats%11==0) {
            self.held=req; self.held_id=id; self.held_addr=req->get_addr();
            // The initial stall lets several DRAM reads become ready, so the
            // interleaving check exercises arbitration rather than DRAM latency.
            self.retry_at=now+(self.beats==0?32:3); ++self.response_denials;
            return vp::IO_RESP_DENIED;
        }
        if (self.last>=0) {
            if (now<=self.last) throw std::runtime_error("endpoint exceeded one beat per cycle");
            auto gap=now-self.last-1;
            self.gaps+=gap; self.max_gap=std::max(self.max_gap,gap);
        }
        if (self.first<0) self.first=now;
        if (self.last_id>=0 && id!=self.last_id && self.received[self.last_id]<self.burst)
            ++self.interleavings;
        self.last_id=id;
        self.last=now; ++self.beats; ++self.received[id];
        if (req->is_last) {
            ++self.done;
            if (self.done==self.warmup) {
                self.steady_start=now; self.steady_first_beat=self.beats;
            }
            if (self.done==self.count-self.warmup) {
                self.steady_end=now; self.steady_last_beat=self.beats;
            }
        }
        req->free();
        return vp::IO_RESP_ACCEPTED;
    }
    static void tick(vp::Block *block,vp::ClockEvent *) {
        auto &self=*static_cast<EndpointBandwidth*>(block);
        self.event.enqueue(1);
        if (self.start>=0 && self.clock.get_cycles()-self.start>10000000)
            throw std::runtime_error("direct endpoint watchdog");
        self.issue();
        if (self.held && self.clock.get_cycles()==self.retry_at) {
            self.retrying=true; self.output.resp_retry(vp::IO_RETRY_READ); self.retrying=false;
            if (self.held) throw std::runtime_error("endpoint did not synchronously resend held beat");
        }
        if (self.done!=self.count) return;
        if (self.check_interleaving && self.interleavings==0)
            throw std::runtime_error("ready reads were not interleaved");
        auto cycles=self.last-self.start;
        auto steady_cycles=self.steady_end-self.steady_start;
        printf("ENDPOINT_BANDWIDTH_RESULT {\"status\":\"PASS\",\"hbm\":%d,\"count\":%d,"
               "\"window\":%d,\"burst\":%d,\"cycles\":%ld,\"first_response_cycles\":%ld,"
               "\"GBps\":%.6f,\"steady_GBps\":%.6f,\"steady_cycles\":%ld,"
               "\"beats\":%lu,\"gap_cycles\":%lu,\"max_gap\":%ld,\"request_denials\":%lu,\"peak\":%d,"
               "\"interleavings\":%lu,\"response_denials\":%lu}\n",
               self.hbm,self.count,self.window,self.burst,cycles,self.first-self.start,
               double(self.count)*self.burst*64/cycles,
               double(self.steady_last_beat-self.steady_first_beat)*64/steady_cycles,steady_cycles,
               self.beats,self.gaps,self.max_gap,self.denials,self.peak,
               self.interleavings,self.response_denials);
        self.time.get_engine()->quit(0);
    }
public:
    explicit EndpointBandwidth(vp::ComponentConf &conf):vp::Component(conf),
        output(retry,response),event(this,tick) {
        auto js=get_js_config(); count=js->get_child_int("count");
        window=js->get_child_int("window"); burst=js->get_child_int("burst");
        hbm=js->get_child_int("hbm"); warmup=count/8;
        stress=js->get_child_int("stress"); check_interleaving=js->get_child_int("check_interleaving");
        if (count<16 || window<1 || burst<1 || burst>256)
            throw std::invalid_argument("invalid endpoint bandwidth configuration");
        received.resize(count); new_master_port("output",&output);
    }
    void reset(bool active) override { if (!active) event.enqueue(1); }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &conf) { return new EndpointBandwidth(conf); }
