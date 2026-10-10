// SPDX-License-Identifier: Apache-2.0
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>
#include "float_math.hpp"
#include <pulp/mxcore_fp4/compute.hpp>

class LayoutEngine : public vp::Component {
    enum {
        TRANSPOSE, MX_TO_E5M2, MX_TO_BF16, BF16_TO_MX, FP16_TO_MX, MX_TO_E4M3,
        MX_ROW_TO_COL, MX_COL_TO_ROW
    };
    struct Tile {
        uint32_t first, count, row = 0, col = 0, rows = 0, cols = 0;
        std::vector<uint8_t> input, output, scales, output_scales;
        unsigned pending = 0;
        int phase = 0; // read, convert, write
        int64_t ready = 0;
    };
    struct Transfer { vp::IoReq req; Tile *tile; int64_t ready = -1; };
    vp::IoSlave input;
    vp::IoMaster tcdm;
    vp::ClockEvent event;
    vp::Trace trace;
    std::array<uint32_t, 16> regs{};
    std::list<std::unique_ptr<Tile>> tiles;
    std::unordered_map<vp::IoReq *, std::unique_ptr<Transfer>> requests;
    std::vector<vp::IoReq *> waiters;
    uint64_t l1_base, l1_size, started = 0, moved = 0;
    unsigned bandwidth, latency, next = 0, total = 0, tile_row = 0, tile_col = 0;
    bool busy = false, claimed = false, failed = false;
    bool pack() const { return regs[7] == BF16_TO_MX || regs[7] == FP16_TO_MX; }
    bool reblock() const { return regs[7] == MX_ROW_TO_COL || regs[7] == MX_COL_TO_ROW; }
    unsigned narrow_bytes() const { return regs[7] == MX_TO_E5M2 || regs[7] == MX_TO_E4M3 ? 1 : 2; }

    // Both layouts contain contiguous 32x32 tiles. Column blocking swaps the
    // tile traversal and the block axis; the logical matrix shape is unchanged.
    uint64_t mx_tile(const Tile &t, bool output) const {
        bool column = (regs[7] == MX_ROW_TO_COL) == output;
        return column ? uint64_t(t.col / 32) * (regs[0] / 32) + t.row / 32 :
                        uint64_t(t.row / 32) * (regs[1] / 32) + t.col / 32;
    }

    bool range(uint64_t addr, uint64_t bytes) const {
        return bytes && addr >= l1_base && addr - l1_base < l1_size &&
            bytes <= l1_size - (addr - l1_base);
    }
    bool valid() const {
        uint64_t count = uint64_t(regs[0]) * regs[1];
        if (!count || count > UINT32_MAX || regs[7] > MX_COL_TO_ROW) return false;
        std::vector<std::pair<uint64_t,uint64_t>> areas;
        if (regs[7] == TRANSPOSE) {
            if (regs[4] != 1 && regs[4] != 2) return false;
            areas = {{regs[2], count * regs[4]}, {regs[3], count * regs[4]}};
        } else if (reblock()) {
            if (regs[0] % 32 || regs[1] % 32) return false;
            areas = {{regs[2], count / 2}, {regs[3], count / 2},
                     {regs[8], count / 32}, {regs[9], count / 32}};
        } else {
            if (count % 32) return false;
            areas = {{regs[2], pack() ? count * 2 : count / 2},
                     {regs[3], pack() ? count / 2 : count * narrow_bytes()},
                     {regs[pack() ? 9 : 8], count / 32}};
        }
        for (unsigned i = 0; i < areas.size(); ++i) {
            if (!range(areas[i].first, areas[i].second)) return false;
            for (unsigned j = 0; j < i; ++j)
                if (areas[i].first < areas[j].first + areas[j].second &&
                    areas[j].first < areas[i].first + areas[i].second) return false;
        }
        return true;
    }

