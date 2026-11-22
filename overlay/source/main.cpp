#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <switch.h>

#include "network_counters.h"
#include "network_traffic_ipc.h"

namespace {

constexpr int HistorySize = 60;

struct TrafficSample {
    u64 received;
    u64 sent;
};

TrafficSample history[HistorySize]{};
int historyPosition = 0;
int historyCount = 0;
u64 receivedRate = 0;
u64 sentRate = 0;
u64 receivedTotal = 0;
u64 sentTotal = 0;
u64 previousReceived = 0;
u64 previousSent = 0;
u64 lastSampleTick = 0;
NetworkCounterErrors counterErrors{};
NetworkCounterErrors systemCounterErrors{};
bool haveBaseline = false;
bool countersAvailable = false;
bool socketReady = false;
bool nifmReady = false;
bool temperatureReady = false;
bool batteryReady = false;
bool clockReady = false;
bool lblReady = false;
bool systemModuleResponding = false;
bool countersFromSystemModule = false;
bool french = false;
u64 lastUiUpdateTick = 0;
std::string exportStatus = "Not exported";

const char *tr(const char *english, const char *frenchText) {
    return french ? frenchText : english;
}

std::string formatBytes(u64 value) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double amount = static_cast<double>(value);
    int unit = 0;
    while (amount >= 1000.0 && unit < 4) {
        amount /= 1000.0;
        unit++;
    }

    char text[48];
    snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.2f %s",
        amount, units[unit]);
    return text;
}

bool strCaseContains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && tolower(static_cast<unsigned char>(*h)) == tolower(static_cast<unsigned char>(*n))) {
            h++;
            n++;
        }
        if (!*n) return true;
        haystack++;
    }
    return false;
}

bool readSystemModuleCounters(NetworkTrafficResponse *response) {
    int client = socket(AF_INET, SOCK_DGRAM, 0);
    if (client < 0)
        return false;

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(NETWORK_TRAFFIC_IPC_PORT);
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    NetworkTrafficRequest request{
        NETWORK_TRAFFIC_IPC_MAGIC, NETWORK_TRAFFIC_IPC_VERSION};
    bool receivedResponse = false;

    if (sendto(client, &request, sizeof(request), 0,
            reinterpret_cast<sockaddr *>(&server), sizeof(server)) ==
        static_cast<ssize_t>(sizeof(request))) {
        pollfd descriptor{};
        descriptor.fd = client;
        descriptor.events = POLLIN;
        if (poll(&descriptor, 1, 250) > 0 && (descriptor.revents & POLLIN)) {
            NetworkTrafficResponse result{};
            ssize_t received = recv(client, &result, sizeof(result), 0);
            if (received == static_cast<ssize_t>(sizeof(result)) &&
                result.magic == NETWORK_TRAFFIC_IPC_MAGIC &&
                result.version == NETWORK_TRAFFIC_IPC_VERSION) {
                *response = result;
                receivedResponse = true;
            }
        }
    }

    close(client);
    return receivedResponse;
}

void resetHistory() {
    historyPosition = 0;
    historyCount = 0;
    receivedRate = 0;
    sentRate = 0;
    receivedTotal = 0;
    sentTotal = 0;
    previousReceived = 0;
    previousSent = 0;
    lastSampleTick = 0;
    haveBaseline = false;
    memset(history, 0, sizeof(history));
}

