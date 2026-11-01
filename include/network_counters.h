#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t received;
    uint64_t sent;
} NetworkCounters;

typedef struct {
    int route_list_error;
    int route_list_long_error;
    int ioctl_error;
} NetworkCounterErrors;

bool network_counters_read(NetworkCounters *counters, NetworkCounterErrors *errors);

#ifdef __cplusplus
}
#endif
