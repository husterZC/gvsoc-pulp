// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/itf/wire.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../../pulp/mxcore_fp4/profile.hpp"

class Test : public vp::Component {
public:
    Test(vp::ComponentConf &config):vp::Component(config),run_event(this,&run),memory_event(this,&memory_step),
        watchdog(this,&timeout) {
        traces.new_trace("trace",&trace,vp::DEBUG);
        new_master_port("mmio",&mmio); new_slave_port("memory",&memory);
        irq.set_sync_meth(&irq_handler); new_slave_port("irq",&irq);
        directory=get_js_config()->get_child_str("vectors");
        mode=get_js_config()->get_child_str("mode");
        if (mode!="sync" && mode!="delayed" && mode!="async" && mode!="denied" && mode!="error")
            trace.fatal("Unknown memory mode %s\n",mode.c_str());
    }
private:
    vp::Trace trace;
    vp::IoMaster mmio{nullptr,nullptr};
    vp::IoSlave memory{&access};
    vp::WireSlave<bool> irq;
    vp::ClockEvent run_event,memory_event,watchdog;
    std::string directory,mode;
    std::vector<uint8_t> mem,expected;
    const mxcore_fp4::Profile *p=nullptr;
    unsigned case_index=0,txn_index=0,nominal=0;
    int64_t start=0;
    std::array<uint32_t,6> base{};
    vp::IoReq *held=nullptr;
    bool accept_retry=false;
    void check(bool ok,const char *message) { if (!ok) trace.fatal("MXCoreFP4: %s\n",message); }
    void reset(bool active) override {
        if (!active) { run_event.enqueue(1); watchdog.enqueue(1000000); }
    }
    static void timeout(vp::Block *b,vp::ClockEvent *) { static_cast<Test *>(b)->trace.fatal("MXCoreFP4 watchdog\n"); }
    uint32_t reg(uint32_t addr,uint32_t value=0,bool write=false,bool valid=true,unsigned size=4) {
        uint8_t bytes[4]; for (int i=0;i<4;++i) bytes[i]=value>>(8*i);
        vp::IoReq req(addr,bytes,size,write); req.prepare();
        check(mmio.req(&req)==vp::IO_REQ_DONE,"MMIO must finish inline");
        check((req.get_resp_status()==vp::IO_RESP_OK)==valid,"Unexpected MMIO status");
        value=0; for (int i=0;i<4;++i) value|=uint32_t(bytes[i])<<(8*i);
        return value;
    }
    void write(uint32_t addr,uint32_t value) { reg(addr,value,true); }
    static void run(vp::Block *b,vp::ClockEvent *) { static_cast<Test *>(b)->next(); }
    void next() {
        unsigned total=sizeof(mxcore_fp4::profiles)/sizeof(mxcore_fp4::profiles[0]);
        if (case_index==total) {
            printf("MXCoreFP4 PASS: %u shapes, mode=%s\n",case_index,mode.c_str());
            watchdog.cancel(); time.get_engine()->quit(0); return;
        }
        p=&mxcore_fp4::profiles[case_index];
        std::string name="m"+std::to_string(p->m)+"_n"+std::to_string(p->n)+"_k"+std::to_string(p->k);
        auto load=[&](const std::string &suffix) {
            std::ifstream file(directory+"/"+name+suffix,std::ios::binary);
            check(bool(file),"Missing fixtures: run calibrate.py first");
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),{});
        };
        mem=load(".bin"); expected=load(".expected");
        base[0]=0x1000; base[1]=base[0]+p->m*p->k/2; base[2]=base[1]+p->n*p->k/2;
        base[3]=base[2]+p->m*p->k/32; base[4]=base[3]+p->n*p->k/32;
        base[5]=base[4]+p->m*p->n/2;
        // Poison the output region to catch incomplete writes and false completion.
        std::fill(mem.begin()+base[4],mem.end(),0xa5);
        write(0x14,0);
        reg(0,0,true,false); // no job acquired
        reg(0x100,0,false,false); reg(0x21,0,false,false); reg(0x20,0,false,false,2);
        check(reg(4)==0,"acquire"); check(reg(4)==0xffffffffU,"duplicate acquire");
        for (unsigned i=0;i<6;++i) write(0x20+4*i,base[i]);
        write(0x38,p->m|(p->k<<10)|(p->n<<22));
        write(0x3c,(1U<<21)|(8U<<9)|(19U<<3));
        write(0x40,(p->m/32)|((p->n/32)<<4)|((p->k/16)<<9)|((p->k/32)<<16));
        write(0x44,2048); write(0x48,2048); write(0x4c,4096); write(0x50,p->k);
        if (case_index==0) {
            write(0x38,0); reg(0,0,true,false); // reject unsupported geometry
            write(0x38,p->m|(p->k<<10)|(p->n<<22));
            write(0x3c,0); reg(0,0,true,false); // reject a non-FP4 operation
            write(0x3c,(1U<<21)|(8U<<9)|(19U<<3));
            write(0x30,base[0]); reg(0,0,true,false); // reject in-place output
            write(0x30,base[4]);
        }
        start=clock.get_cycles(); txn_index=0; nominal=0;
        write(0,0);
        check(reg(0xc)==1,"busy after trigger");
        reg(0,0,true,false); reg(0x38,0,true,false); // reject overlapping jobs/register writes
    }
    static vp::IoReqStatus access(vp::Block *b,vp::IoReq *req) {
        auto &s=*static_cast<Test *>(b);
        if (s.mode=="denied" && !s.accept_retry) {
            s.check(s.held==nullptr,"multiple denied requests");
            s.held=req; s.memory_event.enqueue(2); return vp::IO_REQ_DENIED;
        }
        s.check(req->get_size()==32,"TCDM beat width");
        uint32_t word=mxcore_fp4::transfers[s.p->first+s.txn_index];
        unsigned region=(word>>13)&7,offset=(word&8191)*32;
        s.nominal+=word>>16;
        s.check(req->get_addr()==uint64_t(s.base[region])+offset,"TCDM address/order");
        s.check(req->get_is_write()==(region>=4),"TCDM direction");
        int64_t extra=s.mode=="delayed" || s.mode=="async" || s.mode=="denied" ? 2 : 0;
        s.check(s.clock.get_cycles()-s.start==s.nominal+extra*s.txn_index+(s.mode=="denied" ? 2 : 0),"TCDM issue cycle");
        ++s.txn_index;
        if (s.mode=="error") { req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE; }
        s.check(req->get_addr()+32<=s.mem.size(),"memory bounds");
        if (req->get_is_write()) std::memcpy(s.mem.data()+req->get_addr(),req->get_data(),32);
        else std::memcpy(req->get_data(),s.mem.data()+req->get_addr(),32);
        req->set_resp_status(vp::IO_RESP_OK);
        if (s.mode=="async") {
            s.held=req; s.memory_event.enqueue(3); return vp::IO_REQ_GRANTED;
        }
        req->set_latency(s.mode=="delayed" ? 3 : 1);
        return vp::IO_REQ_DONE;
    }
    static void memory_step(vp::Block *b,vp::ClockEvent *) {
        auto &s=*static_cast<Test *>(b);
        auto *req=s.held; s.held=nullptr;
        if (s.mode=="denied") { s.accept_retry=true; s.memory.retry(); s.accept_retry=false; }
        else s.memory.resp(req);
    }
    static void irq_handler(vp::Block *b,bool active) {
        if (!active) return;
        auto &s=*static_cast<Test *>(b);
        int64_t cycles=s.clock.get_cycles()-s.start;
        s.check(s.reg(0xc)==0 && s.reg(8)==1,"completion registers");
        s.check(s.reg(0x60)==cycles,"elapsed cycle register");
        if (s.mode=="error") s.check(s.reg(0x64)==2,"memory error reporting");
        else {
            s.check(s.txn_index==s.p->count,"transfer count");
            s.check(s.reg(0x64)==0,"unexpected accelerator error");
            s.check(std::equal(s.expected.begin(),s.expected.end(),s.mem.begin()+s.base[4]),"RTL result bytes");
            s.check(cycles==s.p->cycles+(s.mode=="sync" ? 0 : 2*s.p->count),"RTL completion cycle");
        }
        printf("MXCORE_GVSOC,%u,%u,%u,%ld,%s\n",s.p->m,s.p->n,s.p->k,cycles,s.mode.c_str());
        s.write(8,1); s.check(s.reg(8)==0,"IRQ acknowledge");
        ++s.case_index; s.run_event.enqueue(1);
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new Test(config); }