    static vp::IoReqStatus mmio(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<LayoutEngine *>(block);
        unsigned addr = req->get_addr();
        if (req->get_size() != 4 || !req->get_data() || addr >= 64 || addr % 4)
            return vp::IO_REQ_INVALID;
        unsigned reg = addr / 4;
        uint32_t value = 0;
        if (req->get_is_write()) {
            if (self->busy || reg == 5 || reg == 6 || reg >= 10) return vp::IO_REQ_INVALID;
            std::memcpy(&self->regs[reg], req->get_data(), 4);
            return vp::IO_REQ_OK;
        }
        if (reg == 5) {
            if (self->busy) value = 1;
            else {
                self->failed = !self->valid();
                self->regs[10] = self->failed ? 2 : 1;
                self->regs[11] = self->regs[12] = self->regs[14] = 0;
                self->next = self->tile_row = self->tile_col = 0;
                self->moved = 0;
                self->started = self->clock.get_cycles();
                self->total = uint64_t(self->regs[0]) * self->regs[1];
                if (self->failed) { self->claimed = false; value = 2; }
                else { self->busy = true; self->event.enqueue(); }
            }
        } else if (reg == 6 && self->busy) {
            self->waiters.push_back(req);
            return vp::IO_REQ_PENDING;
        } else if (reg == 6) value = self->failed ? 2 : 0;
        else if (reg == 13) {
            value = self->claimed || self->busy ? UINT32_MAX : 0;
            if (!value) self->claimed = true;
        } else if (reg == 15) value = self->latency;
        else value = self->regs[reg];
        std::memcpy(req->get_data(), &value, 4);
        return vp::IO_REQ_OK;
    }

    void transfer(Tile &tile, uint64_t addr, uint8_t *data, unsigned bytes, bool write) {
        while (bytes) {
            unsigned count = std::min<uint64_t>(bytes, bandwidth - addr % bandwidth);
            auto t = std::make_unique<Transfer>();
            t->tile = &tile;
            auto req = &t->req;
            req->init(); req->set_addr(addr - l1_base); req->set_size(count);
            req->set_data(data); req->set_is_write(write);
            requests.emplace(req, std::move(t));
            ++tile.pending;
            moved += count;
            auto status = tcdm.req(req);
            if (status == vp::IO_REQ_OK || status == vp::IO_REQ_INVALID) {
                req->status = status;
                response(this, req);
            }
            // PENDING and DENIED both retain ownership on IO v1.
            addr += count; data += count; bytes -= count;
        }
    }
    static void response(vp::Block *block, vp::IoReq *req) {
        auto self = static_cast<LayoutEngine *>(block);
        auto &t = *self->requests.at(req);
        self->failed |= req->status != vp::IO_REQ_OK;
        t.ready = self->clock.get_cycles() + req->get_latency();
        self->event.enqueue();
    }
    static void grant(vp::Block *, vp::IoReq *) {}

