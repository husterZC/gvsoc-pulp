// SPDX-License-Identifier: Apache-2.0
// Cycle models of registered fat-tree, mesh and crossbar packet fabrics.
#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace network3d {

struct Config {
    int fabric = 0; // 0: radix-16 dual fat tree; 1: XY mesh; 2: full crossbar
    int num_x = 32, num_y = 32, num_levels = 3, routing_mode = 1, io_spill = 2;
    int data_width = 64, addr_width = 32;
};

struct Packet {
    uint64_t id = 0; // Opaque transaction/packet handle, never used for routing.
    int dst = 0, src = 0, tag = 0, seq = 0, kind = 0;
};

// common_cells spill_register is a registered two-entry FIFO. In particular,
// a full register CANNOT accept on the same cycle it drains (no ready bypass).
struct Spill {
    Packet data[2];
    unsigned head = 0, count = 0;
    bool ready() const { return count < 2; }
    const Packet &front() const { return data[head]; }
    void push(const Packet &p) {
        if (!ready()) throw std::logic_error("spill overflow");
        data[(head + count) & 1] = p; ++count;
    }
    void pop() {
        if (!count) throw std::logic_error("spill underflow");
        head ^= 1; --count;
    }
};

inline int ipow(int base, int exponent) {
    int result = 1;
    while (exponent--) {
        if (result > INT32_MAX / base) throw std::invalid_argument("topology too large");
        result *= base;
    }
    return result;
}

// rr_arb_tree is NOT a rotating linear priority encoder. Each binary tree
// level uses one bit of rr. FairArb advances to the next REQUEST after rr,
// not after the winner. LockIn snapshots all request bits, including losers.
struct Arbiter {
    unsigned rr = 0, locked = 0;
    static unsigned choose(unsigned mask, unsigned priority, int width) {
        unsigned base = 0;
        for (int half = 1 << (width-1); half; half >>= 1) {
            unsigned lo = mask & ((1u << half) - 1);
            unsigned hi = mask >> half;
            bool upper = !lo || (hi && (priority & half));
            if (upper) { base += half; mask = hi; } else mask = lo;
        }
        return base;
    }
    int eval(unsigned requests, bool ready, int bits) {
        unsigned mask = locked ? locked : requests;
        if (!mask) return -1;
        int winner = choose(mask, rr, bits);
        if (ready) {
            unsigned upper = mask & (~0u << (rr + 1));
            rr = __builtin_ctz(upper ? upper : mask);
            locked = 0;
        } else locked = mask;
        return winner;
    }
};

