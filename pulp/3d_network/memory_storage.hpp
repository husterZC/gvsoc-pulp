// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <istream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace network3d {

// Untouched RAM reads as zero or the benchmark pattern. Allocate host pages
// only on writes/preloads, so a large modeled capacity costs no upfront RAM.
// Storage has no timing; MemoryEndpointTiming still schedules every AXI beat.
class MemoryEndpointStorage {
    static constexpr uint64_t page_size=4096;
    using Page=std::array<uint8_t,page_size>;
    uint64_t size,endpoint;
    bool pattern;
    std::unordered_map<uint64_t,std::unique_ptr<Page>> pages;

    void check(uint64_t address,uint64_t length) const {
        if (address>size || length>size-address)
            throw std::out_of_range("Memory endpoint access outside RAM");
    }
    void initial(uint64_t address,uint8_t *data,size_t length) const {
        if (!pattern) std::memset(data,0,length);
        else for (size_t i=0;i<length;++i) {
            uint64_t a=address+i;
            data[i]=(17*endpoint+13*a+(a>>8))&255;
        }
    }
    Page &page(uint64_t index) {
        auto &entry=pages[index];
        if (!entry) {
            entry=std::make_unique<Page>();
            initial(index*page_size,entry->data(),page_size);
        }
        return *entry;
    }
public:
    explicit MemoryEndpointStorage(uint64_t size,bool pattern=false,uint64_t endpoint=0)
        : size(size),endpoint(endpoint),pattern(pattern) {
        if (!size) throw std::invalid_argument("Memory endpoint size must be positive");
    }
    void read(uint64_t address,uint8_t *data,uint64_t length) const {
        check(address,length);
        while (length) {
            size_t offset=address%page_size,count=std::min<uint64_t>(length,page_size-offset);
            auto entry=pages.find(address/page_size);
            if (entry==pages.end()) initial(address,data,count);
            else std::memcpy(data,entry->second->data()+offset,count);
            address+=count; data+=count; length-=count;
        }
    }
    void write(uint64_t address,const uint8_t *data,uint64_t length,const uint8_t *strobes=nullptr) {
        check(address,length);
        while (length) {
            size_t offset=address%page_size,count=std::min<uint64_t>(length,page_size-offset);
            // A masked-out beat must not allocate or modify storage.
            if (!strobes || std::any_of(strobes,strobes+count,[](uint8_t value) { return value!=0; })) {
                auto dest=page(address/page_size).data()+offset;
                if (!strobes) std::memcpy(dest,data,count);
                else for (size_t i=0;i<count;++i) if (strobes[i]) dest[i]=data[i];
            }
            address+=count; data+=count; length-=count;
            if (strobes) strobes+=count;
        }
    }
    // Same channel-local [address, file offset, file bytes, memory bytes]
    // fragments used by DRAMSys. Zero-fill overwrites the initial pattern.
    void preload(std::istream &file,uint64_t address,uint64_t offset,
                 uint64_t copied,uint64_t length) {
        file.clear();
        file.seekg(0,std::ios::end);
        auto end=file.tellg();
        if (end<0) throw std::runtime_error("Cannot size memory preload file");
        if (!length || copied>length || address>size || length>size-address ||
            offset>uint64_t(end) || copied>uint64_t(end)-offset)
            throw std::invalid_argument("Memory preload fragment outside RAM or file");
        file.seekg(offset);
        std::array<uint8_t,page_size> buffer;
        for (uint64_t done=0;done<length;) {
            size_t count=std::min<uint64_t>(buffer.size(),length-done);
            size_t bytes=done<copied?std::min<uint64_t>(count,copied-done):0;
            if (bytes && !file.read(reinterpret_cast<char*>(buffer.data()),bytes))
                throw std::runtime_error("Truncated memory preload file");
            std::fill(buffer.begin()+bytes,buffer.begin()+count,0);
            write(address+done,buffer.data(),count);
            done+=count;
        }
    }
};
}