    void read(Tile &t) {
        if (reblock()) {
            uint64_t tile = mx_tile(t, false);
            transfer(t, regs[2] + tile * 512, t.input.data(), 512, false);
            transfer(t, regs[8] + tile * 32, t.scales.data(), 32, false);
        } else if (regs[7] == TRANSPOSE) {
            for (unsigned r = 0; r < t.rows; ++r)
                transfer(t, regs[2] + ((t.row + r) * regs[1] + t.col) * regs[4],
                         t.input.data() + r * t.cols * regs[4], t.cols * regs[4], false);
        } else {
            transfer(t, regs[2] + (pack() ? t.first * 2 : t.first / 2), t.input.data(), t.input.size(), false);
            if (!pack()) transfer(t, regs[8] + t.first / 32, t.scales.data(), t.scales.size(), false);
        }
    }
    void write(Tile &t) {
        if (reblock()) {
            uint64_t tile = mx_tile(t, true);
            transfer(t, regs[3] + tile * 512, t.output.data(), 512, true);
            transfer(t, regs[9] + tile * 32, t.output_scales.data(), 32, true);
        } else if (regs[7] == TRANSPOSE) {
            for (unsigned c = 0; c < t.cols; ++c)
                transfer(t, regs[3] + ((t.col + c) * regs[0] + t.row) * regs[4],
                         t.output.data() + c * t.rows * regs[4], t.rows * regs[4], true);
        } else {
            transfer(t, regs[3] + (pack() ? t.first / 2 : t.first * narrow_bytes()),
                     t.output.data(), t.output.size(), true);
            if (pack()) transfer(t, regs[9] + t.first / 32, t.scales.data(), t.scales.size(), true);
        }
    }
    void convert(Tile &t) {
        using namespace arche3d_float;
        RneScope rounding;
        if (reblock()) {
            // Destination block b gathers one element from each source block.
            // Double represents every finite E2M1/E8M0 value exactly, including
            // magnitudes which would overflow an FP32 intermediate.
            for (unsigned b = 0; b < 32; ++b) {
                std::array<double, 32> values;
                double maximum = 0;
                bool poison = false;
                for (unsigned i = 0; i < 32; ++i) {
                    uint8_t code = (t.input[i * 16 + b / 2] >> (4 * (b % 2))) & 15;
                    uint8_t scale = t.scales[i];
                    poison |= scale == 255;
                    values[i] = std::ldexp(double(mxcore_fp4::twice(code)), int(scale) - 128);
                    if (!(code & 7)) values[i] = std::copysign(0.0, code & 8 ? -1.0 : 1.0);
                    maximum = std::max(maximum, std::abs(values[i]));
                }
                int exponent = maximum ? std::max(-127, std::min(127, std::ilogb(maximum) - 2)) : 0;
                t.output_scales[b] = poison ? 255 : exponent + 127;
                for (unsigned i = 0; i < 16; ++i) {
                    // Normalize before entering the shared FP4 quantizer. Any
                    // FP32 underflow here is far below the FP4 rounding threshold;
                    // all nonzero FP4 decisions and ties remain exact.
                    float lo = std::ldexp(values[2 * i], -exponent);
                    float hi = std::ldexp(values[2 * i + 1], -exponent);
                    t.output[b * 16 + i] = poison ? 0 :
                        mxcore_fp4::quantize(lo, 0) | (mxcore_fp4::quantize(hi, 0) << 4);
                }
            }
        } else if (regs[7] == TRANSPOSE) {
            for (unsigned r = 0; r < t.rows; ++r)
                for (unsigned c = 0; c < t.cols; ++c)
                    std::memcpy(t.output.data() + (c * t.rows + r) * regs[4],
                                t.input.data() + (r * t.cols + c) * regs[4], regs[4]);
        } else if (pack()) {
            auto format = regs[7] == BF16_TO_MX ? BF16 : FP16;
            for (unsigned b = 0; b < t.count / 32; ++b) {
                std::array<float,32> values;
                float maximum = 0;
                bool poison = false;
                for (unsigned i = 0; i < 32; ++i) {
                    uint32_t bits = arche3d_float::convert(load(t.input.data() + (b * 32 + i) * 2, 2), format, FP32);
                    std::memcpy(&values[i], &bits, 4);
                    maximum = std::max(maximum, std::abs(values[i]));
                    poison |= !std::isfinite(values[i]);
                }
                int exponent = maximum ? std::max(-127, std::min(127, std::ilogb(maximum) - 2)) : 0;
                t.scales[b] = poison ? 255 : exponent + 127;
                for (unsigned i = 0; i < 16; ++i)
                    t.output[b * 16 + i] = poison ? 0 :
                        mxcore_fp4::quantize(values[2*i], exponent) |
                        (mxcore_fp4::quantize(values[2*i+1], exponent) << 4);
            }
        } else {
            auto format = regs[7] == MX_TO_BF16 ? BF16 : regs[7] == MX_TO_E4M3 ? E4M3 : E5M2;
            for (unsigned i = 0; i < t.count; ++i) {
                uint8_t code = (t.input[i / 2] >> (4 * (i % 2))) & 15;
                uint8_t scale = t.scales[i / 32];
                uint16_t result;
                if (scale == 255) result = canonical_nan(format);
                else {
                    float value = std::ldexp(float(mxcore_fp4::twice(code)), int(scale) - 128);
                    if (!(code & 7)) value = std::copysign(0.0f, code & 8 ? -1.0f : 1.0f);
                    uint32_t bits; std::memcpy(&bits, &value, 4);
                    result = arche3d_float::convert(bits, FP32, format);
                }
                store(t.output.data() + i * narrow_bytes(), result, narrow_bytes());
            }
        }
    }

