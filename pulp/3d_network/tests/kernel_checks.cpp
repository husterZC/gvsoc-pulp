// SPDX-License-Identifier: Apache-2.0
#include "soc_memory.hpp"
#include <iostream>
#include <random>
#include <set>

static void require(bool ok,const char *message) { if (!ok) throw std::runtime_error(message); }
static void native(network3d::Config config) {
    network3d::Network net(config);
    std::mt19937 rng(42); std::set<uint64_t> received;
    const int count=257;
    std::vector<network3d::Packet> packets(count);
    for (int i=0;i<count;++i) {
        packets[i].id=i; packets[i].src=rng()%net.size(); packets[i].dst=rng()%net.size();
    }
    // A reset must discard in-flight beats and arbitration locks.
    for (int i=0;i<20;++i) { net.offer(packets[i].src,packets[i]); net.step(); }
    net.reset(); require(net.occupancy()==0,"reset did not flush fabric");
    int sent=0;
    for (int cycle=0;received.size()<count;++cycle) {
        require(cycle<10000,"stalled native fabric");
        for (int d=0;d<net.size();++d) if (auto p=net.peek(d)) {
            if (rng()%4==0) continue;
            require(p->id<unsigned(sent) && packets[p->id].dst==d &&
                    p->src==packets[p->id].src && received.insert(p->id).second,
                    "native duplicate/destination/data error");
            net.take(d);
        }
        if (sent<count && net.offer(packets[sent].src,packets[sent])) ++sent;
        net.step();
    }
    require(net.occupancy()==0,"native trailing data");
}

static void mixed_soc() {
    network3d::SocConfig config; config.network.fabric=1;
    config.network.num_x=3; config.network.num_y=2;
    config.source_contexts=3; config.memory_contexts=2;
    config.memory_base=uint64_t(1)<<32; config.axi_addr_width=64;
    config.memory_bytes=8192;
    network3d::Soc soc(config);
    TestMemory memory(soc,1);
    std::mt19937 rng(19);
    std::vector<network3d::Transaction> requests(500);
    std::set<uint64_t> issued,done;
    memory.access=[&](auto &t) {
        require(issued.insert(t.id).second,"duplicate memory issue");
        uint64_t relative=t.address-config.memory_base;
        require(t.local_address==(relative/(4096*soc.size()))*4096+relative%4096,
                "wrong interleaved local address");
        return unsigned(rng()%7);
    };
    soc.respond=[&](auto &t) {
        if (rng()%3==0) return false;
        require(!t.error && issued.count(t.id) && done.insert(t.id).second,
                "mixed read/write response error"); return true;
    };
    int sent=0;
    for (int cycle=0;done.size()<requests.size();++cycle) {
        require(cycle<100000,"mixed read/write deadlock");
        if (sent<int(requests.size())) {
            int source=rng()%soc.size();
            if (soc.can_offer(source)) {
                auto &t=requests[sent]; t.id=sent; t.write=rng()%2; t.size=(1+rng()%16)*8;
                t.address=config.memory_base+4096*(rng()%(soc.size()*2))+64;
                require(soc.offer(source,t),"unexpected source denial"); ++sent;
            }
        }
        memory.step(); soc.step();
    }
    require(!soc.outstanding(),"mixed trailing contexts");
    soc.reset(); memory.reset();
    network3d::Transaction bad; bad.address=0; bad.size=0;
    memory.access=[](auto &) { throw std::runtime_error("invalid request reached memory"); return 0u; };
    soc.respond=[](auto &t) { require(t.error,"missing decode error"); return true; };
    require(soc.offer(0,bad),"invalid admission");
    soc.step(); soc.step(); require(!soc.outstanding(),"zero-size transaction hung");
}

static void endpoint() {
    using namespace network3d;
    MemoryEndpointTiming memory(2);
    MemoryRequest a,b,c; a.beats=2; b.beats=1; c.beats=1;
    a.ready=b.ready=c.ready=true;
    require(memory.read(a,0) && memory.read(b,0) && !memory.read(c,0),"read-slot admission");
    require(!memory.read_response(0).request,"unregistered first R");
    auto r=memory.read_response(1);
    require(r.request==&a && r.sequence==0,"initial endpoint read selection");
    require(memory.read_response(7).request==&a,"stalled R selection changed");
    memory.read_accepted(7);
    require(memory.read_response(8).request==&b,"read round robin did not rotate per beat");
    memory.read_accepted(8);
    require(!memory.read(c,8) && memory.read(c,9),"same-edge read slot reuse");
    r=memory.read_response(9);
    require(r.request==&a && r.sequence==1,"read burst lost progress");
    memory.read_accepted(9);
    require(memory.start_write(a,9) && !memory.can_write_beat(9),"AW to W registration");
    require(memory.can_write_beat(10),"first W not ready"); memory.write_beat(false,10);
    require(!memory.can_write_beat(10) && !memory.write_response(11),"premature W/B");
    memory.write_beat(true,11);
    a.ready_cycle=14;
    require(!memory.write_response(11) && !memory.write_response(13),"early annotated B");
    require(memory.write_response(14)==&a && !memory.can_write(14),"B ownership");
    memory.write_accepted(14);
    require(!memory.start_write(b,14) && memory.start_write(b,15),"same-edge B to AW reuse");
    memory.reset();
    require(memory.can_read(0) && memory.can_write(0) && !memory.read_response(100).request &&
            !memory.write_response(100),"endpoint reset retained requests or locks");

    // The network must wait indefinitely for an external memory response; it
    // cannot synthesize one using an internal latency or read-slot scheduler.
    SocConfig cfg; cfg.network.fabric=1; cfg.network.num_x=cfg.network.num_y=1;
    Soc soc(cfg); Transaction tx; tx.size=8;
    bool issued=false,done=false;
    soc.issue=[&](auto &,int sequence) { require(sequence==-1,"unexpected W"); issued=true; return true; };
    soc.respond=[&](auto &) { done=true; return true; };
    require(soc.offer(0,tx),"external endpoint request denied");
    for (int i=0;i<100;++i) soc.step();
    require(issued && !done && soc.outstanding()==1,"SoC fabricated memory response");
    soc.memory_response(tx,0);
    for (int i=0;i<100 && !done;++i) soc.step();
    require(done && !soc.outstanding(),"external response did not release transaction");
}

int main() try {
    for (int spill:{1,2,3}) {
        network3d::Config c; c.fabric=1; c.num_x=3; c.num_y=2; c.io_spill=spill; native(c);
    }
    for (int mode=0;mode<3;++mode) {
        network3d::Config c; c.routing_mode=mode; native(c);
    }
    network3d::Config c; c.num_levels=4;
    auto shape=network3d::Network::fattree_shape(4); c.num_x=shape.first; c.num_y=shape.second;
    native(c); mixed_soc(); endpoint();
    std::cout<<"KERNEL_CHECKS_PASS: stalls, reset, levels, spills, mixed R/W, 64-bit map, external endpoint\n";
} catch (const std::exception &e) { std::cerr<<e.what()<<"\n"; return 1; }
