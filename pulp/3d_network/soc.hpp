// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "network.hpp"
#include <functional>
#include <map>

namespace network3d {
enum Kind { AW, W, B, AR, R, ReserveRead, ReserveWrite, Grant };

struct SocConfig {
    Config network;
    int source_contexts=4, memory_contexts=4, max_burst_beats=256;
    int axi_addr_width=32, axi_data_width=64, axi_id_width=10, axi_len_width=8;
    uint64_t memory_base=0, interleave_bytes=4096, memory_bytes=4096;
};

struct Transaction {
    uint64_t id=0, address=0, local_address=0, size=0;
    int source=0,destination=0,beats=1,memory_tag=-1;
    bool write=false, error=false;
    void *opaque=nullptr;
};

class Soc {
public:
    // The memory port issues one AR (sequence -1), or one W beat at a time.
    // Endpoint responses enter through memory_response; only the endpoint
    // controls their order and availability. No memory service policy lives here.
    // respond is called for the final ordered R or B; false holds the context.
    std::function<bool(Transaction&,int)> issue;
    std::function<void(Transaction&,int)> memory_response_accepted;
    std::function<bool(Transaction&)> respond;
    explicit Soc(SocConfig config) : cfg(config),network(cfg.network),n(network.size()) {
        if (cfg.source_contexts<1 || cfg.memory_contexts<1 ||
            cfg.max_burst_beats<1 || cfg.max_burst_beats>256 || cfg.axi_len_width<1 ||
            cfg.axi_len_width>8 || cfg.max_burst_beats>(1<<cfg.axi_len_width) ||
            cfg.axi_addr_width<1 || cfg.axi_addr_width>64 || cfg.axi_id_width<1 ||
            cfg.axi_data_width<8 || cfg.axi_data_width>1024 ||
            (cfg.axi_data_width&(cfg.axi_data_width-1)) ||
            cfg.interleave_bytes<unsigned(cfg.axi_data_width/8) ||
            (cfg.interleave_bytes&(cfg.interleave_bytes-1)) || !cfg.memory_bytes ||
            cfg.memory_bytes%cfg.interleave_bytes || cfg.memory_base%cfg.interleave_bytes)
            throw std::invalid_argument("invalid SoC context/width/memory parameters");
        __uint128_t end=__uint128_t(cfg.memory_base)+__uint128_t(n)*cfg.memory_bytes;
        if (end>(__uint128_t(1)<<cfg.axi_addr_width)) throw std::invalid_argument("memory map overflows address width");
        terminals.resize(n); pending.resize(n,nullptr); pending_slot.resize(n);
        for (auto &t:terminals) {
            t.source.resize(cfg.source_contexts); t.memory.resize(cfg.memory_contexts);
            t.peer_busy.resize(n,false);
        }
    }
    int size() const { return n; }
    uint64_t now() const { return cycle; }
    uint64_t outstanding() const { return active; }
    uint64_t payload_packets=0,control_packets=0;
    void memory_response(Transaction &tx,int sequence) {
        auto &t=terminals.at(tx.destination);
        auto &m=t.memory.at(tx.memory_tag);
        auto &r=tx.write?t.b_response:t.r_response;
        if (m.tx!=&tx || r.tx || sequence<0 || sequence>=(tx.write?1:tx.beats))
            throw std::logic_error("invalid or overlapping memory response");
        r={&tx,sequence};
    }
    bool can_offer(int source) const {
        if (pending.at(source)) return false;
        for (const auto &c:terminals[source].source) if (!c.tx) return true;
        return false;
    }
    bool offer(int source,Transaction &tx) {
        if (!can_offer(source)) return false;
        tx.source=source; int bytes=cfg.axi_data_width/8;
        tx.error=!tx.size || tx.size>uint64_t(cfg.max_burst_beats)*bytes;
        tx.beats=tx.error ? 1 : int((tx.size+bytes-1)/bytes);
        __uint128_t end=__uint128_t(tx.address)+tx.size;
        tx.error=tx.error || tx.address<cfg.memory_base ||
            end>__uint128_t(cfg.memory_base)+__uint128_t(n)*cfg.memory_bytes ||
            end>(__uint128_t(1)<<cfg.axi_addr_width);
        if (!tx.error) {
            uint64_t a=tx.address-cfg.memory_base;
            tx.error=(a/cfg.interleave_bytes)!=((a+tx.size-1)/cfg.interleave_bytes);
            tx.destination=(a/cfg.interleave_bytes)%n;
            tx.local_address=((a/cfg.interleave_bytes)/n)*cfg.interleave_bytes+a%cfg.interleave_bytes;
        }
        auto &s=terminals[source].source;
        for (int i=0;i<cfg.source_contexts;++i) if (!s[i].tx) { pending_slot[source]=i; break; }
        pending[source]=&tx; ++active; return true;
    }
    void reset() {
        network.reset(); cycle=active=payload_packets=control_packets=0;
        for (auto &t:terminals) {
            t=Terminal{}; t.source.resize(cfg.source_contexts); t.memory.resize(cfg.memory_contexts);
            t.peer_busy.resize(n,false);
        }
        std::fill(pending.begin(),pending.end(),nullptr);
    }
    void step() {
        received.clear();
        for (int ep=0;ep<n;++ep) {
            auto &t=terminals[ep];
            if (auto p=network.peek(ep)) { received.push_back(*p); network.take(ep); }
            bool output_ready=t.output.ready();
            bool output_take=t.output.count && network.offer(ep,t.output.front());

            // Snapshot candidates from the PRE-edge state, including free slots.
            int free_memory=-1,ar=-1,wr=-1,sc=-1;
            for (int k=0;k<cfg.memory_contexts;++k) {
                if (!t.memory[k].tx && free_memory<0) free_memory=k;
                int p=(t.ar_rr+k)%cfg.memory_contexts; auto &m=t.memory[p];
                if (m.tx && !m.tx->write && m.descriptor && !m.issued && ar<0) ar=p;
                if (t.memory_writes) {
                    p=(t.write_rr+k)%cfg.memory_contexts; auto &w=t.memory[p];
                    if (w.tx && w.tx->write && w.descriptor && !w.issued &&
                        w.received==w.tx->beats && wr<0) wr=p;
                }
            }
            if (t.ar_owner>=0) ar=t.ar_owner;
            bool reply_read=t.r_response.tx && (!t.b_response.tx || !t.reply_rr);
            auto response=reply_read?t.r_response:t.b_response;
            int reply_tag=response.tx?response.tx->memory_tag:-1;
            Packet source_packet,reply_packet,grant_packet;
            for (int k=0;k<cfg.source_contexts;++k) {
                int p=(t.tx_rr+k)%cfg.source_contexts; auto &s=t.source[p];
                if (!s.tx || s.tx->error) continue;
                int kind=-1;
                if (!s.requested && !t.peer_busy[s.tx->destination]) kind=s.tx->write?ReserveWrite:ReserveRead;
                else if (s.granted && !s.descriptor_sent) kind=s.tx->write?AW:AR;
                else if (s.tx->write && s.descriptor_sent && s.w_sent<s.w_available) kind=W;
                if (kind>=0) {
                    sc=p; source_packet=make(s.tx,ep,s.tx->destination,
                        kind==ReserveRead || kind==ReserveWrite?p:s.remote_tag,kind==W?s.w_sent:0,kind); break;
                }
            }
            if (reply_tag>=0) {
                auto &m=t.memory[reply_tag];
                reply_packet=make(m.tx,ep,m.tx->source,m.source_tag,
                    response.sequence,reply_read?R:B);
            }
            // Reservation arbitration uses the same binary priority tree and
            // request-set lock as common_cells, over a sparse pending bank.
            const auto &bank=t.res_locked.empty()?t.reservations:t.res_locked;
            int peer=-1,next_rr=0;
            if (!bank.empty()) {
                unsigned best=~0u;
                for (auto &entry:bank) if ((unsigned(entry.first)^t.res_rr)<best) {
                    best=unsigned(entry.first)^t.res_rr; peer=entry.first;
                }
                auto next=bank.upper_bound(t.res_rr);
                next_rr=(next==bank.end()?bank.begin():next)->first;
                if (free_memory>=0) grant_packet=make(bank.at(peer).tx,ep,peer,
                    bank.at(peer).source_tag,free_memory,Grant);
            }
            int choice=-1;
            for (int k=0;k<3;++k) {
                int p=(t.tx_kind_rr+k)%3;
                if ((p==0 && sc>=0) || (p==1 && reply_tag>=0) || (p==2 && peer>=0 && free_memory>=0)) {
                    choice=p; break;
                }
            }
            bool accept=choice>=0 && output_ready;
            bool grant=accept && choice==2;
            if (peer>=0 && !grant && t.res_locked.empty()) t.res_locked=t.reservations;

            // Ordered R and B channels have separate arbiters. A completed
            // response never frees a source slot early, even if LAST arrived first.
            for (int kind=0;kind<2;++kind) {
                if (kind && !t.source_writes) continue;
                int &rr=kind?t.b_rr:t.r_rr;
                int &lock=kind?t.b_lock:t.r_lock;
                for (int k=0;k<cfg.source_contexts;++k) {
                    int p=lock>=0?lock:(rr+k)%cfg.source_contexts; auto &s=t.source[p];
                    if (!s.tx || s.tx->write!=bool(kind)) continue;
                    bool valid=kind ? s.b_seen || (s.tx->error && s.w_available>=s.tx->beats)
                        : s.tx->error || s.present[s.r_next];
                    if (!valid) continue;
                    bool last=kind || s.r_next+1==s.tx->beats;
                    if (!last || !respond || respond(*s.tx)) {
                        ++s.r_next; rr=(p+1)%cfg.source_contexts; lock=-1;
                        if (last) { if (kind) --t.source_writes; s.tx=nullptr; --active; }
                    } else lock=p;
                    break;
                }
            }

            if (accept) {
                t.tx_kind_rr=(choice+1)%3;
                if (choice==0) {
                    auto &s=t.source[sc]; t.tx_rr=(sc+1)%cfg.source_contexts;
                    if (source_packet.kind==ReserveRead || source_packet.kind==ReserveWrite) {
                        s.requested=true; t.peer_busy[s.tx->destination]=true;
                    } else if (source_packet.kind==W) ++s.w_sent;
                    else s.descriptor_sent=true;
                } else if (choice==1) {
                    auto &m=t.memory[reply_tag]; t.reply_rr=reply_read;
                    auto tx=m.tx;
                    (reply_read?t.r_response:t.b_response)={};
                    if (!reply_read || response.sequence+1==tx->beats) m.tx=nullptr;
                    if (!reply_read) --t.memory_writes;
                    if (memory_response_accepted) memory_response_accepted(*tx,response.sequence);
                } else {
                    auto request=bank.at(peer);
                    t.memory[free_memory]=Memory{};
                    t.memory[free_memory].tx=request.tx;
                    t.memory[free_memory].source_tag=request.source_tag;
                    request.tx->memory_tag=free_memory;
                    if (request.tx->write) ++t.memory_writes;
                    t.reservations.erase(peer); t.res_locked.clear(); t.res_rr=next_rr;
                }
            }
            // The AR selection stays locked while the external endpoint stalls.
            if (ar>=0) {
                auto &m=t.memory[ar];
                if (issue && issue(*m.tx,-1)) {
                    m.issued=true; t.ar_owner=-1; t.ar_rr=(ar+1)%cfg.memory_contexts;
                } else t.ar_owner=ar;
            }
            // The sink NI reassembles W, then holds ownership while actual W
            // beats cross the memory port. AW setup/B timing belong to its slave.
            if (t.write_owner<0) t.write_owner=wr;
            else {
                auto &m=t.memory[t.write_owner];
                if (issue && issue(*m.tx,m.w_next) && ++m.w_next==m.tx->beats) {
                    m.issued=true;
                    t.write_rr=(t.write_owner+1)%cfg.memory_contexts; t.write_owner=-1;
                }
            }
            if (t.source_writes)
                for (auto &s:t.source) if (s.tx && s.tx->write && s.w_available<s.tx->beats) ++s.w_available;
            if (output_take) t.output.pop();
            if (accept) {
                Packet p=choice==0?source_packet:choice==1?reply_packet:grant_packet;
                t.output.push(p);
                if (p.kind>=ReserveRead) ++control_packets; else ++payload_packets;
            }
        }
        network.step();
        // RX writes are registered and cannot affect the choices made above.
        for (const auto &p:received) {
            auto &t=terminals[p.dst];
            if (p.kind==ReserveRead || p.kind==ReserveWrite) {
                if (t.reservations.count(p.src)) throw std::logic_error("reservation overflow");
                t.reservations[p.src]={lookup(p),p.tag};
            } else if (p.kind==Grant) {
                auto &s=t.source.at(p.tag);
                s.granted=true; s.remote_tag=p.seq; t.peer_busy[p.src]=false;
            } else if (p.kind==AR || p.kind==AW) t.memory.at(p.tag).descriptor=true;
            else if (p.kind==W) ++t.memory.at(p.tag).received;
            else if (p.kind==R) {
                auto &s=t.source.at(p.tag);
                if (s.present.at(p.seq)) throw std::logic_error("duplicate R sequence");
                s.present[p.seq]=true;
            } else t.source.at(p.tag).b_seen=true;
        }
        for (int ep=0;ep<n;++ep) if (pending[ep]) {
            auto &s=terminals[ep].source[pending_slot[ep]]; s=Source{}; s.tx=pending[ep];
            s.present.assign(s.tx->beats,false); s.w_available=s.tx->write?1:0;
            if (s.tx->write) ++terminals[ep].source_writes;
            pending[ep]=nullptr;
        }
        ++cycle;
    }
private:
    struct Source {
        Transaction *tx=nullptr;
        bool requested=false,granted=false,descriptor_sent=false,b_seen=false;
        int remote_tag=0,w_sent=0,w_available=0,r_next=0;
        std::vector<bool> present;
    };
    struct Memory { Transaction *tx=nullptr; int source_tag=0,received=0,w_next=0; bool descriptor=false,issued=false; };
    struct Response { Transaction *tx=nullptr; int sequence=0; };
    struct Reservation { Transaction *tx; int source_tag; };
    struct Terminal {
        std::vector<Source> source;
        std::vector<Memory> memory;
        Response r_response,b_response;
        std::vector<bool> peer_busy;
        Spill output;
        int tx_rr=0,tx_kind_rr=0,r_rr=0,b_rr=0,ar_rr=0,ar_owner=-1,write_rr=0,write_owner=-1;
        bool reply_rr=false;
        int source_writes=0,memory_writes=0;
        int r_lock=-1,b_lock=-1;
        unsigned res_rr=0;
        std::map<int,Reservation> reservations,res_locked;
    };
    SocConfig cfg; Network network; int n;
    uint64_t cycle=0,active=0;
    std::vector<Terminal> terminals;
    std::vector<Transaction*> pending;
    std::vector<int> pending_slot;
    std::vector<Packet> received;
    Packet make(Transaction *tx,int src,int dst,int tag,int seq,int kind) {
        Packet p; p.id=reinterpret_cast<uintptr_t>(tx); p.src=src;p.dst=dst;p.tag=tag;p.seq=seq;p.kind=kind;return p;
    }
    Transaction *lookup(const Packet &p) { return reinterpret_cast<Transaction*>(uintptr_t(p.id)); }
};
}