bool exportHistory() {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchInfoNX", 0777);
    FILE *file = fopen("sdmc:/switch/SwitchInfoNX/network_traffic.csv", "w");
    if (!file) {
        exportStatus = std::string(tr("Export failed (errno ", "Echec export (errno ")) +
            std::to_string(errno) + ")";
        return false;
    }

    bool ok = fprintf(file, "seconds_ago,received_bytes_per_second,sent_bytes_per_second\n") >= 0;
    for (int i = 0; ok && i < historyCount; i++) {
        int index = (historyPosition - historyCount + i + HistorySize) % HistorySize;
        int secondsAgo = historyCount - i - 1;
        ok = fprintf(file, "%d,%llu,%llu\n", secondsAgo,
            static_cast<unsigned long long>(history[index].received),
            static_cast<unsigned long long>(history[index].sent)) >= 0;
    }
    if (ok) {
        ok = fprintf(file, "session_total_received,%llu\nsession_total_sent,%llu\n",
            static_cast<unsigned long long>(receivedTotal),
            static_cast<unsigned long long>(sentTotal)) >= 0;
    }
    if (fclose(file) != 0)
        ok = false;

    exportStatus = ok ? tr("Saved to SD: network_traffic.csv", "Enregistre sur SD : network_traffic.csv") :
        tr("Export failed while writing the SD card.", "Echec de l'ecriture sur la carte SD.");
    return ok;
}

void sampleTraffic() {
    u64 now = armGetSystemTick();
    u64 frequency = armGetSystemTickFreq();
    if (lastSampleTick && now - lastSampleTick < frequency)
        return;

    NetworkCounters counters{};
    NetworkTrafficResponse response{};
    systemModuleResponding = socketReady && readSystemModuleCounters(&response);
    countersFromSystemModule = systemModuleResponding && response.available;
    if (countersFromSystemModule) {
        counters = response.counters;
    } else if (!socketReady) {
        counterErrors = {ENOTCONN, ENOTCONN, ENOTCONN};
        countersAvailable = false;
        receivedRate = 0;
        sentRate = 0;
        haveBaseline = false;
        return;
    } else if (!network_counters_read(&counters, &counterErrors)) {
        systemCounterErrors = systemModuleResponding ? response.errors :
            NetworkCounterErrors{};
        countersAvailable = false;
        receivedRate = 0;
        sentRate = 0;
        haveBaseline = false;
        return;
    }

    if (haveBaseline && now > lastSampleTick) {
        double elapsed = static_cast<double>(now - lastSampleTick) /
            static_cast<double>(frequency);
        u64 receivedDelta = counters.received >= previousReceived ?
            counters.received - previousReceived : counters.received;
        u64 sentDelta = counters.sent >= previousSent ?
            counters.sent - previousSent : counters.sent;
        receivedRate = static_cast<u64>(receivedDelta / elapsed);
        sentRate = static_cast<u64>(sentDelta / elapsed);
        receivedTotal += receivedDelta;
        sentTotal += sentDelta;
    } else {
        receivedRate = 0;
        sentRate = 0;
    }

    previousReceived = counters.received;
    previousSent = counters.sent;
    lastSampleTick = now;
    haveBaseline = true;
    countersAvailable = true;
    counterErrors = {};
    history[historyPosition] = {receivedRate, sentRate};
    historyPosition = (historyPosition + 1) % HistorySize;
    if (historyCount < HistorySize)
        historyCount++;
}

std::string connectionValue() {
    if (!nifmReady)
        return tr("Network service unavailable", "Service reseau indisponible");

    NifmInternetConnectionType type{};
    NifmInternetConnectionStatus status{};
    u32 signal = 0;
    Result result = nifmGetInternetConnectionStatus(&type, &signal, &status);
    if (R_FAILED(result))
        return tr("Not connected", "Non connecte");
    if (status != NifmInternetConnectionStatus_Connected)
        return tr("Connecting / offline", "Connexion / hors ligne");

    char value[80];
    const char *kind = type == NifmInternetConnectionType_Ethernet ?
        "Ethernet" : "Wi-Fi";
    if (type == NifmInternetConnectionType_WiFi)
        snprintf(value, sizeof(value), "%s (%u/3)", kind, signal);
    else
        snprintf(value, sizeof(value), "%s", kind);
    return value;
}

std::string ipValue() {
    if (!nifmReady)
        return "--";

    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0)
        return "--";

    struct in_addr address{};
    address.s_addr = ip;
    char value[INET_ADDRSTRLEN] = {0};
    return inet_ntop(AF_INET, &address, value, sizeof(value)) ?
        std::string(value) : std::string("--");
}

