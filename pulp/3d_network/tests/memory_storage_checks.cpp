// SPDX-License-Identifier: Apache-2.0
#include "../memory_storage.hpp"
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <vector>

using network3d::MemoryEndpointStorage;
static void require(bool ok,const char *message) { if (!ok) throw std::runtime_error(message); }
template<typename F> static void rejected(F action) {
    bool failed=false;
    try { action(); } catch (const std::exception &) { failed=true; }
    require(failed,"invalid RAM access/preload accepted");
}
static uint8_t pattern(uint64_t address,unsigned endpoint) {
    return (17*endpoint+13*address+(address>>8))&255;
}

static void random_access(bool patterned) {
    const unsigned size=3*4096+17,endpoint=37;
    MemoryEndpointStorage ram(size,patterned,endpoint);
    std::vector<uint8_t> expected(size),actual(size);
    if (patterned) for (unsigned i=0;i<size;++i) expected[i]=pattern(i,endpoint);
    ram.read(0,actual.data(),size);
    require(actual==expected,"untouched RAM initial contents");
    std::mt19937 rng(42);
    for (unsigned trial=0;trial<200;++trial) {
        unsigned address=rng()%size,length=1+rng()%std::min(6000u,size-address);
        std::vector<uint8_t> data(length),strobes(length);
        for (unsigned i=0;i<length;++i) { data[i]=rng(); strobes[i]=rng()%2; }
        const bool masked=trial%3!=0;
        ram.write(address,data.data(),length,masked?strobes.data():nullptr);
        for (unsigned i=0;i<length;++i)
            if (!masked || strobes[i]) expected[address+i]=data[i];
        ram.read(0,actual.data(),size);
        require(actual==expected,"unaligned/cross-page/strobed write corrupted RAM");
    }
    uint8_t byte=99,disabled=0;
    ram.write(size-1,&byte,1,&disabled);
    ram.read(0,actual.data(),size);
    require(actual==expected,"disabled strobe changed RAM");
    rejected([&] { ram.read(size-1,&byte,2); });
    rejected([&] { ram.write(size,&byte,1); });
    rejected([&] { ram.read(std::numeric_limits<uint64_t>::max(),&byte,2); });
    rejected([&] { ram.write(1,&byte,std::numeric_limits<uint64_t>::max()); });
}

static void preloads() {
    MemoryEndpointStorage ram(20000,true,5);
    std::string bytes(7000,'\0');
    for (unsigned i=0;i<bytes.size();++i) bytes[i]=(i*31+9)&255;
    std::istringstream file(bytes);
    ram.preload(file,4093,17,5001,9000);
    std::vector<uint8_t> actual(20000);
    ram.read(0,actual.data(),actual.size());
    for (unsigned i=0;i<actual.size();++i) {
        uint8_t expected=i<4093 || i>=4093+9000?pattern(i,5):
            i<4093+5001?uint8_t(bytes[17+i-4093]):0;
        require(actual[i]==expected,"file preload/zero fill/neighbor preservation");
    }
    ram.preload(file,19997,0,0,3);
    ram.read(0,actual.data(),actual.size());
    require(actual[19996]==pattern(19996,5) && actual[19997]==0 && actual[19999]==0,
            "pure zero-fill fragment");
    rejected([&] { ram.preload(file,0,0,2,1); });
    rejected([&] { ram.preload(file,19999,0,2,2); });
    rejected([&] { ram.preload(file,0,bytes.size(),1,1); });
    rejected([&] { ram.preload(file,0,std::numeric_limits<uint64_t>::max(),0,1); });
    rejected([&] { ram.preload(file,0,0,0,0); });
}

static void large_capacity() {
    // Exercise the full default chip map without allocating its 128 GiB.
    std::vector<std::unique_ptr<MemoryEndpointStorage>> channels;
    for (unsigned channel=0;channel<1024;++channel) {
        channels.emplace_back(std::make_unique<MemoryEndpointStorage>(128ull<<20,true,channel));
        uint8_t data[5];
        channels.back()->read((128ull<<20)-sizeof(data),data,sizeof(data));
        for (unsigned i=0;i<sizeof(data);++i)
            require(data[i]==pattern((128ull<<20)-sizeof(data)+i,channel),"large sparse map");
    }
    MemoryEndpointStorage wide(16ull<<30);
    uint8_t data[]={1,2,3,4,5},actual[5];
    wide.write((8ull<<30)-2,data,sizeof(data));
    wide.read((8ull<<30)-2,actual,sizeof(actual));
    require(std::equal(std::begin(data),std::end(data),std::begin(actual)),"64-bit RAM address");
}

int main() {
    rejected([] { MemoryEndpointStorage invalid(0); });
    random_access(false); random_access(true); preloads(); large_capacity();
    std::cout<<"MEMORY_STORAGE_CHECKS_PASS: sparse capacity, 64-bit addresses, strobes, preloads, bounds\n";
}
