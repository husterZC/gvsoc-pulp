// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../i3d.hpp"
#include "../memory_endpoint.hpp"
#include <memory>
#include <unordered_map>

// Standalone harness for the same external endpoint policy used by GVSoC.
// I3d only sees request handshakes and externally offered R/B responses.
class TestMemory {
    struct Job : network3d::MemoryRequest { network3d::Transaction *tx; };
    struct Endpoint {
        network3d::MemoryEndpointTiming timing;
        Job *writer=nullptr;
        bool r_valid=false,b_valid=false;
        explicit Endpoint(int slots) : timing(slots) {}
    };
    network3d::I3d &i3d;
    std::vector<Endpoint> endpoints;
    std::unordered_map<network3d::Transaction*,std::unique_ptr<Job>> jobs;
public:
    std::function<unsigned(network3d::Transaction&)> access;
    TestMemory(network3d::I3d &i3d,int slots) : i3d(i3d) {
        for (int i=0;i<i3d.size();++i) endpoints.emplace_back(slots);
        i3d.issue=[this](auto &tx,int sequence) {
            auto &ep=endpoints[tx.destination]; auto now=this->i3d.now();
            if (sequence<0) {
                if (!ep.timing.can_read(now)) return false;
                auto job=std::make_unique<Job>(); job->tx=&tx; job->beats=tx.beats;
                job->ready_cycle=now+(access?access(tx):0); job->ready=true;
                ep.timing.read(*job,now); jobs[&tx]=std::move(job); return true;
            }
            if (!ep.writer) {
                if (ep.timing.can_write(now)) {
                    auto job=std::make_unique<Job>(); job->tx=&tx; job->beats=tx.beats;
                    ep.writer=job.get(); ep.timing.start_write(*job,now); jobs[&tx]=std::move(job);
                }
                return false;
            }
            if (ep.writer->tx!=&tx || !ep.timing.can_write_beat(now)) return false;
            bool last=sequence+1==tx.beats; ep.timing.write_beat(last,now);
            if (last) {
                ep.writer->ready_cycle=now+(access?access(tx):0); ep.writer->ready=true;
            }
            return true;
        };
        i3d.memory_response_accepted=[this](auto &tx,int sequence) {
            auto &ep=endpoints[tx.destination]; auto now=this->i3d.now();
            if (tx.write) { ep.timing.write_accepted(now); ep.writer=nullptr; ep.b_valid=false; }
            else { ep.timing.read_accepted(now); ep.r_valid=false; }
            if (tx.write || sequence+1==tx.beats) jobs.erase(&tx);
        };
    }
    void step() {
        for (auto &ep:endpoints) {
            if (!ep.r_valid) {
                auto r=ep.timing.read_response(i3d.now());
                if (r.request) { i3d.memory_response(*static_cast<Job*>(r.request)->tx,r.sequence); ep.r_valid=true; }
            }
            if (!ep.b_valid) if (auto r=ep.timing.write_response(i3d.now())) {
                i3d.memory_response(*static_cast<Job*>(r)->tx,0); ep.b_valid=true;
            }
        }
    }
    void reset() {
        for (auto &ep:endpoints) { ep.timing.reset(); ep.writer=nullptr; ep.r_valid=ep.b_valid=false; }
        jobs.clear();
    }
};