std::string temperatureValue(TsLocation location) {
    if (!temperatureReady)
        return "--";

    s32 temperature = 0;
    if (R_FAILED(tsGetTemperature(location, &temperature)))
        return "--";

    char value[24];
    snprintf(value, sizeof(value), "%d C", temperature);
    return value;
}

std::string clockValue(PcvModuleId module) {
    if (!clockReady)
        return "--";

    ClkrstSession session{};
    if (R_FAILED(clkrstOpenSession(&session, module, 3)))
        return "--";

    u32 rateHz = 0;
    Result rc = clkrstGetClockRate(&session, &rateHz);
    clkrstCloseSession(&session);
    if (R_FAILED(rc) || rateHz == 0)
        return "--";

    char text[32];
    snprintf(text, sizeof(text), "%u MHz", rateHz / 1000000);
    return text;
}

std::string operationModeValue() {
    AppletOperationMode mode = appletGetOperationMode();
    if (mode == AppletOperationMode_Console)
        return tr("Docked (TV)", "Docke (TV)");
    return tr("Handheld", "Portable");
}

std::string batteryValue() {
    if (!batteryReady)
        return "--";

    u32 percentage = 0;
    if (R_FAILED(psmGetBatteryChargePercentage(&percentage)))
        return "--";

    char value[24];
    snprintf(value, sizeof(value), "%u%%", percentage);
    return value;
}

std::string chargerValue() {
    if (!batteryReady)
        return "--";

    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetChargerType(&charger)))
        return "--";

    switch (charger) {
        case PsmChargerType_EnoughPower:
            return tr("Charging (Full)", "En charge (Rapide)");
        case PsmChargerType_LowPower:
            return tr("Charging (USB)", "En charge (USB)");
        case PsmChargerType_NotSupported:
            return tr("Connected (Unsupported)", "Connecte (Non supporte)");
        case PsmChargerType_Unconnected:
        default:
            return tr("On Battery", "Sur batterie");
    }
}

std::string batteryHealthValue() {
    if (!batteryReady)
        return "--";

    double age = 0.0;
    if (R_FAILED(psmGetBatteryAgePercentage(&age)))
        return "--";

    char value[24];
    snprintf(value, sizeof(value), "%.1f%%", age);
    return value;
}

std::string sdSpaceValue() {
    struct statvfs vfs{};
    if (statvfs("sdmc:/", &vfs) != 0)
        return "--";

    u64 freeBytes = static_cast<u64>(vfs.f_bavail) * vfs.f_frsize;
    u64 totalBytes = static_cast<u64>(vfs.f_blocks) * vfs.f_frsize;
    return formatBytes(freeBytes) + " / " + formatBytes(totalBytes);
}

std::string sdFreePctValue() {
    struct statvfs vfs{};
    if (statvfs("sdmc:/", &vfs) != 0 || vfs.f_blocks == 0)
        return "--";

    u64 freeBytes = static_cast<u64>(vfs.f_bavail) * vfs.f_frsize;
    u64 totalBytes = static_cast<u64>(vfs.f_blocks) * vfs.f_frsize;
    int pct = static_cast<int>((freeBytes * 100) / totalBytes);
    char text[32];
    snprintf(text, sizeof(text), "%d%% %s", pct, tr("free", "libre"));
    return text;
}

std::string brightnessValue() {
    if (!lblReady)
        return "--";

    float b = 0.0f;
    if (R_FAILED(lblGetCurrentBrightnessSetting(&b)))
        return "--";

    int pct = static_cast<int>(b * 100.0f + 0.5f);
    char text[32];
    snprintf(text, sizeof(text), "%d%%  ([A] %s)", pct, tr("Step", "Regler"));
    return text;
}

void cycleBrightness() {
    if (!lblReady)
        return;

    float b = 0.0f;
    if (R_FAILED(lblGetCurrentBrightnessSetting(&b)))
        return;

    int pct = static_cast<int>(b * 100.0f + 0.5f);
    float next = 0.50f;
    if (pct < 35) next = 0.50f;
    else if (pct < 60) next = 0.75f;
    else if (pct < 85) next = 1.00f;
    else next = 0.25f;
    lblSetCurrentBrightnessSetting(next);
}

