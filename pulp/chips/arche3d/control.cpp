// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include "dma_access.hpp"
#include "icache_preload.hpp"

class Control : public vp::Component
{
    struct Stats
    {
        uint64_t issued = 0, completed = 0, bytes = 0, expected = 0, burst_bytes = 0;
        std::set<unsigned> ids;
        bool exited = false;
    };
    std::vector<vp::IoSlave> inputs;
    std::vector<vp::WireSlave<Arche3dDmaEvent>> activities;
    vp::WireMaster<bool> ready;
    vp::WireMaster<Arche3dIcachePreload> cache_preload;
    std::vector<vp::WireSlave<uint64_t>> cache_refills;
    std::vector<uint64_t> refill_counts;
    uint64_t refills_at_boot = 0;
    uint64_t total_refills()
    {
        uint64_t count = 0;
        for (auto n : refill_counts)
        {
            count += n;
        }
        return count;
    }
    vp::ClockEvent boot, release, progress;
    vp::Trace trace;
    std::vector<vp::IoReq *> waiters;
    std::vector<vp::IoReq *> row_waiters, column_waiters;
    std::vector<unsigned> row_arrived, column_arrived;
    std::vector<uint64_t> row_generation, column_generation;
    std::vector<int64_t> row_due, column_due;
    struct MaskedBarrier
    {
        uint32_t masks = 0, anchor = 0;
        unsigned target = 0;
        int64_t due = -1;
        std::vector<unsigned> members;
    };
    // One outstanding masked arrival per cluster bounds simultaneous groups.
    std::vector<MaskedBarrier> masked_groups;
    std::vector<uint32_t> masked_config;
    std::vector<vp::IoReq *> masked_waiters;
    int64_t global_due = -1;
    std::vector<Stats> stats;
    unsigned nx, ny, count, arrived = 0, exited = 0;
    uint64_t image_bytes, preheat_lines, preheat_base;
    uint64_t dram_preloaded_bytes;
    std::vector<uint8_t> preheat_data;
    int64_t boot_cycle = -1;
    uint64_t memory_base, interleave, axi_bytes, issued = 0, completed = 0, bytes = 0, peak = 0;
    int64_t first = -1, last = -1, inject = -1, begin = -1, progress_cycles, watchdog;
    std::chrono::steady_clock::time_point wall;
    static void start(vp::Block *block, vp::ClockEvent *)
    {
        auto self = static_cast<Control *>(block);
        self->boot_cycle = self->clock.get_cycles();
        self->refills_at_boot = self->total_refills();
        printf("ARCHE3D_BOOT {\"clusters\":%u,\"image_bytes\":%lu,\"image_loaded_cycle\":0,"
               "\"image_load_mode\":\"direct\",\"dram_preloaded_bytes\":%lu,"
               "\"preheat_mode\":\"direct\",\"preheat_cycles\":0,"
               "\"preheat_lines_per_cluster\":%lu,\"cores_start_cycle\":%ld}\n",
               self->count, self->image_bytes, self->dram_preloaded_bytes, self->preheat_lines,
               self->boot_cycle);
        fflush(stdout);
        self->ready.sync(true);
    }
    static void refilled(vp::Block *block, uint64_t count, int cluster)
    {
        static_cast<Control *>(block)->refill_counts[cluster] = count;
    }
    static void release_barrier(vp::Block *block, vp::ClockEvent *)
    {
        auto self = static_cast<Control *>(block);
        int64_t now = self->clock.get_cycles();
        if (self->global_due >= 0 && self->global_due <= now)
        {
            auto waiters = self->waiters;
            std::fill(self->waiters.begin(), self->waiters.end(), nullptr);
            self->arrived = 0;
            self->global_due = -1;
            self->begin = now;
            for (auto req : waiters)
            {
                req->get_resp_port()->resp(req);
            }
        }
        for (bool column : {false, true})
        {
            auto &due = column ? self->column_due : self->row_due;
            auto &arrivals = column ? self->column_arrived : self->row_arrived;
            auto &generation = column ? self->column_generation : self->row_generation;
            auto &waiters = column ? self->column_waiters : self->row_waiters;
            for (unsigned group = 0; group < due.size(); ++group)
            {
                if (due[group] < 0 || due[group] > now)
                {
                    continue;
                }
                due[group] = -1;
                arrivals[group] = 0;
                ++generation[group];
                self->trace.msg(vp::Trace::LEVEL_DEBUG,
                                "BARRIER_RELEASE column=%d group=%u generation=%lu\n", column,
                                group, generation[group]);
                for (unsigned rank = 0; rank < (column ? self->ny : self->nx); ++rank)
                {
                    unsigned id = column ? rank * self->nx + group : group * self->nx + rank;
                    auto *req = waiters[id];
                    waiters[id] = nullptr;
                    req->get_resp_port()->resp(req);
                }
            }
        }
        for (auto &group : self->masked_groups)
        {
            if (group.due < 0 || group.due > now)
            {
                continue;
            }
            std::vector<vp::IoReq *> ready;
            for (auto id : group.members)
            {
                ready.push_back(self->masked_waiters[id]);
                self->masked_waiters[id] = nullptr;
            }
            self->trace.msg(vp::Trace::LEVEL_DEBUG,
                            "MASKED_BARRIER_RELEASE masks=0x%x anchor=0x%x members=%zu\n",
                            group.masks, group.anchor, ready.size());
            group = {};
            for (auto *req : ready)
            {
                req->get_resp_port()->resp(req);
            }
        }
        int64_t next = self->global_due;
        for (auto due : self->row_due)
        {
            if (due >= 0 && (next < 0 || due < next))
            {
                next = due;
            }
        }
        for (auto due : self->column_due)
        {
            if (due >= 0 && (next < 0 || due < next))
            {
                next = due;
            }
        }
        for (auto &group : self->masked_groups)
        {
            if (group.due >= 0 && (next < 0 || group.due < next))
            {
                next = group.due;
            }
        }
        if (next >= 0)
        {
            self->release.enqueue(std::max<int64_t>(1, next - now));
        }
    }
    static void tick(vp::Block *block, vp::ClockEvent *)
    {
        auto self = static_cast<Control *>(block);
        int64_t now = self->clock.get_cycles();
        if (now >= self->watchdog)
        {
            self->trace.fatal("arche3d software watchdog expired\n");
        }
        printf("ARCHE3D_PROGRESS "
               "{\"cycle\":%ld,\"issued\":%lu,\"completed\":%lu,\"clusters_done\":%u}\n",
               now, self->issued, self->completed, self->exited);
        fflush(stdout);
        self->progress.enqueue(self->progress_cycles);
    }
    static void activity(vp::Block *block, Arche3dDmaEvent evt, int cluster)
    {
        auto self = static_cast<Control *>(block);
        auto &s = self->stats[cluster];
        int64_t now = self->clock.get_cycles();
        if (!evt.completed)
        {
            if (!s.ids.insert(evt.id).second)
            {
                self->trace.fatal("Duplicate live I3D AXI ID\n");
            }
            if (s.expected)
            {
                unsigned terminal = (cluster % self->nx) * self->ny + cluster / self->nx;
                uint64_t destination = (terminal + s.issued) % self->count;
                if (evt.write || evt.size != s.burst_bytes ||
                    evt.address !=
                        self->memory_base + (self->count + destination) * self->interleave)
                {
                    self->trace.fatal("All-to-all DMA sequence mismatch in cluster %u\n", cluster);
                }
            }
            ++s.issued;
            ++self->issued;
            s.bytes += evt.size;
            self->bytes += evt.size;
            if (self->first < 0)
            {
                self->first = now;
            }
            self->inject = now;
            self->peak = std::max(self->peak, self->issued - self->completed);
        }
        else
        {
            if (!s.ids.erase(evt.id))
            {
                self->trace.fatal("Unknown I3D DMA completion ID\n");
            }
            ++s.completed;
            ++self->completed;
            self->last = now;
        }
    }
    void finish()
    {
        for (auto &s : stats)
        {
            if (s.issued != s.completed || !s.ids.empty() ||
                (s.expected && s.completed != s.expected))
            {
                trace.fatal("Software exited with incomplete or missing DMA transfers\n");
            }
        }
        double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - wall).count();
        printf("ARCHE3D_RESULT "
               "{\"status\":\"PASS\",\"clusters\":%u,\"transactions\":%lu,\"bytes\":%lu,"
               "\"peak_outstanding\":%lu,\"network_cycles\":%.1f,\"injection_cycles\":%.1f,"
               "\"software_cycles\":%ld,\"boot_cycles\":%ld,\"total_cycles\":%ld,"
               "\"icache_preloaded_lines\":%lu,\"icache_refills\":%lu,"
               "\"icache_runtime_refills\":%lu,\"wall_seconds\":%.6f}\n",
               count, completed, bytes, peak, first < 0 ? 0.0 : last - first + 0.5,
               first < 0 ? 0.0 : inject - first + 0.5, begin < 0 ? 0 : clock.get_cycles() - begin,
               boot_cycle, clock.get_cycles(), preheat_lines * count, total_refills(),
               total_refills() - refills_at_boot, seconds);
        fflush(stdout);
        time.get_engine()->quit(0);
    }
    static vp::IoReqStatus input(vp::Block *block, vp::IoReq *req, int cluster)
    {
        auto self = static_cast<Control *>(block);
        uint64_t addr = req->get_addr();
        if (req->get_size() != 4)
        {
            return vp::IO_REQ_INVALID;
        }
        uint32_t value = 0;
        std::memcpy(&value, req->get_data(), 4);
        if (!req->get_is_write())
        {
            switch (addr)
            {
            case 0x08:
                value = self->clock.get_cycles();
                break;
            case 0x0c:
                value = uint64_t(self->clock.get_cycles()) >> 32;
                break;
            case 0x110:
                value = self->refill_counts[cluster];
                break;
            case 0x11c:
                value = self->masked_config[cluster];
                break;
            default:
                return vp::IO_REQ_INVALID;
            }
            std::memcpy(req->get_data(), &value, 4);
            return vp::IO_REQ_OK;
        }
        switch (addr)
        {
        case 0:
            if (value)
            {
                fprintf(stderr, "ARCHE3D_FAIL cluster=%d code=%u\n", cluster, value);
                self->time.get_engine()->quit(1);
            }
            else
            {
                if (self->stats[cluster].exited)
                {
                    self->trace.fatal("Cluster exited twice\n");
                }
                self->stats[cluster].exited = true;
                if (++self->exited == self->count)
                {
                    self->finish();
                }
            }
            break;
        case 0x04:
            break; // Reserved system wakeup register.
        case 0x10:
            std::putchar(value);
            std::fflush(stdout);
            break;
        case 0x100:
            if (self->waiters[cluster])
            {
                self->trace.fatal("Duplicate global barrier arrival\n");
            }
            self->waiters[cluster] = req;
            if (++self->arrived == self->count)
            {
                self->global_due = self->clock.get_cycles() + 1;
                self->release.enqueue();
            }
            return vp::IO_REQ_PENDING;
        case 0x114:
        case 0x118:
        {
            bool column = addr == 0x118;
            unsigned group = column ? cluster % self->nx : cluster / self->nx;
            auto &waiters = column ? self->column_waiters : self->row_waiters;
            auto &arrivals = column ? self->column_arrived : self->row_arrived;
            auto &due = column ? self->column_due : self->row_due;
            if (waiters[cluster])
            {
                self->trace.fatal("Duplicate row/column barrier arrival\n");
            }
            waiters[cluster] = req;
            if (++arrivals[group] == (column ? self->ny : self->nx))
            {
                due[group] = self->clock.get_cycles() + 1;
                self->release.enqueue();
            }
            return vp::IO_REQ_PENDING;
        }
        case 0x11c:
            if (self->masked_waiters[cluster])
            {
                self->trace.fatal("Reprogramming an active masked barrier\n");
            }
            self->masked_config[cluster] = value;
            break;
        case 0x120:
        {
            if (self->masked_waiters[cluster])
            {
                self->trace.fatal("Duplicate masked barrier arrival\n");
            }
            uint32_t masks = self->masked_config[cluster];
            uint32_t anchor = ((cluster % self->nx) | ((cluster / self->nx) << 16)) & masks;
            MaskedBarrier *group = nullptr, *empty = nullptr;
            for (auto &candidate : self->masked_groups)
            {
                if (!candidate.target)
                {
                    empty = &candidate;
                }
                else if (candidate.masks == masks && candidate.anchor == anchor)
                {
                    group = &candidate;
                    break;
                }
            }
            if (!group)
            {
                if (!empty)
                {
                    self->trace.fatal("Masked barrier context capacity exceeded\n");
                }
                group = empty;
                group->masks = masks;
                group->anchor = anchor;
                unsigned xs = 0, ys = 0;
                for (unsigned x = 0; x < self->nx; ++x)
                {
                    xs += (x & (masks & 0xffff)) == (anchor & 0xffff);
                }
                for (unsigned y = 0; y < self->ny; ++y)
                {
                    ys += (y & (masks >> 16)) == (anchor >> 16);
                }
                group->target = xs * ys;
            }
            self->masked_waiters[cluster] = req;
            group->members.push_back(cluster);
            if (group->members.size() == group->target)
            {
                group->due = self->clock.get_cycles() + 1;
                self->release.enqueue();
            }
            return vp::IO_REQ_PENDING;
        }
        case 0x104:
            self->stats[cluster].expected = value;
            break;
        case 0x108:
            self->stats[cluster].burst_bytes = value;
            break;
        default:
            return vp::IO_REQ_INVALID;
        }
        return vp::IO_REQ_OK;
    }

  public:
    explicit Control(vp::ComponentConf &config)
        : vp::Component(config), boot(this, start), release(this, release_barrier),
          progress(this, tick)
    {
        traces.new_trace("trace", &trace, vp::DEBUG);
        auto cfg = get_js_config();
        nx = cfg->get_uint("nx");
        ny = cfg->get_uint("ny");
        count = nx * ny;
        memory_base = cfg->get_uint("memory_base");
        interleave = cfg->get_uint("interleave");
        axi_bytes = cfg->get_uint("axi_bytes");
        progress_cycles = cfg->get_int("progress_cycles");
        watchdog = cfg->get_int("watchdog_cycles");
        image_bytes = cfg->get_uint("image_bytes");
        preheat_lines = cfg->get_uint("preheat_lines");
        dram_preloaded_bytes = cfg->get_uint("dram_preloaded_bytes");
        preheat_base = cfg->get_uint("preheat_base");
        auto hex = cfg->get("preheat_data")->get_str();
        if (hex.size() % 2)
        {
            trace.fatal("Invalid cache preload hex data\n");
        }
        auto digit = [this](char c) -> unsigned
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            trace.fatal("Invalid cache preload hex digit\n");
            return 0;
        };
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            preheat_data.push_back((digit(hex[i]) << 4) | digit(hex[i + 1]));
        }
        if (progress_cycles <= 0)
        {
            progress_cycles = watchdog;
        }
        stats.resize(count);
        waiters.resize(count);
        inputs.resize(count);
        activities.resize(count);
        row_waiters.resize(count);
        column_waiters.resize(count);
        row_arrived.resize(ny);
        column_arrived.resize(nx);
        row_generation.resize(ny);
        column_generation.resize(nx);
        row_due.assign(ny, -1);
        column_due.assign(nx, -1);
        masked_groups.resize(count);
        masked_config.resize(count);
        masked_waiters.resize(count);
        cache_refills.resize(count);
        refill_counts.resize(count);
        for (unsigned i = 0; i < count; ++i)
        {
            inputs[i].set_req_meth_muxed(input, i);
            new_slave_port("input_" + std::to_string(i), &inputs[i]);
            activities[i].set_sync_meth_muxed(activity, i);
            new_slave_port("activity_" + std::to_string(i), &activities[i]);
            cache_refills[i].set_sync_meth_muxed(refilled, i);
            new_slave_port("cache_refills_" + std::to_string(i), &cache_refills[i]);
        }
        new_master_port("ready", &ready);
        new_master_port("cache_preload", &cache_preload);
    }
    void reset(bool active) override
    {
        if (active)
        {
            release.cancel();
            std::fill(waiters.begin(), waiters.end(), nullptr);
            std::fill(row_waiters.begin(), row_waiters.end(), nullptr);
            std::fill(column_waiters.begin(), column_waiters.end(), nullptr);
            std::fill(row_arrived.begin(), row_arrived.end(), 0);
            std::fill(column_arrived.begin(), column_arrived.end(), 0);
            std::fill(row_generation.begin(), row_generation.end(), 0);
            std::fill(column_generation.begin(), column_generation.end(), 0);
            std::fill(row_due.begin(), row_due.end(), -1);
            std::fill(column_due.begin(), column_due.end(), -1);
            std::fill(masked_config.begin(), masked_config.end(), 0);
            std::fill(masked_waiters.begin(), masked_waiters.end(), nullptr);
            for (auto &group : masked_groups)
            {
                group = {};
            }
            arrived = 0;
            global_due = -1;
        }
        else
        {
            wall = std::chrono::steady_clock::now();
            // All reset assertions have completed. Initialize every cache at
            // time zero. Direct DRAM population has completed in the endpoints'
            // constructors. Release all cores on the next clock edge.
            cache_preload.sync({preheat_base, preheat_data.data(), preheat_data.size()});
            boot.enqueue();
            progress.enqueue(progress_cycles);
        }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new Control(config);
}
