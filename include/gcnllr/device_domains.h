/* SPDX-License-Identifier: MIT */
#ifndef GCNLLR_DEVICE_DOMAINS_H
#define GCNLLR_DEVICE_DOMAINS_H
#include <stdbool.h>
#include <stdint.h>

enum {
    GCN_DOMAIN_AUDIO=1u, GCN_DOMAIN_GPU=2u,
    GCN_DOMAIN_STORAGE=4u, GCN_DOMAIN_VIDEO=8u, GCN_DOMAIN_ALL=15u,
    GCN_DOMAIN_IO=GCN_DOMAIN_STORAGE, GCN_DOMAIN_TIME=16u,
    GCN_DOMAIN_COUNT=5u, GCN_DOMAIN_WII_ALL=31u
};
typedef bool (*GcnDomainAdvance)(void*,uint32_t domain,uint64_t elapsed,uint64_t target);
typedef uint64_t (*GcnDomainDeadline)(void*,uint32_t domain,uint64_t cursor);
/* One owner thread. Time is the monotonic machine cycle domain, never TB.
 * Deadlines are cached absolute events. A register/queue mutation MUST call
 * changed before the next CPU budget. No title addresses or synthetic time.
 * Callback dependencies may observe OTHER domains; a dependency cycle fails.
 * A failed partial advance is terminal, never retried as a fresh delta. */
typedef struct GcnDeviceDomains {
    uint64_t cursor[GCN_DOMAIN_COUNT],due[GCN_DOMAIN_COUNT],now,next_due;
    uint64_t credits,services[GCN_DOMAIN_COUNT],observations,failures,event_observations;
    uint32_t dirty,active,enabled;
    bool failed;
    GcnDomainAdvance advance;
    GcnDomainDeadline deadline;
    void* user;
} GcnDeviceDomains;
bool gcn_domains_init(GcnDeviceDomains*,uint64_t,GcnDomainAdvance,GcnDomainDeadline,void*);
/* The legacy initializer selects the original four GameCube domains. Wii
 * explicitly enables TIME in addition to AUDIO/GPU/IO/VIDEO. */
bool gcn_domains_init_mask(GcnDeviceDomains*,uint64_t,uint32_t,GcnDomainAdvance,GcnDomainDeadline,void*);
void gcn_domains_changed(GcnDeviceDomains*,uint32_t);
bool gcn_domains_credit(GcnDeviceDomains*,uint64_t);
bool gcn_domains_observe(GcnDeviceDomains*,uint32_t,uint64_t);
/* For a native register view that computes sub-event state from `now`.
 * Retire due events; do not force cursor advancement between those events.
 * Read side effects still belong to the device, and mutations invalidate due. */
bool gcn_domains_observe_events(GcnDeviceDomains*,uint32_t,uint64_t);
uint64_t gcn_domains_deadline(GcnDeviceDomains*,uint64_t);
const char* gcn_domains_identity(void);
#endif