const char *findHekatePayload() {
    if (access("sdmc:/bootloader/update.bin", F_OK) == 0)
        return "sdmc:/bootloader/update.bin";
    DIR *dir = opendir("sdmc:/bootloader/payloads");
    if (dir) {
        struct dirent *ent;
        static char found_path[512];
        while ((ent = readdir(dir)) != nullptr) {
            if (strCaseContains(ent->d_name, "hekate") && strCaseContains(ent->d_name, ".bin")) {
                snprintf(found_path, sizeof(found_path), "sdmc:/bootloader/payloads/%s", ent->d_name);
                closedir(dir);
                return found_path;
            }
        }
        closedir(dir);
    }
    if (access("sdmc:/payload.bin", F_OK) == 0)
        return "sdmc:/payload.bin";
    if (access("sdmc:/atmosphere/reboot_payload.bin", F_OK) == 0)
        return "sdmc:/atmosphere/reboot_payload.bin";
    return nullptr;
}

void rebootToPayload() {
    const char *payload = findHekatePayload();
    if (payload && strcmp(payload, "sdmc:/atmosphere/reboot_payload.bin") != 0) {
        mkdir("sdmc:/atmosphere", 0777);
        FILE *src = fopen(payload, "rb");
        if (src) {
            FILE *dst = fopen("sdmc:/atmosphere/reboot_payload.bin", "wb");
            if (dst) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
                    fwrite(buf, 1, n, dst);
                }
                fclose(dst);
            }
            fclose(src);
        }
    }
    if (R_SUCCEEDED(spsmInitialize())) {
        spsmShutdown(true);
        spsmExit();
    }
    if (R_SUCCEEDED(bpcInitialize())) {
        bpcRebootSystem();
        bpcExit();
    }
}

void rebootSystem() {
    if (R_SUCCEEDED(spsmInitialize())) {
        spsmShutdown(true);
        spsmExit();
    }
    if (R_SUCCEEDED(bpcInitialize())) {
        bpcRebootSystem();
        bpcExit();
    }
}

void shutdownSystem() {
    if (R_SUCCEEDED(spsmInitialize())) {
        spsmShutdown(false);
        spsmExit();
    }
    if (R_SUCCEEDED(bpcInitialize())) {
        bpcShutdownSystem();
        bpcExit();
    }
}

class NetworkGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("SwitchInfoNX", "v1.0 | Overlay & Actions");
        auto list = new tsl::elm::List();

        // Section 1: Hardware & Clocks
        list->addItem(new tsl::elm::CategoryHeader(tr("Hardware & Performance", "Materiel & Performances")));
        socTemperatureItem = new tsl::elm::ListItem(tr("SoC temperature", "Temperature SoC"));
        pcbTemperatureItem = new tsl::elm::ListItem(tr("PCB temperature", "Temperature carte"));
        cpuClockItem = new tsl::elm::ListItem(tr("CPU Clock", "Frequence CPU"));
        gpuClockItem = new tsl::elm::ListItem(tr("GPU Clock", "Frequence GPU"));
        ramClockItem = new tsl::elm::ListItem(tr("RAM Clock", "Frequence RAM"));
        modeItem = new tsl::elm::ListItem(tr("Operation mode", "Mode console"));
        list->addItem(socTemperatureItem);
        list->addItem(pcbTemperatureItem);
        list->addItem(cpuClockItem);
        list->addItem(gpuClockItem);
        list->addItem(ramClockItem);
        list->addItem(modeItem);

        // Section 2: Battery & Power
        list->addItem(new tsl::elm::CategoryHeader(tr("Battery & Power", "Batterie & Alimentation")));
        batteryItem = new tsl::elm::ListItem(tr("Battery level", "Niveau batterie"));
        powerSourceItem = new tsl::elm::ListItem(tr("Power source", "Alimentation"));
        batteryHealthItem = new tsl::elm::ListItem(tr("Battery health", "Sante batterie"));
        list->addItem(batteryItem);
        list->addItem(powerSourceItem);
        list->addItem(batteryHealthItem);

        // Section 3: MicroSD Storage
        list->addItem(new tsl::elm::CategoryHeader(tr("MicroSD Storage", "Stockage MicroSD")));
        sdSpaceItem = new tsl::elm::ListItem(tr("SD Space (Free/Total)", "Espace SD (Libre/Total)"));
        sdFreePctItem = new tsl::elm::ListItem(tr("SD Availability", "Disponibilite SD"));
        list->addItem(sdSpaceItem);
        list->addItem(sdFreePctItem);

        // Section 4: Screen & Brightness
        list->addItem(new tsl::elm::CategoryHeader(tr("Screen & Brightness", "Ecran & Luminosite")));
        brightnessItem = new tsl::elm::ListItem(tr("Display brightness", "Luminosite ecran"));
        brightnessItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                cycleBrightness();
                if (brightnessItem)
                    brightnessItem->setValue(brightnessValue());
                return true;
            }
            return false;
        });
        list->addItem(brightnessItem);

        // Section 5: Live Traffic & Network
        list->addItem(new tsl::elm::CategoryHeader(tr("Network & Traffic", "Reseau & Trafic live")));
        connectionItem = new tsl::elm::ListItem(tr("Connection type", "Type de connexion"));
        ipItem = new tsl::elm::ListItem("IP");
        counterItem = new tsl::elm::ListItem(tr("Interface counters", "Compteurs d'interface"));
        receiveItem = new tsl::elm::ListItem(tr("Download rate", "Debit entrant"));
        sendItem = new tsl::elm::ListItem(tr("Upload rate", "Debit sortant"));
        receiveTotalItem = new tsl::elm::ListItem(tr("Received this session", "Recu cette session"));
        sendTotalItem = new tsl::elm::ListItem(tr("Sent this session", "Envoye cette session"));
        list->addItem(connectionItem);
        list->addItem(ipItem);
        list->addItem(counterItem);
        list->addItem(receiveItem);
        list->addItem(sendItem);
        list->addItem(receiveTotalItem);
        list->addItem(sendTotalItem);

        // Section 6: Graph (Last 60s)
        list->addItem(new tsl::elm::CategoryHeader(tr("Last 60 seconds", "60 dernieres secondes")));
        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *renderer,
            s32 x, s32 y, s32 width, s32 height) {
            const s32 left = x + 20;
            const s32 top = y + 15;
            const s32 graphWidth = width - 45;
            const s32 graphHeight = height - 38;
            renderer->drawRect(left, top + graphHeight, graphWidth, 1,
                tsl::Color{0x5, 0x5, 0x5, 0xF});

            u64 maximum = 1;
            for (int i = 0; i < historyCount; i++) {
                int index = (historyPosition - historyCount + i + HistorySize) % HistorySize;
                maximum = std::max(maximum, std::max(history[index].received, history[index].sent));
            }

            for (int i = 0; i < historyCount; i++) {
                int index = (historyPosition - historyCount + i + HistorySize) % HistorySize;
                s32 px = left + (historyCount > 1 ?
                    i * (graphWidth - 1) / (historyCount - 1) : graphWidth - 1);
                s32 rxHeight = static_cast<s32>(
                    history[index].received * static_cast<u64>(graphHeight - 3) / maximum);
                s32 txHeight = static_cast<s32>(
                    history[index].sent * static_cast<u64>(graphHeight - 3) / maximum);
                renderer->drawRect(px, top + graphHeight - rxHeight, 2,
                    rxHeight > 0 ? rxHeight : 1, tsl::Color{0x0, 0xD, 0xF, 0xF});
                renderer->drawRect(px + 2, top + graphHeight - txHeight, 2,
                    txHeight > 0 ? txHeight : 1, tsl::Color{0xF, 0x7, 0x0, 0xF});
            }

            renderer->drawString("RX", false, left, top + graphHeight + 5, 16,
                tsl::Color{0x0, 0xD, 0xF, 0xF});
            renderer->drawString("TX", false, left + 55, top + graphHeight + 5, 16,
                tsl::Color{0xF, 0x7, 0x0, 0xF});
            renderer->drawString("60s", false, left + graphWidth - 50,
                top + graphHeight + 5, 16, tsl::Color{0x8, 0x8, 0x8, 0xF});
        }), 150);

        // Section 7: Power & Quick Actions
        list->addItem(new tsl::elm::CategoryHeader(tr("Power & Quick Actions", "Alimentation & Actions")));
        
        auto rebootHekateItem = new tsl::elm::ListItem(
            tr("Reboot to Hekate / Payload", "Redemarrer sur Hekate / Payload"));
        rebootHekateItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                rebootToPayload();
                return true;
            }
            return false;
        });
        list->addItem(rebootHekateItem);

        auto rebootSysItem = new tsl::elm::ListItem(
            tr("Reboot Console", "Redemarrer la console"));
        rebootSysItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                rebootSystem();
                return true;
            }
            return false;
        });
        list->addItem(rebootSysItem);

        auto shutdownItem = new tsl::elm::ListItem(
            tr("Power Off Console", "Eteindre la console"));
        shutdownItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                shutdownSystem();
                return true;
            }
            return false;
        });
        list->addItem(shutdownItem);

        auto resetItem = new tsl::elm::ListItem(tr("Reset session totals", "Reinitialiser les totaux"));
        resetItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                resetHistory();
                exportStatus = tr("Session totals reset", "Totaux de session reinitialises");
                return true;
            }
            return false;
        });
        list->addItem(resetItem);

        exportItem = new tsl::elm::ListItem(
            tr("Export 60s history to SD", "Exporter l'historique 60s sur SD"));
        exportItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                exportHistory();
                return true;
            }
            return false;
        });
        list->addItem(exportItem);
        exportItem->setValue(exportStatus, true);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        u64 now = armGetSystemTick();
        u64 frequency = armGetSystemTickFreq();
        if (lastUiUpdateTick && now - lastUiUpdateTick < frequency)
            return;
        lastUiUpdateTick = now;
        sampleTraffic();

        if (socTemperatureItem)
            socTemperatureItem->setValue(temperatureValue(TsLocation_External));
        if (pcbTemperatureItem)
            pcbTemperatureItem->setValue(temperatureValue(TsLocation_Internal));
        if (cpuClockItem)
            cpuClockItem->setValue(clockValue(PcvModuleId_CpuBus));
        if (gpuClockItem)
            gpuClockItem->setValue(clockValue(PcvModuleId_GPU));
        if (ramClockItem)
            ramClockItem->setValue(clockValue(PcvModuleId_EMC));
        if (modeItem)
            modeItem->setValue(operationModeValue());

        if (batteryItem)
            batteryItem->setValue(batteryValue());
        if (powerSourceItem)
            powerSourceItem->setValue(chargerValue());
        if (batteryHealthItem)
            batteryHealthItem->setValue(batteryHealthValue());

        if (sdSpaceItem)
            sdSpaceItem->setValue(sdSpaceValue());
        if (sdFreePctItem)
            sdFreePctItem->setValue(sdFreePctValue());

        if (brightnessItem)
            brightnessItem->setValue(brightnessValue());

        if (connectionItem)
            connectionItem->setValue(connectionValue());
        if (ipItem)
            ipItem->setValue(ipValue());
        if (exportItem)
            exportItem->setValue(exportStatus, true);

        if (!counterItem)
            return;

        if (countersAvailable) {
            counterItem->setValue(countersFromSystemModule ?
                tr("Available (system module)", "Disponible (module systeme)") :
                tr("Available (direct)", "Disponible (direct)"), true);
            receiveItem->setValue(formatBytes(receivedRate) + "/s");
            sendItem->setValue(formatBytes(sentRate) + "/s");
            receiveTotalItem->setValue(formatBytes(receivedTotal), true);
            sendTotalItem->setValue(formatBytes(sentTotal), true);
        } else {
            char error[180];
            if (systemModuleResponding) {
                snprintf(error, sizeof(error), "sys route %d/%d/%d | direct %d/%d/%d",
                    systemCounterErrors.route_list_error,
                    systemCounterErrors.route_list_long_error,
                    systemCounterErrors.ioctl_error,
                    counterErrors.route_list_error,
                    counterErrors.route_list_long_error,
                    counterErrors.ioctl_error);
            } else {
                snprintf(error, sizeof(error), "%sroute %d / routeL %d / ioctl %d",
                    tr("sysmodule offline | direct: ", "module sys. absent | direct : "),
                    counterErrors.route_list_error,
                    counterErrors.route_list_long_error,
                    counterErrors.ioctl_error);
            }
            counterItem->setValue(error);
            receiveItem->setValue("--");
            sendItem->setValue("--");
        }
    }

