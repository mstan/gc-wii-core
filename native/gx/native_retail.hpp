// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <stdexcept>

extern "C" uint32_t gcnllr_aurora_retail_process(const uint8_t*,uint32_t);
extern "C" void gcnllr_aurora_retail_finish();
namespace gcnllr {
class NativeRetailFrontend {
public:
    bool write_fifo(std::span<const uint8_t> bytes) {
        if(bytes.empty())return false;
        if(bytes.size()>16u*1024u*1024u-pending_.size())throw std::runtime_error("Retail FIFO carry exceeds bounded storage");
        pending_.insert(pending_.end(),bytes.begin(),bytes.end());return true;
    }
    bool flush() {
        if(pending_.empty())return true;
        const uint32_t used=gcnllr_aurora_retail_process(pending_.data(),uint32_t(pending_.size()));
        gcnllr_aurora_retail_finish();
        if(used>pending_.size())throw std::runtime_error("Retail decoder consumed beyond its input");
        pending_.erase(pending_.begin(),pending_.begin()+used);return true;
    }
    size_t pending_fifo_size()const{return pending_.size();}
    const char* last_error()const{return nullptr;}
private:
    std::vector<uint8_t> pending_;
};
}
