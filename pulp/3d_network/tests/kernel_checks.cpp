// SPDX-License-Identifier: Apache-2.0
#include "i3d_memory.hpp"
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

static void xbar_pipeline(int ports,int spill) {
    network3d::Config cfg; cfg.fabric=2; cfg.num_x=ports; cfg.num_y=1; cfg.io_spill=spill;
    network3d::Network net(cfg);
    require(net.router_count()==1,"crossbar must be a single switch");
    const int beats=12,latency=2*spill;
    for (int cycle=0;cycle<beats+latency;++cycle) {
        for (int dst=0;dst<ports;++dst) {
            auto p=net.peek(dst);
            require(bool(p)==(cycle>=latency),"crossbar pipeline latency/bandwidth");
            if (p) {
                int src=(dst+ports-1)%ports,seq=cycle-latency;
                require(p->src==src && p->dst==dst && p->seq==seq &&
                        p->id==uint64_t(src)*beats+seq,"crossbar permutation delivery");
                net.take(dst);
            }
        }
        if (cycle<beats) for (int src=0;src<ports;++src) {
            network3d::Packet p; p.src=src; p.dst=(src+1)%ports; p.seq=cycle;
            p.id=uint64_t(src)*beats+cycle;
            require(net.offer(src,p),"crossbar failed one packet/input/cycle");
        }
        net.step();
    }
    require(!net.occupancy(),"crossbar permutation did not drain");
}

static void xbar_contention() {
    const int ports=65,beats=3;
    network3d::Config cfg; cfg.fabric=2; cfg.num_x=ports; cfg.num_y=1; cfg.io_spill=1;
    network3d::Network net(cfg);
    std::vector<int> sent(ports,0);
    int received=0; bool held=false; uint64_t held_id=0;
    for (int cycle=0;received<ports*beats;++cycle) {
        require(cycle<2000,"crossbar contention starvation");
        if (auto p=net.peek(0)) {
            if (held) require(p->id==held_id,"crossbar changed a stalled output");
            held=cycle<12 || cycle%3==0; held_id=p->id;
            if (!held) {
                require(p->src==received%ports && p->seq==received/ports,
                        "crossbar arbitration lost fairness or source order");
                net.take(0); ++received;
            }
        } else require(!held,"crossbar withdrew a stalled output");
        for (int src=0;src<ports;++src) if (sent[src]<beats) {
            network3d::Packet p; p.src=src; p.dst=0; p.seq=sent[src];
            p.id=uint64_t(src)*beats+p.seq;
            if (net.offer(src,p)) ++sent[src];
        }
        net.step();
    }
    require(!net.occupancy(),"crossbar contention did not drain");
}

static void xbar_locks() {
    network3d::Config cfg; cfg.fabric=2; cfg.num_x=4; cfg.num_y=1; cfg.io_spill=1;
    network3d::Network net(cfg);
    auto send=[&](int src,int dst,uint64_t id) {
        network3d::Packet p; p.src=src; p.dst=dst; p.id=id;
        require(net.offer(src,p),"crossbar lock test admission");
    };
    auto receive=[&](int dst,uint64_t id) {
        auto p=net.peek(dst);
        require(p && p->id==id,"crossbar output/lock selection"); net.take(dst);
    };
    auto block=[&]() {
        send(1,0,10); net.step();
        send(1,0,11); net.step();
        send(2,0,20); net.step();
        net.step(); // Output 0 is full; lock the request set containing input 2.
    };
    block();
    send(0,0,0); receive(0,10); net.step();
    receive(0,11); net.step(); // Late input 0 must not preempt locked input 2.
    receive(0,20); net.step();
    receive(0,0); net.step();
    require(!net.occupancy(),"crossbar lock test did not drain");

    net.reset(); block();
    send(3,2,32); net.step(); net.step();
    receive(2,32); net.step(); // Blocked output 0 cannot stop another output.
    net.reset();
    require(!net.occupancy() && !net.peek(0),"crossbar reset kept buffered packets");
    send(0,0,100); net.step(); net.step();
    receive(0,100); net.step(); // Reset must also discard the locked request set.
    require(!net.occupancy(),"crossbar reset kept arbitration state");
}