private:
    tsl::elm::ListItem *socTemperatureItem = nullptr;
    tsl::elm::ListItem *pcbTemperatureItem = nullptr;
    tsl::elm::ListItem *cpuClockItem = nullptr;
    tsl::elm::ListItem *gpuClockItem = nullptr;
    tsl::elm::ListItem *ramClockItem = nullptr;
    tsl::elm::ListItem *modeItem = nullptr;

    tsl::elm::ListItem *batteryItem = nullptr;
    tsl::elm::ListItem *powerSourceItem = nullptr;
    tsl::elm::ListItem *batteryHealthItem = nullptr;

    tsl::elm::ListItem *sdSpaceItem = nullptr;
    tsl::elm::ListItem *sdFreePctItem = nullptr;

    tsl::elm::ListItem *brightnessItem = nullptr;

    tsl::elm::ListItem *counterItem = nullptr;
    tsl::elm::ListItem *receiveItem = nullptr;
    tsl::elm::ListItem *sendItem = nullptr;
    tsl::elm::ListItem *receiveTotalItem = nullptr;
    tsl::elm::ListItem *sendTotalItem = nullptr;
    tsl::elm::ListItem *connectionItem = nullptr;
    tsl::elm::ListItem *ipItem = nullptr;
    tsl::elm::ListItem *exportItem = nullptr;
};

