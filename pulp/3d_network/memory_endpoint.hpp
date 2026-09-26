// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace network3d {

// Service policy of verification/soc/axi_sim_mem.sv. This independent endpoint
// knows nothing about NoC contexts, routing, source tags or address interleaving.
// Its owner supplies storage completion and transports responses over IO_v2.
struct MemoryRequest {
    int beats=1;
    bool ready=false;
    uint64_t ready_cycle=0;
};

class MemoryEndpointTiming {
public:
    struct Response { MemoryRequest *request=nullptr; int sequence=0; };
    explicit MemoryEndpointTiming(int read_slots) : reads(read_slots) {
        if (read_slots<1) throw std::invalid_argument("read_slots must be positive");
    }
    bool can_read(uint64_t now) const {
        for (const auto &r:reads) if (!r.request && r.available<=now) return true;
        return false;
    }
    bool read(MemoryRequest &request,uint64_t now) {
        for (auto &r:reads) if (!r.request && r.available<=now) {
            r.request=&request; r.sequence=0; r.available=now+1; return true;
        }
        return false;
    }
    Response read_response(uint64_t now) {
        if (read_lock<0) for (unsigned k=0;k<reads.size();++k) {
            int p=(read_rr+k)%reads.size(); auto &r=reads[p];
            if (r.request && r.available<=now && r.request->ready && r.request->ready_cycle<=now) {
                read_lock=p; break;
            }
        }
        if (read_lock<0) return {};
        auto &r=reads[read_lock]; return {r.request,r.sequence};
    }
    void read_accepted(uint64_t now) {
        if (read_lock<0) throw std::logic_error("R without a selected read");
        auto &r=reads[read_lock];
        if (++r.sequence==r.request->beats) { r.request=nullptr; r.available=now+1; }
        read_rr=(read_lock+1)%reads.size(); read_lock=-1;
    }
    bool can_write(uint64_t now) const { return !writer && write_available<=now; }
    bool start_write(MemoryRequest &request,uint64_t now) {
        if (!can_write(now)) return false;
        writer=&request; write_available=now+1; write_complete=false; return true;
    }
    bool can_write_beat(uint64_t now) const {
        return writer && !write_complete && write_available<=now;
    }
    void write_beat(bool last,uint64_t now) {
        if (!can_write_beat(now)) throw std::logic_error("W without AW or while stalled");
        write_available=now+1; write_complete=last;
    }
    MemoryRequest *write_response(uint64_t now) const {
        return writer && write_complete && write_available<=now && writer->ready &&
            writer->ready_cycle<=now ? writer : nullptr;
    }
    void write_accepted(uint64_t now) {
        if (!write_response(now)) throw std::logic_error("B without completed write");
        writer=nullptr; write_available=now+1;
    }
    void reset() {
        std::fill(reads.begin(),reads.end(),Read{}); read_rr=0; read_lock=-1;
        writer=nullptr; write_available=0; write_complete=false;
    }
private:
    struct Read { MemoryRequest *request=nullptr; int sequence=0; uint64_t available=0; };
    std::vector<Read> reads;
    int read_rr=0,read_lock=-1;
    MemoryRequest *writer=nullptr;
    uint64_t write_available=0;
    bool write_complete=false;
};
}
