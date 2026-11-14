/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif
#include "di/di.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);goto cleanup;}}while(0)
static FILE* create_new(const char* path) {
#ifdef _WIN32
    int fd=_open(path,_O_CREAT|_O_EXCL|_O_RDWR|_O_BINARY,_S_IREAD|_S_IWRITE);
    if(fd<0)return NULL;
    FILE* file=_fdopen(fd,"w+b");
    if(!file){_close(fd);remove(path);}return file;
#else
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);
    if(fd<0)return NULL;
    FILE* file=fdopen(fd,"w+b");
    if(!file){close(fd);remove(path);}return file;
#endif
}
static int fixture_seek(FILE* file, long long offset) {
#ifdef _WIN32
    return _fseeki64(file,offset,SEEK_SET);
#else
    return fseeko(file,(off_t)offset,SEEK_SET);
#endif
}
static void read_command(GcnDi* di,CPUState* cpu,u32 command,u64 offset) {
    gcn_di_write(di,cpu,GCN_DI_BASE+GCN_DI_CMDBUF0,command,4);
    gcn_di_write(di,cpu,GCN_DI_BASE+GCN_DI_CMDBUF1,(u32)(offset>>2),4);
    gcn_di_write(di,cpu,GCN_DI_BASE+GCN_DI_CMDBUF2,32,4);
    gcn_di_write(di,cpu,GCN_DI_BASE+GCN_DI_MAR,0x2000,4);
    gcn_di_write(di,cpu,GCN_DI_BASE+GCN_DI_LENGTH,32,4);
    gcn_di_write(di,cpu,GCN_DI_BASE+GCN_DI_CR,3,4);
}
int main(int argc,char** argv) {
    if(argc!=3)return 2;
    const u64 offset=0x100002000ull;
    int result=1,created=0,empty_created=0;
    FILE* file=NULL;CPUState cpu={0};GcnDi di={0};
    u8 low[32],high[32];char empty[4096];
    for(unsigned n=0;n<32;n++){low[n]=(u8)(0x10+n);high[n]=(u8)(0xd0-n);}
    CHECK(snprintf(empty,sizeof empty,"%s.empty",argv[1])<(int)sizeof empty);
    file=create_new(argv[1]);CHECK(file);created=1;
#ifdef _WIN32
    /* Refuse a large physical allocation on filesystems lacking sparse files. */
    DWORD returned;
    CHECK(DeviceIoControl((HANDLE)_get_osfhandle(_fileno(file)),FSCTL_SET_SPARSE,
                          NULL,0,NULL,0,&returned,NULL));
#endif
    CHECK(fwrite(low,1,32,file)==32 && !fixture_seek(file,(long long)offset));
    CHECK(fwrite(high,1,32,file)==32);
    int closed=fclose(file);file=NULL;CHECK(!closed);
    CHECK(cpu_init(&cpu));gcn_di_init_model(&di,true);
    CHECK(gcn_di_set_disc(&di,argv[1]) && di.disc_size==offset+32);
    FILE* mounted=di.disc_file;
    CHECK(!gcn_di_set_disc(&di,argv[2]) && di.disc_file==mounted && di.disc_size==offset+32);
    file=create_new(empty);CHECK(file);empty_created=1;
    closed=fclose(file);file=NULL;CHECK(!closed);
    CHECK(!gcn_di_set_disc(&di,empty) && di.disc_file==mounted && di.disc_size==offset+32);
    gcn_di_reset_drive(&di,1);
    read_command(&di,&cpu,0xa8000040u,0);
    CHECK(di.pending_read_active && di.pending_intr==GCN_DI_INT_TCINT && di.pending_cycles<=0xffffffffu);
    gcn_di_tick(&cpu,(u32)di.pending_cycles);CHECK(!memcmp(cpu.ram+0x2000,low,32));
    memset(cpu.ram+0x2000,0xa5,32);read_command(&di,&cpu,0xa8000000u,offset);
    CHECK(di.pending_read_active && di.pending_read_offset==offset && di.pending_cycles<=0xffffffffu);
    CHECK(cpu.ram[0x2000]==0xa5); /* deferred, not an eager fixture write */
    gcn_di_tick(&cpu,(u32)di.pending_cycles);
    CHECK(!memcmp(cpu.ram+0x2000,high,32) && !di.pending_read_active && !di.cmd_pending);
    CHECK(di.disc_size==offset+32 && !cpu.halted);result=0;
cleanup:
    if(file)fclose(file);gcn_di_free(&di);cpu_free(&cpu);
    if(empty_created && remove(empty)){perror("remove empty fixture");result=1;}
    if(created && remove(argv[1])){perror("remove sparse fixture");result=1;}
    return result;
}
