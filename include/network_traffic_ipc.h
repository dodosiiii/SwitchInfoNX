#pragma once

#include <stdint.h>

#include "network_counters.h"

#define NETWORK_TRAFFIC_IPC_MAGIC UINT32_C(0x584E4953)
#define NETWORK_TRAFFIC_IPC_VERSION 1
#define NETWORK_TRAFFIC_IPC_PORT 0xD05A

typedef struct {
    uint32_t magic;
    uint32_t version;
} NetworkTrafficRequest;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t available;
    uint32_t reserved;
    NetworkCounters counters;
    NetworkCounterErrors errors;
} NetworkTrafficResponse;
