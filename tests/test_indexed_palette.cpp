// SPDX-License-Identifier: MIT
#include "../native/gx/indexed_palette.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void check(bool condition,const char* why){if(!condition){std::fprintf(stderr,"%s\n",why);std::abort();}}
int main() {
    std::array<std::uint8_t,33> storage{};auto* p=storage.data()+1;
    // Unaligned big-endian input. Fixed reference colors exercise the copied
    // RGB565 green expansion that an ordinary RGB5A3 palette cannot express.
    constexpr unsigned raw[]{0x0400,0x0400,0x0401,0x03c0,0xf800,0x003e,0xffff,0};
    constexpr std::uint8_t rgb[][3]{{0,134,0},{0,134,0},{0,134,0},{0,121,0},{255,0,0},{0,0,255},{255,255,255},{0,0,0}};
    for(unsigned i=0;i<8;++i){p[i*2]=raw[i]>>8;p[i*2+1]=raw[i];}
    const auto out=gcnllr::decode_rgb5551_zero_palette(16,p,32);check(out.size()==64,"palette extent");
    for(unsigned i=0;i<8;++i){check(!std::memcmp(out.data()+i*4,rgb[i],3),"exact reference color");check(out[i*4+3]==(i?255:0),"index-zero alpha");}
    check(gcnllr::decode_rgb5551_zero_palette(15,p,32).empty(),"short palette count accepted");
    check(gcnllr::decode_rgb5551_zero_palette(17,p,32).empty(),"oversized palette count accepted");
    check(gcnllr::decode_rgb5551_zero_palette(16,p,31).empty(),"short bytes accepted");
    check(gcnllr::decode_rgb5551_zero_palette(16,nullptr,32).empty(),"null bytes accepted");
    std::puts("Native indexed palette: exact RGB565 colors, zero-index alpha and bounded source passed");
}
