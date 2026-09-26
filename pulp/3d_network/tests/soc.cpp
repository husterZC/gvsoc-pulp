// SPDX-License-Identifier: Apache-2.0
#include "soc_memory.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
using namespace network3d;
int main(int argc,char **argv) try {
    if (argc!=10) throw std::invalid_argument("usage: soc fabric mode nx ny burst sc mc readslots offset");
    SocConfig c; c.network.fabric=std::stoi(argv[1]); c.network.routing_mode=std::stoi(argv[2]);
    c.network.num_x=std::stoi(argv[3]);c.network.num_y=std::stoi(argv[4]);
    int burst=std::stoi(argv[5]); c.source_contexts=std::stoi(argv[6]);
    c.memory_contexts=std::stoi(argv[7]); int readslots=std::stoi(argv[8]);
    int offset=std::stoi(argv[9]);
    if (burst<1 || burst>256 || offset<0 || offset+burst*8>4096) throw std::invalid_argument("bad burst");
    Soc soc(c); int n=soc.size(); uint64_t total=uint64_t(n)*n,done=0,issued=0,peak=0;
    TestMemory memory(soc,readslots);
    std::vector<Transaction> requests(total);
    std::vector<bool> seen(total),mem_seen(total);std::vector<int> next(n);
    memory.access=[&](Transaction &tx) {
        if (mem_seen[tx.id] || tx.local_address!=unsigned(offset) || tx.destination!=int((tx.source+tx.id%n)%n))
            throw std::runtime_error("memory request scoreboard");
        mem_seen[tx.id]=true; ++issued; return 0;
    };
    soc.respond=[&](Transaction &tx) {
        if (seen[tx.id] || !mem_seen[tx.id] || tx.error) throw std::runtime_error("response scoreboard");
        seen[tx.id]=true;++done;return true;
    };
    uint64_t cycle=0,inject=0;
    auto start=std::chrono::steady_clock::now();
    while (done<total) {
        if (cycle>10000000) throw std::runtime_error("watchdog");
        for (int s=0;s<n;++s) if (next[s]<n && soc.can_offer(s)) {
            auto &tx=requests[uint64_t(s)*n+next[s]];
            tx.id=uint64_t(s)*n+next[s]; tx.address=uint64_t((s+next[s])%n)*4096+offset;tx.size=burst*8;
            if (!soc.offer(s,tx)) throw std::logic_error("unexpected admission failure");
            ++next[s];inject=cycle;
        }
        memory.step(); soc.step();peak=std::max(peak,soc.outstanding());
        if (done==total) break;
        ++cycle;
    }
    if (issued!=total || soc.control_packets!=total*2 || soc.payload_packets!=total*(1+burst))
        throw std::runtime_error("packet coverage");
    double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout<<std::setprecision(12)<<"{\"status\":\"PASS\",\"runtime_cycles\":"<<cycle+0.5
        <<",\"injection_cycles\":"<<inject+0.5<<",\"drain_cycles\":"<<cycle-inject
        <<",\"transactions\":"<<total<<",\"response_beats\":"<<total*burst
        <<",\"peak_outstanding\":"<<peak<<",\"payload_packets\":"<<soc.payload_packets
        <<",\"control_packets\":"<<soc.control_packets<<",\"wall_seconds\":"<<elapsed<<"}\n";
} catch(const std::exception &e) { std::cerr<<e.what()<<"\n";return 1; }
