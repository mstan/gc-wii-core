// SPDX-License-Identifier: MIT
#pragma once
#include "../../include/gcnllr/native_indexed.h"
#include <cstddef>
#include <cstdint>
#include <vector>
namespace gcnllr {
inline std::vector<std::uint8_t> decode_rgb5551_zero_palette(
        std::uint32_t entries,const std::uint8_t* source,std::size_t size) {
    if(entries!=16 || !source || size<32)return {};
    std::vector<std::uint8_t> out(64);
    for(unsigned n=0;n<16;++n) {
        const unsigned c=((unsigned(source[n*2])<<8)|source[n*2+1])>>1;
        const unsigned value=((c&0x7c00)<<1)|((c&0x03e0)<<1)|((c&0x0200)>>4)|(c&31);
        const unsigned r=value>>11,g=(value>>5)&63,b=value&31;
        out[n*4]=std::uint8_t((r<<3)|(r>>2));
        out[n*4+1]=std::uint8_t((g<<2)|(g>>4));
        out[n*4+2]=std::uint8_t((b<<3)|(b>>2));
        out[n*4+3]=n?255:0;
    }
    return out;
}
}
