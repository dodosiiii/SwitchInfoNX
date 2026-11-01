#include <switch.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "network_traffic_ipc.h"

#define INNER_HEAP_SIZE 0x80000

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 0;
static bool socket_ready;

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result result = smInitialize();
    if (R_FAILED(result))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    result = setsysInitialize();
    if (R_SUCCEEDED(result)) {
        SetSysFirmwareVersion firmware;
        result = setsysGetFirmwareVersion(&firmware);
        if (R_SUCCEEDED(result))
            hosversionSet(MAKEHOSVERSION(
                firmware.major, firmware.minor, firmware.micro));
        setsysExit();
    }
}

void __appExit(void) {
    if (socket_ready)
        socketExit();
    smExit();
}

static int create_server_socket(void) {
    int server = socket(AF_INET, SOCK_DGRAM, 0);
    if (server < 0)
        return -1;

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(NETWORK_TRAFFIC_IPC_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int saved_errno = errno;
        close(server);
        errno = saved_errno;
        return -1;
    }

    return server;
}

int main(void) {
    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.bsd_service_type = BsdServiceType_System;
    if (R_FAILED(socketInitialize(&config)))
        return 1;
    socket_ready = true;

    int server = create_server_socket();
    if (server < 0)
        return 2;

    for (;;) {
        NetworkTrafficRequest request;
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        ssize_t received = recvfrom(server, &request, sizeof(request), 0,
            (struct sockaddr *)&peer, &peer_length);
        if (received < 0) {
            if (errno != EINTR)
                svcSleepThread(10000000);
            continue;
        }
        if (received != sizeof(request) ||
            peer.sin_family != AF_INET ||
            peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
            request.magic != NETWORK_TRAFFIC_IPC_MAGIC ||
            request.version != NETWORK_TRAFFIC_IPC_VERSION)
            continue;

        NetworkTrafficResponse response;
        memset(&response, 0, sizeof(response));
        response.magic = NETWORK_TRAFFIC_IPC_MAGIC;
        response.version = NETWORK_TRAFFIC_IPC_VERSION;
        response.available = network_counters_read(
            &response.counters, &response.errors);
        sendto(server, &response, sizeof(response), 0,
            (struct sockaddr *)&peer, peer_length);
    }
}
