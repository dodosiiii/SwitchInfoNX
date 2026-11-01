#include "network_counters.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <net/if.h>
#include <net/route.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#define ROUTE_BUFFER_LIMIT (256 * 1024)
#define ROUTE_READ_ATTEMPTS 4

#ifndef SIOCGIFDATA
#define SIOCGIFDATA _IOW('i', 44, struct ifreq)
#endif

static bool read_route_dump(int route_type, char **buffer, size_t *length,
    int *error_code) {
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_UNSPEC, route_type, 0};
    for (int attempt = 0; attempt < ROUTE_READ_ATTEMPTS; attempt++) {
        size_t required = 0;
        if (sysctl(mib, 6, NULL, &required, NULL, 0) != 0) {
            *error_code = errno;
            return false;
        }
        if (required == 0 || required > ROUTE_BUFFER_LIMIT) {
            *error_code = required == 0 ? EIO : EOVERFLOW;
            return false;
        }

        size_t capacity = required + required / 2 + 4096;
        if (capacity > ROUTE_BUFFER_LIMIT)
            capacity = ROUTE_BUFFER_LIMIT;
        char *data = malloc(capacity);
        if (!data) {
            *error_code = ENOMEM;
            return false;
        }

        size_t actual = capacity;
        if (sysctl(mib, 6, data, &actual, NULL, 0) == 0) {
            if (actual == 0 || actual > capacity) {
                free(data);
                *error_code = EIO;
                return false;
            }
            *buffer = data;
            *length = actual;
            *error_code = 0;
            return true;
        }

        int read_error = errno;
        free(data);
        if (read_error != ENOMEM && actual <= capacity) {
            *error_code = read_error;
            return false;
        }
        *error_code = read_error;
    }
    return false;
}

static bool sum_legacy_messages(const char *buffer, size_t length,
    NetworkCounters *counters) {
    size_t offset = 0;
    bool found_message = false;
    while (offset + offsetof(struct if_msghdr, ifm_data) +
        offsetof(struct if_data, ifi_obytes) + sizeof(uint64_t) <= length) {
        const struct if_msghdr *message =
            (const struct if_msghdr *)(buffer + offset);
        if (message->ifm_msglen < sizeof(*message) ||
            message->ifm_msglen > length - offset)
            return false;
        if (message->ifm_type == RTM_IFINFO) {
            found_message = true;
            if (!(message->ifm_flags & IFF_LOOPBACK) &&
                (message->ifm_flags & IFF_UP)) {
                counters->received += message->ifm_data.ifi_ibytes;
                counters->sent += message->ifm_data.ifi_obytes;
            }
        }
        offset += message->ifm_msglen;
    }
    return found_message && offset == length;
}

static bool sum_long_messages(const char *buffer, size_t length,
    NetworkCounters *counters) {
    size_t offset = 0;
    bool found_message = false;
    while (offset + sizeof(struct if_msghdrl) <= length) {
        const struct if_msghdrl *message =
            (const struct if_msghdrl *)(buffer + offset);
        if (message->ifm_msglen < sizeof(*message) ||
            message->ifm_msglen > length - offset)
            return false;
        if (message->ifm_type == RTM_IFINFO) {
            found_message = true;
            size_t data_offset = message->ifm_data_off;
            size_t data_end = data_offset + offsetof(struct if_data, ifi_obytes) +
                sizeof(uint64_t);
            if (data_offset < sizeof(*message) ||
                data_end > message->ifm_len ||
                message->ifm_len > message->ifm_msglen)
                return false;
            if (!(message->ifm_flags & IFF_LOOPBACK) &&
                (message->ifm_flags & IFF_UP)) {
                const struct if_data *data =
                    (const struct if_data *)(buffer + offset + data_offset);
                counters->received += data->ifi_ibytes;
                counters->sent += data->ifi_obytes;
            }
        }
        offset += message->ifm_msglen;
    }
    return found_message && offset == length;
}

