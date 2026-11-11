/* SPDX-License-Identifier: MIT
 * Independent cursor/deadline design informed by pinned BlueWake main.c
 * host_cycle_cursor_delta/host_sync_*_cycles. This implementation replaces
 * the combined device advance; boot/devices and actual IRQ sources stay real.
 */
#include "gcnllr/device_domains.h"
#include <string.h>

static bool fail(GcnDeviceDomains* d) {
    if(d && !d->failed){d->failed=true;++d->failures;}return false;
}
static bool target(GcnDeviceDomains* d,uint64_t now) {
    if(!d || d->failed)return false;
    if(now<d->now)return fail(d);
    d->now=now;return true;
}
static bool refresh(GcnDeviceDomains* d) {
    if(d->failed)return false;
    for(unsigned i=0;i<GCN_DOMAIN_COUNT;i++)if((d->dirty&~d->active)&(1u<<i)) {
        const uint64_t distance=d->deadline(d->user,1u<<i,d->cursor[i]);
        if(d->failed)return false;
        d->due[i]=distance==UINT64_MAX || distance>UINT64_MAX-d->cursor[i]?
            UINT64_MAX:d->cursor[i]+distance;
        d->dirty&=~(1u<<i);
    }
    return true;
}
static bool service(GcnDeviceDomains* d,unsigned i,uint64_t now) {
    const uint32_t bit=1u<<i;
    if(d->active&bit)return fail(d);
    if(now<d->cursor[i])return fail(d);
    /* Even a zero-delta due event must be serviced. The provider must consume
     * it or move its next deadline; credit never silently spins past it. */
    d->active|=bit;
    const bool ok=d->advance(d->user,bit,now-d->cursor[i],now);
    d->active&=~bit;
    if(!ok || d->failed)return fail(d);
    d->cursor[i]=now;++d->services[i];d->dirty|=bit;
    return true;
}
bool gcn_domains_init(GcnDeviceDomains* d,uint64_t now,GcnDomainAdvance advance,
                     GcnDomainDeadline deadline,void* user) {
    return gcn_domains_init_mask(d,now,GCN_DOMAIN_ALL,advance,deadline,user);
}
bool gcn_domains_init_mask(GcnDeviceDomains* d,uint64_t now,uint32_t mask,
                          GcnDomainAdvance advance,GcnDomainDeadline deadline,void* user) {
    if(!d)return false;
    memset(d,0,sizeof(*d));d->now=now;
    if(!advance || !deadline || !mask || (mask&~GCN_DOMAIN_WII_ALL))return fail(d);
    d->advance=advance;d->deadline=deadline;d->user=user;d->dirty=mask;d->enabled=mask;
    for(unsigned i=0;i<GCN_DOMAIN_COUNT;i++) {
        d->cursor[i]=now;
        d->due[i]=UINT64_MAX;
    }
    return refresh(d);
}
void gcn_domains_changed(GcnDeviceDomains* d,uint32_t mask) {
    if(d && !d->failed) {
        if(mask&~d->enabled){fail(d);return;}
        d->dirty|=mask;
    }
}
bool gcn_domains_credit(GcnDeviceDomains* d,uint64_t now) {
    if(d && d->active)return fail(d);
    if(!target(d,now) || !refresh(d))return false;
    ++d->credits;
    /* Fixed observable order matches the functioning combined floor. Actual
     * dependencies (e.g. VI consuming completed audio/GX) are explicit in the
     * machine callback, not a synchronization of every device per MMIO. */
    for(unsigned i=0;i<GCN_DOMAIN_COUNT;i++)if(d->due[i]!=UINT64_MAX && d->due[i]<=now)
        if(!service(d,i,now))return false;
    return refresh(d);
}
bool gcn_domains_observe(GcnDeviceDomains* d,uint32_t mask,uint64_t now) {
    if(d && d->active && now!=d->now)return fail(d);
    if(!target(d,now) || !refresh(d))return false;
    if(mask&~d->enabled)return fail(d);
    ++d->observations;
    for(unsigned i=0;i<GCN_DOMAIN_COUNT;i++)if(mask&(1u<<i)) {
        if(d->active&(1u<<i))return fail(d);
        if(now!=d->cursor[i] || d->due[i]<=now)
            if(!service(d,i,now))return false;
    }
    return refresh(d);
}
uint64_t gcn_domains_deadline(GcnDeviceDomains* d,uint64_t now) {
    if(!target(d,now) || !refresh(d))return 1;
    uint64_t due=UINT64_MAX;
    for(unsigned i=0;i<GCN_DOMAIN_COUNT;i++)if(d->due[i]<due)due=d->due[i];
    return due==UINT64_MAX?UINT64_MAX:(due>now?due-now:1);
}
bool gcn_domains_observe_events(GcnDeviceDomains* d,uint32_t mask,uint64_t now) {
    if(d && d->active && now!=d->now)return fail(d);
    if(!target(d,now) || !refresh(d))return false;
    if(mask&~d->enabled)return fail(d);
    ++d->observations;++d->event_observations;
    for(unsigned i=0;i<GCN_DOMAIN_COUNT;i++)if(mask&(1u<<i)) {
        if(d->active&(1u<<i))return fail(d);
        if(d->due[i]!=UINT64_MAX && d->due[i]<=now)
            if(!service(d,i,now))return false;
    }
    return refresh(d);
}
const char* gcn_domains_identity(void) {return "bluewake-independent-device-domains-v1";}
