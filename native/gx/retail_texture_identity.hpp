// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>
namespace gcnllr {
class RetailTextureIdentity {
public:
    using Key=std::array<uint32_t,6>; // physical address, bytes, width, height, format, mips
    template<class Generation>
    std::pair<uint32_t,uint32_t> identify(const Key& key,const uint8_t* bytes,Generation generation) {
        if(!bytes || !key[1])throw std::runtime_error("Empty native texture identity");
        ++queries;
        auto& entry=entries_[key];
        uint64_t epoch{};const bool valid=generation(&epoch);
        const bool same=entry.id && ((valid && entry.valid && epoch==entry.epoch) ||
            (!valid && !entry.valid && entry.snapshot.size()==key[1] &&
             !std::memcmp(entry.snapshot.data(),bytes,key[1])));
        if(same)++reused;
        if(!same) {
            if(!entry.id || entry.version==UINT32_MAX) {
                if(next_id_==UINT32_MAX)throw std::runtime_error("Native texture identities exhausted");
                entry.id=++next_id_;entry.version=0;
            }
            ++entry.version;
            entry.valid=valid;entry.epoch=epoch;
            if(valid)entry.snapshot.clear();else entry.snapshot.assign(bytes,bytes+key[1]);
        }
        return {entry.id,entry.version};
    }
    void clear(){entries_.clear();} // ids are never reused within this host lifetime
    uint64_t queries{},reused{};
private:
    struct Entry {uint32_t id{},version{};uint64_t epoch{};bool valid{};std::vector<uint8_t> snapshot;};
    std::map<Key,Entry> entries_;
    uint32_t next_id_=0x40000000u;
};
}