    static void step(vp::Block *block, vp::ClockEvent *) {
        auto self = static_cast<LayoutEngine *>(block);
        auto now = self->clock.get_cycles();
        for (auto it = self->requests.begin(); it != self->requests.end();) {
            auto &t = *it->second;
            if (t.ready >= 0 && t.ready <= now) {
                t.tile->ready = std::max(t.tile->ready, t.ready);
                --t.tile->pending; it = self->requests.erase(it);
            }
            else ++it;
        }
        for (auto it = self->tiles.begin(); it != self->tiles.end();) {
            auto &t = **it;
            if (t.phase == 0 && !t.pending) {
                self->convert(t);
                t.phase = 1;
                t.ready += self->regs[7] == TRANSPOSE ? 0 : self->latency;
            }
            if (t.phase == 1 && t.ready <= now) {
                t.phase = 2;
                if (!self->failed) self->write(t);
            }
            if (t.phase == 2 && !t.pending) {
                self->regs[14] += t.count;
                it = self->tiles.erase(it);
            } else ++it;
        }
        // Bounded window of 16 x 1024-element batches. Read capture, five-cycle
        // conversion and writeback overlap. Outstanding buffers live until ACK.
        while (!self->failed && self->next < self->total && self->tiles.size() < 16) {
            auto tile = std::make_unique<Tile>();
            auto &t = *tile;
            t.first = self->next;
            if (self->regs[7] == TRANSPOSE || self->reblock()) {
                if (self->reblock()) {
                    // Diagonal tile traversal distributes both source and
                    // transposed destination scale accesses across L1 banks.
                    unsigned ordinal = self->next / 1024;
                    unsigned row = ordinal % (self->regs[0] / 32);
                    t.row = row * 32;
                    t.col = ((ordinal / (self->regs[0] / 32) + row) % (self->regs[1] / 32)) * 32;
                } else {
                    t.row = self->tile_row; t.col = self->tile_col;
                }
                t.rows = std::min(32u, self->regs[0] - t.row);
                t.cols = std::min(32u, self->regs[1] - t.col);
                t.count = t.rows * t.cols;
                if (self->reblock()) {
                    t.input.resize(512); t.output.resize(512);
                    t.scales.resize(32); t.output_scales.resize(32);
                } else {
                    t.input.resize(t.count * self->regs[4]); t.output.resize(t.input.size());
                }
                if (!self->reblock()) {
                    self->tile_col += t.cols;
                    if (self->tile_col == self->regs[1]) { self->tile_col = 0; self->tile_row += t.rows; }
                }
            } else {
                t.count = std::min(1024u, self->total - self->next);
                t.input.resize(self->pack() ? t.count * 2 : t.count / 2);
                t.output.resize(self->pack() ? t.count / 2 : t.count * self->narrow_bytes());
                t.scales.resize(t.count / 32);
            }
            self->next += t.count;
            self->tiles.push_back(std::move(tile));
            self->read(t);
        }
        if (self->tiles.empty() && (self->failed || self->next == self->total)) {
            self->busy = self->claimed = false;
            self->regs[10] = self->failed ? 2 : 4;
            self->regs[11] = now - self->started;
            self->regs[12] = self->moved;
            auto waiters = std::move(self->waiters);
            self->waiters.clear();
            for (auto req : waiters) {
                uint32_t status = self->failed ? 2 : 0;
                std::memcpy(req->get_data(), &status, 4);
                req->status = vp::IO_REQ_OK;
                req->get_resp_port()->resp(req);
            }
        } else self->event.enqueue();
    }
public:
    explicit LayoutEngine(vp::ComponentConf &config) : vp::Component(config), event(this, step) {
        traces.new_trace("trace", &trace, vp::DEBUG);
        bandwidth = get_js_config()->get_int("bandwidth");
        latency = get_js_config()->get_int("conversion_latency");
        l1_base = get_js_config()->get_uint("l1_base");
        l1_size = get_js_config()->get_uint("l1_size");
        input.set_req_meth(mmio); tcdm.set_resp_meth(response); tcdm.set_grant_meth(grant);
        new_slave_port("input", &input); new_master_port("tcdm", &tcdm);
        regs[4] = 2;
    }
    void reset(bool active) override {
        if (active) {
            event.cancel(); requests.clear(); tiles.clear(); waiters.clear();
            regs.fill(0); regs[4] = 2; busy = claimed = failed = false;
        }
    }
};
extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new LayoutEngine(config); }