class NetworkOverlay : public tsl::Overlay {
public:
    void initServices() override {
        // Avoid libnx's multi-megabyte default BSD transfer-memory allocation.
        SocketInitConfig socketConfig{};
        socketConfig.udp_tx_buf_size = 0x1000;
        socketConfig.udp_rx_buf_size = 0x1000;
        socketConfig.sb_efficiency = 1;
        socketConfig.num_bsd_sessions = 1;
        socketConfig.bsd_service_type = BsdServiceType_User;
        socketReady = R_SUCCEEDED(socketInitialize(&socketConfig));
        nifmReady = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
        temperatureReady = R_SUCCEEDED(tsInitialize());
        batteryReady = R_SUCCEEDED(psmInitialize());
        clockReady = R_SUCCEEDED(clkrstInitialize());
        lblReady = R_SUCCEEDED(lblInitialize());

        u64 languageCode = 0;
        SetLanguage language;
        if (R_SUCCEEDED(setGetSystemLanguage(&languageCode)) &&
            R_SUCCEEDED(setMakeLanguage(languageCode, &language)))
            french = language == SetLanguage_FR || language == SetLanguage_FRCA;
    }

    void exitServices() override {
        if (lblReady)
            lblExit();
        if (clockReady)
            clkrstExit();
        if (nifmReady)
            nifmExit();
        if (batteryReady)
            psmExit();
        if (temperatureReady)
            tsExit();
        if (socketReady)
            socketExit();
        lblReady = false;
        clockReady = false;
        nifmReady = false;
        batteryReady = false;
        temperatureReady = false;
        socketReady = false;
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<NetworkGui>();
    }
};

}

int main(int argc, char **argv) {
    return tsl::loop<NetworkOverlay>(argc, argv);
}