class Network {
public:
    explicit Network(Config config) : cfg(config) {
        if (cfg.fabric < 0 || cfg.fabric > 2 || cfg.routing_mode < 0 || cfg.routing_mode > 2 ||
            cfg.io_spill < 1 || cfg.io_spill > 32 || cfg.data_width < 1 || cfg.addr_width < 1 ||
            cfg.num_x < 1 || cfg.num_y < 1)
            throw std::invalid_argument("invalid network parameters");
        if (cfg.fabric == 0) {
            if (cfg.num_levels < 3 || cfg.num_levels > 6 || cfg.io_spill != 2)
                throw std::invalid_argument("fat tree needs 3..6 levels and exactly two spills per side");
            auto shape = fattree_shape(cfg.num_levels);
            if (cfg.num_x != shape.first || cfg.num_y != shape.second)
                throw std::invalid_argument("fat-tree dimensions do not match NumLevels");
        }
        int64_t n = int64_t(cfg.num_x) * cfg.num_y;
        if (n > 1048576) throw std::invalid_argument("network exceeds one million terminals");
        terminals = int(n);
        for (int v = terminals-1; v; v >>= 1) ++dst_bits;
        if (cfg.addr_width < dst_bits) throw std::invalid_argument("address cannot hold terminal ID");
        endpoint.resize(terminals);
        offered.resize(terminals); pending.assign(terminals, false); taken.assign(terminals, false);
        if (cfg.fabric == 2) {
            build_xbar();
            moves.reserve(pipes.size());
            return;
        }
        radix = cfg.fabric ? 5 : 16;
        int per_level = cfg.fabric ? 0 : 2 * ipow(8, cfg.num_levels-1);
        int count = cfg.fabric ? terminals : per_level * (cfg.num_levels-1) + per_level/2;
        routers.resize(count);
        pipes.resize(size_t(count)*radix*cfg.io_spill*2);
        for (int r=0; r<count; ++r) {
            auto &router = routers[r];
            router.level = cfg.fabric ? 0 : r / per_level + 1;
            router.index = cfg.fabric ? r : r % per_level;
            router.route_rr.resize(radix); router.route_lock.assign(radix, -1);
            router.arbiters.resize(radix);
            for (int p=0; p<radix; ++p) {
                router.route_rr[p] = p & 7;
                for (int side=0; side<2; ++side)
                    for (int s=0; s+1<cfg.io_spill; ++s)
                        edges.emplace_back(pipe(r,p,side,s), pipe(r,p,side,s+1));
            }
        }
        if (cfg.fabric) build_mesh(); else build_fattree(per_level);
        // Precompute static downward decisions and hash bits once, not per cycle.
        dest_tile.resize(terminals); dest_port.resize(terminals); dest_hash.resize(terminals);
        for (int d=0; d<terminals; ++d) {
            dest_tile[d] = cfg.fabric ? 0 : tile(d / cfg.num_y, d % cfg.num_y);
            static const int ports[] = {1,2,0,3,4,7,5,6};
            dest_port[d] = ports[((d / cfg.num_y) % 4)*2 + (d % cfg.num_y)%2];
            int hash=0;
            for (int b=0; b<dst_bits; ++b) hash ^= ((d >> b)&1) << (b%3);
            dest_hash[d]=hash;
        }
        moves.reserve(pipes.size());
    }
    static std::pair<int,int> fattree_shape(int levels) {
        int x=4,y=2;
        for (int l=2; l<=levels; ++l) (l%2 ? y : x) *= l==levels ? 16 : 8;
        return {x,y};
    }
    int size() const { return terminals; }
    int router_count() const { return cfg.fabric == 2 ? 1 : int(routers.size()); }
    uint64_t occupancy() const { return resident; }
    bool can_offer(int src) const {
        return !pending.at(src) && pipes[endpoint[src].first].ready();
    }
    bool offer(int src, const Packet &p) {
        if (p.dst < 0 || p.dst >= terminals) throw std::invalid_argument("destination outside network");
        if (!can_offer(src)) return false;
        pending[src]=true; offered[src]=p; return true;
    }
    const Packet *peek(int dst) const {
        const auto &q = pipes[endpoint.at(dst).second];
        return q.count ? &q.front() : nullptr;
    }
    void take(int dst) {
        if (!peek(dst) || taken.at(dst)) throw std::logic_error("invalid ejection");
        taken[dst]=true;
    }
    void reset() {
        for (auto &q:pipes) q.count=q.head=0;
        for (auto &r:routers) {
            for (int p=0;p<radix;++p) { r.arbiters[p]={}; r.route_rr[p]=p&7; r.route_lock[p]=-1; }
        }
        std::fill(xbar_rr.begin(),xbar_rr.end(),0);
        std::fill(xbar_locked_input.begin(),xbar_locked_input.end(),false);
        std::fill(xbar_locked_output.begin(),xbar_locked_output.end(),false);
        std::fill(pending.begin(),pending.end(),false);
        std::fill(taken.begin(),taken.end(),false); resident=0;
    }
    void step() {
        moves.clear();
        for (const auto &e:edges)
            if (pipes[e.first].count && pipes[e.second].ready()) moves.push_back(e);
        if (cfg.fabric == 2) arbitrate_xbar();
        for (int ri=0;ri<int(routers.size());++ri) {
            auto &r=routers[ri];
            unsigned requests[16]={}; int selections[16];
            int costs[16]={};
            if (!cfg.fabric && cfg.routing_mode==2)
                for (int p=0;p<radix;++p)
                    for (int s=0;s<cfg.io_spill;++s) costs[p]+=pipes[pipe(ri,p,1,s)].count;
            for (int p=0;p<radix;++p) {
                auto &q=pipes[pipe(ri,p,0,cfg.io_spill-1)];
                selections[p]=-1;
                if (!q.count) continue;
                int selected=route(ri,p,q.front().dst,costs);
                selections[p]=selected;
                requests[selected] |= 1u << p;
            }
            unsigned accepted=0;
            for (int out=0;out<radix;++out) {
                int op=pipe(ri,out,1,0);
                int winner=r.arbiters[out].eval(requests[out],pipes[op].ready(),cfg.fabric ? 3 : 4);
                if (winner>=0 && pipes[op].ready()) {
                    moves.emplace_back(pipe(ri,winner,0,cfg.io_spill-1),op);
                    accepted |= 1u << winner;
                }
            }
            if (!cfg.fabric && cfg.routing_mode==2)
                for (int p=0;p<radix;++p) if (selections[p]>=0) {
                    if (accepted & (1u<<p)) {
                        r.route_lock[p]=-1;
                        if (r.level<cfg.num_levels && selections[p]>=8) r.route_rr[p]=(selections[p]+1)&7;
                    } else if (r.route_lock[p]<0) r.route_lock[p]=selections[p];
                }
        }
        // Snapshot every transferred payload before modifying any register.
        payloads.clear();
        for (auto &m:moves) payloads.push_back(pipes[m.first].front());
        for (auto &m:moves) pipes[m.first].pop();
        for (int d=0;d<terminals;++d) if (taken[d]) {
            pipes[endpoint[d].second].pop(); taken[d]=false; --resident;
        }
        for (size_t i=0;i<moves.size();++i) pipes[moves[i].second].push(payloads[i]);
        for (int s=0;s<terminals;++s) if (pending[s]) {
            pipes[endpoint[s].first].push(offered[s]); pending[s]=false; ++resident;
        }
    }
private:
    struct Router {
        int level, index;
        std::vector<int> route_rr, route_lock;
        std::vector<Arbiter> arbiters;
    };
    Config cfg;
    int radix, terminals, dst_bits=0;
    uint64_t resident=0;
    std::vector<Router> routers;
    std::vector<Spill> pipes;
    std::vector<std::pair<int,int>> edges,endpoint,moves;
    std::vector<Packet> offered,payloads;
    std::vector<bool> pending,taken;
    std::vector<int> dest_tile,dest_port,dest_hash;
    // Each input head requests exactly one output. Linked request lists and
    // one lock bit per input avoid an N-by-N request/lock matrix.
    std::vector<int> xbar_head,xbar_next;
    std::vector<unsigned> xbar_rr;
    std::vector<bool> xbar_locked_input,xbar_locked_output;
    int pipe(int r,int p,int side,int s) const {
        return ((r*radix+p)*2+side)*cfg.io_spill+s;
    }
    void connect(int a,int ap,int b,int bp) {
        edges.emplace_back(pipe(a,ap,1,cfg.io_spill-1),pipe(b,bp,0,0));
        edges.emplace_back(pipe(b,bp,1,cfg.io_spill-1),pipe(a,ap,0,0));
    }
    int tile(int x,int y) const {
        int xs=1,ys=1,prior=1,result=0;
        for (int l=2;l<=cfg.num_levels;++l) {
            int dup=l==cfg.num_levels?16:8;
            if (l%2==0) { result+=((x/4/xs)%dup)*prior; xs*=dup; }
            else { result+=((y/2/ys)%dup)*prior; ys*=dup; }
            prior*=dup;
        }
        return result;
    }
    void build_xbar() {
        radix=terminals;
        pipes.resize(size_t(terminals)*2*cfg.io_spill);
        xbar_head.resize(terminals); xbar_next.resize(terminals);
        xbar_rr.assign(terminals,0);
        xbar_locked_input.assign(terminals,false);
        xbar_locked_output.assign(terminals,false);
        for (int p=0;p<terminals;++p) {
            endpoint[p]={pipe(0,p,0,0),pipe(0,p,1,cfg.io_spill-1)};
            for (int side=0;side<2;++side)
                for (int s=0;s+1<cfg.io_spill;++s)
                    edges.emplace_back(pipe(0,p,side,s),pipe(0,p,side,s+1));
        }
    }
    void arbitrate_xbar() {
        std::fill(xbar_head.begin(),xbar_head.end(),-1);
        for (int src=0;src<terminals;++src) {
            const auto &q=pipes[pipe(0,src,0,cfg.io_spill-1)];
            if (!q.count) continue;
            int dst=q.front().dst;
            xbar_next[src]=xbar_head[dst]; xbar_head[dst]=src;
        }
        for (int dst=0;dst<terminals;++dst) {
            int winner=-1,first=terminals,next=terminals;
            unsigned rr=xbar_rr[dst];
            bool locked=xbar_locked_output[dst];
            for (int src=xbar_head[dst];src>=0;src=xbar_next[src]) {
                if (locked && !xbar_locked_input[src]) continue;
                // Equivalent to rr_arb_tree's binary priority selection, also
                // for non-power-of-two port counts padded with inactive leaves.
                if (winner<0 || (unsigned(src)^rr)<(unsigned(winner)^rr)) winner=src;
                first=std::min(first,src);
                if (unsigned(src)>rr) next=std::min(next,src);
            }
            if (winner<0) continue;
            int output=pipe(0,dst,1,0);
            bool ready=pipes[output].ready();
            if (ready) {
                moves.emplace_back(pipe(0,winner,0,cfg.io_spill-1),output);
                // FairArb advances past the previous priority, not the winner.
                xbar_rr[dst]=next<terminals?next:first;
            }
            if (ready || !locked) {
                // LockIn snapshots all current contenders on the first stall.
                // A locked input cannot leave until this output grants a beat.
                for (int src=xbar_head[dst];src>=0;src=xbar_next[src])
                    xbar_locked_input[src]=!ready;
                xbar_locked_output[dst]=!ready;
            }
        }
    }
    void build_mesh() {
        for (int x=0;x<cfg.num_x;++x) for (int y=0;y<cfg.num_y;++y) {
            int r=x*cfg.num_y+y;
            endpoint[r]={pipe(r,0,0,0),pipe(r,0,1,cfg.io_spill-1)};
            if (x+1<cfg.num_x) connect(r,2,r+cfg.num_y,4);
            if (y+1<cfg.num_y) connect(r,1,r+1,3);
        }
    }
    void build_fattree(int n) {
        static const int ports[]={1,2,0,3,4,7,5,6};
        for (int x=0;x<cfg.num_x;++x) for (int y=0;y<cfg.num_y;++y) {
            int r=tile(x,y),p=ports[(x%4)*2+y%2];
            endpoint[x*cfg.num_y+y]={pipe(r,p,0,0),pipe(r,p,1,cfg.io_spill-1)};
        }
        for (int l=1;l<cfg.num_levels;++l) for (int r=0;r<n;++r) for (int p=0;p<8;++p) {
            int up,port;
            if (l+1==cfg.num_levels) {
                bool east=r>=n/2; int ordered=east?n-1-r:r;
                int linear=ordered*4+p%4,seq=linear%(n/4);
                up=p/4 ? n/2-1-seq : seq; port=(east?8:0)+linear/(n/4);
            } else {
                int group=ipow(8,l),half=group/2,local=r%group;
                bool east=local>=half; int ordered=east?group-1-local:local;
                int linear=ordered*4+p%4,seq=linear%half;
                up=(r/group)*group+(p/4?group-1-seq:seq);
                port=(east?4:0)+linear/half;
            }
            connect((l-1)*n+r,8+p,l*n+up,port);
        }
    }
    int route(int ri,int ingress,int dst,const int *costs) const {
        const auto &r=routers[ri];
        if (cfg.fabric) {
            int x=ri/cfg.num_y,y=ri%cfg.num_y,dx=dst/cfg.num_y,dy=dst%cfg.num_y;
            return dx>x?2:dx<x?4:dy>y?1:dy<y?3:0;
        }
        if (r.route_lock[ingress]>=0) return r.route_lock[ingress];
        int group=ipow(8,r.level-1),child=r.level>1?group/8:1;
        int down=r.level==cfg.num_levels?16:8;
        if (r.level==cfg.num_levels || dest_tile[dst]/group==r.index/group) {
            if (r.level==1) return dest_port[dst];
            int branch=(dest_tile[dst]/child)%down;
            return branch<down/2 ? branch : down+down/2-1-branch;
        }
        if (ingress>=8) throw std::logic_error("illegal down-to-up turn");
        if (cfg.routing_mode==0) return 8;
        if (cfg.routing_mode==1) return 8+((ingress^r.index^(r.index>>3)^r.level^dest_hash[dst])&7);
        int selected=8+r.route_rr[ingress];
        for (int k=1;k<8;++k) {
            int p=8+((r.route_rr[ingress]+k)&7);
            if (costs[p]<costs[selected]) selected=p;
        }
        return selected;
    }
};
}
