#include <thread>
#include <cstring>

#include "mtp/mtp_wrapper.h"
#include "mtp/MtpServer.h"
#include "mtp/MtpStorage.h"
#include "mtp/SwitchMtpDatabase.h"
#include "mtp/usb.h"
#include "mtp/USBMtpInterface.h"

using namespace android;

// MTP transfer progress globals
extern "C" {
volatile int mtp_xfer_active = 0;
volatile u64 mtp_xfer_done = 0;
volatile u64 mtp_xfer_total = 0;
volatile char mtp_xfer_filename[256] = {0};
}

static MtpServer *g_server = NULL;
static MtpStorage *g_storage = NULL;
static SwitchMtpDatabase *g_database = NULL;
static std::thread *g_thread = NULL;
static bool g_running = false;

static void mtp_thread_func(void) {
    struct usb_device_descriptor device_desc = {
        .bLength = USB_DT_DEVICE_SIZE,
        .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0110,
        .bDeviceClass = 0x00,
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = 0x40,
        .idVendor = 0x057e,
        .idProduct = 0x4000,
        .bcdDevice = 0x0100,
        .bNumConfigurations = 0x01
    };

    UsbInterfaceDesc infos[1];
    int num_interface = 0;

    USBMtpInterface *mtp_interface = new USBMtpInterface(num_interface, &infos[num_interface]);
    num_interface++;

    Result rc = usbInitialize(&device_desc, num_interface, infos);
    if (R_FAILED(rc)) {
        delete mtp_interface;
        g_running = false;
        return;
    }

    g_storage = new MtpStorage(
        MTP_STORAGE_REMOVABLE_RAM,
        "sdmc:/",
        "SD Card",
        1024U * 1024U * 100U,
        false,
        1024U * 1024U * 1024U * 4U - 1
    );

    g_database = new SwitchMtpDatabase();
    g_database->addStoragePath("sdmc:/", "SD Card", MTP_STORAGE_REMOVABLE_RAM, true);

    g_server = new MtpServer(
        mtp_interface,
        g_database,
        false,
        0,
        0,
        0
    );

    g_server->addStorage(g_storage);
    g_running = true;
    g_server->run();

    // Cleanup after run() returns
    delete g_server;
    g_server = NULL;
    delete g_database;
    g_database = NULL;
    delete g_storage;
    g_storage = NULL;
    delete mtp_interface;

    usbExit();
    g_running = false;
}

Result mtp_server_start(void) {
    if (g_running || g_thread) return 0;

    g_thread = new std::thread(mtp_thread_func);
    if (!g_thread) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    svcSleepThread(500000000);
    return 0;
}

void mtp_server_stop(void) {
    if (g_server) {
        g_server->stop();
    }
    if (g_thread) {
        if (g_thread->joinable())
            g_thread->join();
        delete g_thread;
        g_thread = NULL;
    }
    g_running = false;
}

bool mtp_server_is_running(void) {
    return g_running;
}
