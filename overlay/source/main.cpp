#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

class NetworkGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("SwitchInfoNX Status", "v0.5 | live status");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(tr("System status", "Etat de la console")));
        socTemperatureItem = new tsl::elm::ListItem(tr("SoC temperature", "Temperature SoC"));
        pcbTemperatureItem = new tsl::elm::ListItem(tr("PCB temperature", "Temperature carte"));
        batteryItem = new tsl::elm::ListItem(tr("Battery", "Batterie"));
        list->addItem(socTemperatureItem);
        list->addItem(pcbTemperatureItem);
        list->addItem(batteryItem);

        list->addItem(new tsl::elm::CategoryHeader(tr("Live traffic", "Trafic en direct")));
        counterItem = new tsl::elm::ListItem(tr("Interface counters", "Compteurs d'interface"));
        receiveItem = new tsl::elm::ListItem(tr("Download rate", "Debit entrant"));
        sendItem = new tsl::elm::ListItem(tr("Upload rate", "Debit sortant"));
        receiveTotalItem = new tsl::elm::ListItem(tr("Received this session", "Recu cette session"));
        sendTotalItem = new tsl::elm::ListItem(tr("Sent this session", "Envoye cette session"));
        list->addItem(counterItem);
        list->addItem(receiveItem);
        list->addItem(sendItem);
        list->addItem(receiveTotalItem);
        list->addItem(sendTotalItem);

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

        list->addItem(new tsl::elm::CategoryHeader(tr("Connection", "Connexion")));
        connectionItem = new tsl::elm::ListItem(tr("Connection type", "Type de connexion"));
        ipItem = new tsl::elm::ListItem("IP");
        list->addItem(connectionItem);
        list->addItem(ipItem);

        list->addItem(new tsl::elm::CategoryHeader(tr("Actions", "Actions")));
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
        if (batteryItem)
            batteryItem->setValue(batteryValue());
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

        connectionItem->setValue(connectionValue());
        ipItem->setValue(ipValue());
        exportItem->setValue(exportStatus, true);
    }

private:
    tsl::elm::ListItem *socTemperatureItem = nullptr;
    tsl::elm::ListItem *pcbTemperatureItem = nullptr;
    tsl::elm::ListItem *batteryItem = nullptr;
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

        u64 languageCode = 0;
        SetLanguage language;
        if (R_SUCCEEDED(setGetSystemLanguage(&languageCode)) &&
            R_SUCCEEDED(setMakeLanguage(languageCode, &language)))
            french = language == SetLanguage_FR || language == SetLanguage_FRCA;
    }

    void exitServices() override {
        if (nifmReady)
            nifmExit();
        if (batteryReady)
            psmExit();
        if (temperatureReady)
            tsExit();
        if (socketReady)
            socketExit();
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