static bool read_route_type(int route_type, bool long_format,
    NetworkCounters *counters, int *error_code) {
    char *buffer = NULL;
    size_t length = 0;
    if (!read_route_dump(route_type, &buffer, &length, error_code))
        return false;

    NetworkCounters result = {0, 0};
    bool valid = long_format ?
        sum_long_messages(buffer, length, &result) :
        sum_legacy_messages(buffer, length, &result);
    free(buffer);
    if (!valid) {
        *error_code = EPROTO;
        return false;
    }

    *counters = result;
    *error_code = 0;
    return true;
}

static bool read_ioctl_counters(NetworkCounters *counters, int *error_code) {
    *error_code = 0;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        *error_code = errno;
        return false;
    }

    struct ifreq interfaces[64];
    memset(interfaces, 0, sizeof(interfaces));
    struct ifconf config;
    memset(&config, 0, sizeof(config));
    config.ifc_len = sizeof(interfaces);
    config.ifc_buf = (caddr_t)interfaces;
    if (ioctl(socket_fd, SIOCGIFCONF, &config) != 0) {
        *error_code = errno;
        close(socket_fd);
        return false;
    }
    if (config.ifc_len < 0 || (size_t)config.ifc_len > sizeof(interfaces)) {
        *error_code = EOVERFLOW;
        close(socket_fd);
        return false;
    }

    NetworkCounters result = {0, 0};
    bool found_interface = false;
    bool saw_active_interface = false;
    bool flags_available = false;
    size_t count = (size_t)config.ifc_len / sizeof(struct ifreq);
    if (count > sizeof(interfaces) / sizeof(interfaces[0]))
        count = sizeof(interfaces) / sizeof(interfaces[0]);

    for (size_t i = 0; i < count; i++) {
        struct ifreq request;
        memset(&request, 0, sizeof(request));
        memcpy(request.ifr_name, interfaces[i].ifr_name, sizeof(request.ifr_name));

        struct ifreq flags_request = request;
        if (ioctl(socket_fd, SIOCGIFFLAGS, &flags_request) != 0)
            continue;
        flags_available = true;
        if (!(flags_request.ifr_flags & IFF_UP) ||
            (flags_request.ifr_flags & IFF_LOOPBACK))
            continue;
        saw_active_interface = true;

        struct if_data data;
        memset(&data, 0, sizeof(data));
        request.ifr_data = (caddr_t)&data;
        if (ioctl(socket_fd, SIOCGIFDATA, &request) != 0) {
            *error_code = errno;
            continue;
        }

        result.received += data.ifi_ibytes;
        result.sent += data.ifi_obytes;
        found_interface = true;
    }

    close(socket_fd);
    if (!found_interface && saw_active_interface) {
        if (*error_code == 0)
            *error_code = ENOTSUP;
        return false;
    }
    if (!found_interface && count > 0 && !flags_available) {
        *error_code = ENOTSUP;
        return false;
    }

    *counters = result;
    *error_code = 0;
    return true;
}

bool network_counters_read(NetworkCounters *counters, NetworkCounterErrors *errors) {
    NetworkCounterErrors result_errors = {0, 0, 0};
    if (!counters) {
        if (errors) {
            result_errors.route_list_error = EINVAL;
            *errors = result_errors;
        }
        return false;
    }

    NetworkCounters result = {0, 0};
    if (read_route_type(NET_RT_IFLIST, false, &result,
        &result_errors.route_list_error)) {
        *counters = result;
        if (errors) *errors = result_errors;
        return true;
    }

#ifdef NET_RT_IFLISTL
    result.received = 0;
    result.sent = 0;
    if (read_route_type(NET_RT_IFLISTL, true, &result,
        &result_errors.route_list_long_error)) {
        *counters = result;
        if (errors) *errors = result_errors;
        return true;
    }
#else
    result_errors.route_list_long_error = ENOTSUP;
#endif

    if (read_ioctl_counters(&result, &result_errors.ioctl_error)) {
        *counters = result;
        if (errors) *errors = result_errors;
        return true;
    }

    if (errors) *errors = result_errors;
    return false;
}
