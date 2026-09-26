// SPDX-License-Identifier: Apache-2.0
#include "../network.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
using namespace network3d;

static unsigned philox(unsigned seed,unsigned source,unsigned ordinal) {
    uint32_t a=ordinal,b=source,c=0,d=0,k0=seed,k1=0;
    for (int i=0;i<10;++i) {
        uint64_t p=uint64_t(a)*0xd2511f53u,q=uint64_t(c)*0xcd9e8d57u;
        a=(q>>32)^b^k0; b=uint32_t(q); c=(p>>32)^d^k1; d=uint32_t(p);
        k0+=0x9e3779b9u; k1+=0xbb67ae85u;
    }
    return a;
}

int main(int argc,char **argv) try {
    if (argc!=7) throw std::invalid_argument("usage: native fabric mode all|sparse groups repeats seed");
    Config cfg; cfg.fabric=std::stoi(argv[1]); cfg.routing_mode=std::stoi(argv[2]);
    bool sparse=std::string(argv[3])=="sparse";
    int groups=std::stoi(argv[4]),repeats=std::stoi(argv[5]);
    unsigned seed=std::stoul(argv[6]);
    if (groups<1 || repeats<1) throw std::invalid_argument("positive workload sizes required");
    Network net(cfg); int n=net.size(),count=(sparse?groups:n)*repeats;
    uint64_t total=uint64_t(n)*count,done=0,accepted=0,peak=0,stalls=0;
    std::vector<int> sent(n),dst(total);
    std::vector<bool> seen(total);
    uint64_t checksum=0xcbf29ce484222325ULL;
    for (int s=0;s<n;++s) for (int k=0;k<count;++k) {
        int d=sparse ? philox(seed,s,k/repeats)&1023 : (s+k/repeats)%n;
        dst[uint64_t(s)*count+k]=d;
        for (int byte : {d&255,d>>8}) checksum=(checksum^byte)*0x100000001b3ULL;
    }
    auto start=std::chrono::steady_clock::now();
    uint64_t cycle=0,inject=0;
    while (done<total) {
        if (cycle>100000+256*uint64_t(count)) throw std::runtime_error("watchdog");
        for (int d=0;d<n;++d) if (auto p=net.peek(d)) {
            if (p->id>=total || dst[p->id]!=d || p->dst!=d || seen[p->id] ||
                p->src!=int(p->id/count) || p->id%count>=unsigned(sent[p->src]))
                throw std::runtime_error("wrong destination/payload, duplicate or uninjected packet");
            seen[p->id]=true; ++done; net.take(d);
        }
        for (int s=0;s<n;++s) if (sent[s]<count) {
            uint64_t id=uint64_t(s)*count+sent[s]; Packet p; p.id=id; p.src=s; p.dst=dst[id];
            if (net.offer(s,p)) { ++sent[s]; ++accepted; inject=cycle; } else ++stalls;
        }
        net.step(); peak=std::max(peak,accepted-done);
        if (done==total) break;
        ++cycle;
    }
    double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    if (net.occupancy()) throw std::runtime_error("trailing traffic");
    std::cout<<std::setprecision(12)<<"{\"status\":\"PASS\",\"runtime_cycles\":"<<cycle+0.5
        <<",\"injection_cycles\":"<<inject+0.5<<",\"drain_cycles\":"<<cycle-inject
        <<",\"total_beats\":"<<total<<",\"peak_outstanding\":"<<peak
        <<",\"stalled_source_cycles\":"<<stalls<<",\"wall_seconds\":"<<seconds
        <<",\"workload_checksum\":\""<<std::hex<<checksum<<"\"}\n";
} catch(const std::exception &e) { std::cerr<<e.what()<<"\n"; return 1; }
