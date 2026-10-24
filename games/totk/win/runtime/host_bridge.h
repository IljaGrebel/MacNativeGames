/* Host functions the upstream runtime provides to the game (its *_override tables), with their generated adapters
   (gen_host_bridge.py). The tables are NULL-terminated; host_bridge_tables lists every file's table. */
#pragma once
#include "bridge_abi.h"
typedef struct HostBridgeEntry { const char *name; const void *fn; void (*adapter)(GuestCpu *c); } HostBridgeEntry;
extern const HostBridgeEntry *const host_bridge_tables[];