static void mixed_i3d(int fabric) {
    network3d::I3dConfig config; config.network.fabric=fabric;
    config.network.num_x=3; config.network.num_y=2;
    config.source_contexts=3; config.memory_contexts=2;
    config.memory_base=uint64_t(1)<<32; config.axi_addr_width=64;
    config.memory_bytes=8192;
    network3d::I3d i3d(config);
    TestMemory memory(i3d,1);
    std::mt19937 rng(19);
    std::vector<network3d::Transaction> requests(500);
    std::set<uint64_t> issued,done;
    memory.access=[&](auto &t) {
        require(issued.insert(t.id).second,"duplicate memory issue");
        uint64_t relative=t.address-config.memory_base;
        require(t.local_address==(relative/(4096*i3d.size()))*4096+relative%4096,
                "wrong interleaved local address");
        return unsigned(rng()%7);
    };
    i3d.respond=[&](auto &t) {
        if (rng()%3==0) return false;
        require(!t.error && issued.count(t.id) && done.insert(t.id).second,
                "mixed read/write response error"); return true;
    };
    int sent=0;
    for (int cycle=0;done.size()<requests.size();++cycle) {
        require(cycle<100000,"mixed read/write deadlock");
        if (sent<int(requests.size())) {
            int source=rng()%i3d.size();
            if (i3d.can_offer(source)) {
                auto &t=requests[sent]; t.id=sent; t.write=rng()%2; t.size=(1+rng()%16)*8;
                t.address=config.memory_base+4096*(rng()%(i3d.size()*2))+64;
                require(i3d.offer(source,t),"unexpected source denial"); ++sent;
            }
        }
        memory.step(); i3d.step();
    }
    require(!i3d.outstanding(),"mixed trailing contexts");
    i3d.reset(); memory.reset();
    network3d::Transaction bad; bad.address=0; bad.size=0;
    memory.access=[](auto &) { throw std::runtime_error("invalid request reached memory"); return 0u; };
    i3d.respond=[](auto &t) { require(t.error,"missing decode error"); return true; };
    require(i3d.offer(0,bad),"invalid admission");
    i3d.step(); i3d.step(); require(!i3d.outstanding(),"zero-size transaction hung");
}

static void endpoint(int fabric) {
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
    I3dConfig cfg; cfg.network.fabric=fabric; cfg.network.num_x=cfg.network.num_y=1;
    I3d i3d(cfg); Transaction tx; tx.size=8;
    bool issued=false,done=false;
    i3d.issue=[&](auto &,int sequence) { require(sequence==-1,"unexpected W"); issued=true; return true; };
    i3d.respond=[&](auto &) { done=true; return true; };
    require(i3d.offer(0,tx),"external endpoint request denied");
    for (int i=0;i<100;++i) i3d.step();
    require(issued && !done && i3d.outstanding()==1,"I3D fabricated memory response");
    i3d.memory_response(tx,0);
    for (int i=0;i<100 && !done;++i) i3d.step();
    require(done && !i3d.outstanding(),"external response did not release transaction");
}

int main() try {
    for (int spill:{1,2,3}) {
        network3d::Config c; c.fabric=1; c.num_x=3; c.num_y=2; c.io_spill=spill; native(c);
        c.fabric=2; native(c);
        for (int ports:{1,6,33,64,1024}) xbar_pipeline(ports,spill);
    }
    for (int mode=0;mode<3;++mode) {
        network3d::Config c; c.routing_mode=mode; native(c);
    }
    network3d::Config c; c.num_levels=4;
    auto shape=network3d::Network::fattree_shape(4); c.num_x=shape.first; c.num_y=shape.second;
    native(c); xbar_contention(); xbar_locks();
    for (int fabric:{1,2}) { mixed_i3d(fabric); endpoint(fabric); }
    std::cout<<"KERNEL_CHECKS_PASS: stalls, reset, levels, spills, crossbar latency/throughput/arbitration, mixed R/W, external endpoint\n";
} catch (const std::exception &e) { std::cerr<<e.what()<<"\n"; return 1; }
