// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/itf/wire.hpp>
#include <array>
#include <cstring>
#include "compute.hpp"
#include "profile.hpp"

class MXCoreFP4 : public vp::Component {
public:
    MXCoreFP4(vp::ComponentConf &config) : vp::Component(config), event(this, &step) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        new_slave_port("input", &input);
        new_master_port("out", &out);
        new_master_port("irq", &irq);
    }
private:
    vp::Trace trace;
    vp::IoSlave input{&mmio};
    vp::IoMaster out{&retry, &response};
    vp::WireMaster<bool> irq;
    vp::ClockEvent event;
    vp::IoReq request;
    mxcore_fp4::Compute compute;
    const mxcore_fp4::Profile *profile=nullptr;
    const uint32_t *transfers=nullptr;
    std::array<uint32_t,13> regs{};
    std::array<uint32_t,6> bases{};
    std::array<uint8_t,32> payload{};
    unsigned index=0, nominal=0, region=0, offset=0;
    int64_t start_cycle=0, issue_cycle=0, stalls=0;
    uint32_t elapsed=0, error=0;
    uint32_t nominal_cycles=0, read_bytes=0, write_bytes=0;
    bool busy=false, acquired=false, done=false, pending=false, denied=false;
    bool canceled=false, completing=false;

    void signal(bool value) { if (irq.is_bound()) irq.sync(value); }
    void reset(bool active) override {
        if (!active) return;
        event.cancel();
        regs.fill(0); busy=false; acquired=false; done=false;
        elapsed=0; error=0; completing=false;
        stalls=0; start_cycle=0; nominal_cycles=0; read_bytes=0; write_bytes=0;
        // A request already accepted by memory must retain its payload until
        // the response arrives. Reject a new trigger during that drain.
        canceled=pending;
        signal(false);
    }
    static vp::IoReqStatus mmio(vp::Block *block, vp::IoReq *req) {
        auto &s=*static_cast<MXCoreFP4 *>(block);
        auto invalid=[&]() { req->set_resp_status(vp::IO_RESP_INVALID); return vp::IO_REQ_DONE; };
        if (req->get_size()!=4 || (req->get_addr() & 3) || !req->get_data() ||
            (req->get_opcode()!=vp::READ && req->get_opcode()!=vp::WRITE)) return invalid();
        if (req->get_strb())
            for (int i=0; i<4; ++i) if (!req->get_strb()[i]) return invalid();
        uint64_t addr=req->get_addr();
        uint32_t value=0;
        if (req->get_is_write())
            for (int i=0; i<4; ++i) value |= uint32_t(req->get_data()[i]) << (8*i);
        if (addr>=0x20 && addr<=0x50) {
            unsigned reg=(addr-0x20)/4;
            if (req->get_is_write()) {
                if (s.busy || s.pending) return invalid();
                s.regs[reg]=value;
            } else value=s.regs[reg];
        } else if (req->get_is_write()) {
            switch (addr) {
            case 0:
                if (!s.launch()) return invalid();
                break;
            case 8: // Sticky completion acknowledge (model software interface).
                s.done=false; s.signal(false); break;
            case 0x14: s.reset(true); break;
            default: return invalid();
            }
        } else {
            switch (addr) {
            case 4:
                value=(s.busy || s.acquired || s.pending) ? 0xffffffffU : 0;
                if (value==0) s.acquired=true;
                break;
            case 8: value=s.done; break;
            case 0xc: value=s.busy; break;
            case 0x10: value=s.busy ? 0 : 0xffffffffU; break;
            case 0x60: value=s.elapsed; break;
            case 0x64: value=s.error; break;
            case 0x68: value=s.nominal_cycles; break;
            case 0x6c: value=s.stalls; break;
            case 0x70: value=s.read_bytes; break;
            case 0x74: value=s.write_bytes; break;
            case 0x78: value=uint64_t(s.start_cycle); break;
            case 0x7c: value=uint64_t(s.start_cycle)>>32; break;
            default: return invalid();
            }
        }
        if (!req->get_is_write())
            for (int i=0; i<4; ++i) req->get_data()[i]=value >> (8*i);
        req->set_resp_status(vp::IO_RESP_OK);
        return vp::IO_REQ_DONE;
    }
    bool launch() {
        if (busy || pending || !acquired) return false;
        unsigned m=regs[6]&1023, k=(regs[6]>>10)&4095, n=regs[6]>>22;
        auto format=mxcore_fp4::OutputFormat((regs[7]>>23)&7);
        unsigned bits=mxcore_fp4::output_bits(format);
        profile=mxcore_fp4::find_output_profile(m,n,k,format,transfers);
        unsigned control=(unsigned(format)<<23)|(format==mxcore_fp4::OutputFormat::MXFP4 ? 1U<<21 : 0)|
                         (8U<<9)|(19U<<3);
        // No silent rounding, fallback format, or extrapolation to an unmeasured shape.
        if (!profile || !bits || regs[7]!=control ||
            regs[8]!=((m/32)|((n/32)<<4)|((k/16)<<9)|((k/32)<<16)) ||
            regs[9]!=2048 || regs[10]!=2048 || regs[11]!=32*32*bits || regs[12]!=k) {
            error=1; return false;
        }
        compute.init(m,n,k,format);
        for (unsigned i=0; i<6; ++i) {
            bases[i]=regs[i];
            if (!compute.data[i].empty() && ((bases[i]&31) ||
                uint64_t(bases[i])+compute.data[i].size()>0x100000000ULL)) {
                error=1; return false;
            }
        }
        for (unsigned i=4; i<6; ++i) for (unsigned j=0; j<i; ++j)
            if (!compute.data[i].empty() && !compute.data[j].empty() &&
                uint64_t(bases[i]) < uint64_t(bases[j])+compute.data[j].size() &&
                uint64_t(bases[j]) < uint64_t(bases[i])+compute.data[i].size()) {
                error=1; return false;
            }
        busy=true; acquired=false; done=false; error=0; elapsed=0;
        canceled=false; completing=false; signal(false);
        start_cycle=clock.get_cycles(); stalls=0; index=0;
        nominal_cycles=profile->cycles; read_bytes=0; write_bytes=0;
        nominal=transfers[profile->first]>>16;
        event.enqueue(nominal);
        return true;
    }
    void finish(bool failed=false) {
        busy=false; done=true; completing=false;
        elapsed=clock.get_cycles()-start_cycle;
        if (failed) error=2;
        signal(true);
    }
    void drive() {
        if (!busy) return;
        int64_t due=start_cycle+stalls+(index==profile->count ? profile->cycles : nominal);
        int64_t now=clock.get_cycles();
        if (due>now) { event.enqueue(due-now); return; }
        if (index==profile->count) { finish(); return; }
        uint32_t word=transfers[profile->first+index];
        region=(word>>13)&7; offset=(word&8191)*32;
        if (region>=6 || offset+32>compute.data[region].size())
            trace.fatal("Invalid MXCoreFP4 calibration profile\n");
        if (region>=4) {
            unsigned block_bytes=32*mxcore_fp4::output_bits(compute.format)/8;
            unsigned first=region==4 ? offset/block_bytes : offset;
            unsigned last=region==4 ? (offset+31)/block_bytes : offset+31;
            try { for (unsigned b=first; b<=last; ++b) compute.block(b); }
            catch (const std::exception &e) { trace.fatal("%s\n",e.what()); }
            std::memcpy(payload.data(), compute.data[region].data()+offset, 32);
        }
        issue_cycle=now;
        pending=true;
        send();
    }
    void send() {
        request.prepare();
        request.set_addr(uint64_t(bases[region])+offset);
        request.set_data(payload.data()); request.set_size(32);
        request.set_is_write(region>=4);
        request.is_first=true; request.is_last=true; request.initiator=this;
        auto status=out.req(&request);
        denied=status==vp::IO_REQ_DENIED;
        if (status==vp::IO_REQ_DONE) received(&request);
    }
    void received(vp::IoReq *req) {
        if (req!=&request || !req->is_first || !req->is_last)
            trace.fatal("MXCoreFP4 requires a single-request io_v2 binding; use the single-request-to-beat adapter\n");
        pending=false; denied=false;
        if (canceled) { canceled=false; return; }
        if (req->get_resp_status()!=vp::IO_RESP_OK) { finish(true); return; }
        int64_t now=clock.get_cycles();
        int64_t ready=std::max(issue_cycle+1, now+req->get_full_latency());
        stalls+=ready-issue_cycle-1;
        completing=true;
        if (ready>now) event.enqueue(ready-now);
        else complete_transfer();
    }
    void complete_transfer() {
        completing=false;
        if (region<4) read_bytes+=32; else write_bytes+=32;
        if (region<4) {
            std::memcpy(compute.data[region].data()+offset, payload.data(), 32);
            std::fill(compute.present[region].begin()+offset, compute.present[region].begin()+offset+32, true);
        }
        ++index;
        if (index<profile->count) nominal+=transfers[profile->first+index]>>16;
        drive();
    }
    static void step(vp::Block *block, vp::ClockEvent *) {
        auto &s=*static_cast<MXCoreFP4 *>(block);
        if (s.completing) s.complete_transfer(); else s.drive();
    }
    static void retry(vp::Block *block, vp::IoRetryChannel) {
        auto &s=*static_cast<MXCoreFP4 *>(block);
        // The downstream port's accept window is open only during this callback.
        if (s.denied) s.send();
    }
    static vp::IoRespAck response(vp::Block *block, vp::IoReq *req) {
        static_cast<MXCoreFP4 *>(block)->received(req);
        return vp::IO_RESP_ACCEPTED;
    }
};

extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new MXCoreFP4(config); }
