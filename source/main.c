#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <arpa/inet.h>
#include <switch/services/fan.h>
#include <switch/services/acc.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#include "mtp/mtp_wrapper.h"
#include "save_manager.h"
#include "game_icons.h"
#include "network_counters.h"

#define W 1280
#define H 720
#define PGS 9
#define APP_VERSION "0.6"
#define APP_NAME "SwitchInfoNX"

// ─── Color definitions ────────────────────────────────────
static SDL_Color color_bg = {18, 18, 20, 255};
static SDL_Color color_card = {28, 28, 32, 255};
static SDL_Color color_card_border = {42, 42, 48, 255};
static SDL_Color color_cyan = {0, 210, 255, 255};
static SDL_Color color_green = {0, 255, 136, 255};
static SDL_Color color_red = {255, 51, 102, 255};
static SDL_Color color_yellow = {255, 204, 0, 255};
static SDL_Color color_white = {255, 255, 255, 255};
static SDL_Color color_grey = {150, 150, 160, 255};
static SDL_Color color_dark_grey = {55, 55, 62, 255};
static SDL_Color color_bg2 = {22, 22, 26, 255};
static SDL_Color color_bg3 = {34, 34, 42, 255};
static SDL_Color color_purple = {180, 100, 255, 255};
static SDL_Color color_orange = {255, 150, 50, 255};

// static const char *pgname[PGS] = {
//     "System", "Storage", "Network", "Transfer", "Perf", "Controller", "Tools", "About", "Settings"
// };

// Tick at app start for uptime
static u64 start_tick = 0;
// Refresh counter
static u32 refresh_count = 0;
// FPS counter
static u64 fps_last_tick = 0;
static int fps_count = 0;
static int current_fps = 0;
// Network connection uptime
static u64 net_connected_tick = 0;
// Brightness
static float ctrl_brightness = -1.0f;
// Dead-pixel screen test
static int screen_test_active = 0;
#define SCREEN_TEST_PHASES 6
static bool lbl_ready = false;
static bool lbl_emulator = true;

static TTF_Font *font_xs = NULL;
static TTF_Font *font_sm = NULL;
static TTF_Font *font_md = NULL;
static TTF_Font *font_lg = NULL;
static void text_cache_clear(void);
// System shared fonts are split by script on Switch. Keep the available
// families so a missing glyph in one language can be drawn from another.
static TTF_Font *ui_font_families[PlSharedFontType_Total][4];
static PlFontData ui_font_data[PlSharedFontType_Total];
static bool ui_font_data_available[PlSharedFontType_Total];
static const int ui_font_sizes[4] = {14, 18, 24, 36};

static HidVibrationDeviceHandle vibe_handles[2];
static bool vibe_init_ok = false;
static int vibe_handle_count = 0;
static bool vibe_session_started = false;
static bool vibe_permission_ready = false;
static bool vibe_active[2] = {false, false};
static u64 vibe_last_send_tick[2] = {0, 0};
static int vibe_stop_retries[2] = {0, 0};
static int touch_vibe_index = -1;
static void add_alert(const char *msg);

static bool init_vibration_for_pad(PadState *pad) {
    if (vibe_init_ok) return true;
    if (!vibe_permission_ready) {
        Result permit_rc = hidBeginPermitVibrationSession();
        if (R_SUCCEEDED(permit_rc)) {
            vibe_session_started = true;
            vibe_permission_ready = true;
        } else {
            bool already_permitted = false;
            if (R_SUCCEEDED(hidIsVibrationPermitted(&already_permitted)) && already_permitted)
                vibe_permission_ready = true;
            else
                return false;
        }
    }
    HidNpadIdType id = padIsHandheld(pad) ? HidNpadIdType_Handheld : HidNpadIdType_No1;
    u32 styles = hidGetNpadStyleSet(id);
    struct { u32 style; int count; } candidates[] = {
        { HidNpadStyleTag_NpadHandheld, 2 }, { HidNpadStyleTag_NpadJoyDual, 2 },
        { HidNpadStyleTag_NpadFullKey, 2 }, { HidNpadStyleTag_NpadJoyLeft, 1 },
        { HidNpadStyleTag_NpadJoyRight, 1 }
    };
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!(styles & candidates[i].style)) continue;
        Result rc = hidInitializeVibrationDevices(vibe_handles, candidates[i].count, id, candidates[i].style);
        if (R_SUCCEEDED(rc)) {
            vibe_handle_count = candidates[i].count;
            vibe_init_ok = true;
            return true;
        }
    }
    return false;
}

static void vibration_stop(int index) {
    if (index < 0 || index >= 2) return;
    if (vibe_active[index] && vibe_stop_retries[index] == 0) vibe_stop_retries[index] = 3;
    if (vibe_stop_retries[index] == 0) return;
    if (index < vibe_handle_count) {
        // libnx/Switch neutral rumble value: zero amplitude at the standard bands.
        HidVibrationValue stop = {0.0f, 160.0f, 0.0f, 320.0f};
        if (R_FAILED(hidSendVibrationValue(vibe_handles[index], &stop))) return;
    }
    if (--vibe_stop_retries[index] <= 0) {
        vibe_stop_retries[index] = 0;
        vibe_active[index] = false;
    }
}

static void vibration_hold(int index, bool pressed) {
    if (!pressed) { vibration_stop(index); return; }
    if (!vibe_init_ok || index < 0 || index >= vibe_handle_count) return;
    u64 now = armGetSystemTick();
    if (vibe_active[index] && now - vibe_last_send_tick[index] < armGetSystemTickFreq() / 8) return;
    HidVibrationValue max_rumble = {160.0f, 1.0f, 320.0f, 1.0f};
    if (R_SUCCEEDED(hidSendVibrationValue(vibe_handles[index], &max_rumble))) {
        vibe_active[index] = true;
        vibe_stop_retries[index] = 0;
        vibe_last_send_tick[index] = now;
    } else {
        vibe_active[index] = false;
    }
}

static u64 ctrl_rumble_until[2] = {0, 0};
static float stick_l_peak_drift = 0.0f;
static float stick_r_peak_drift = 0.0f;

static void ctrl_trigger_rumble(int index, u32 ms) {
    if (!vibe_init_ok) {
        PadState dummy_pad;
        padInitializeAny(&dummy_pad);
        init_vibration_for_pad(&dummy_pad);
    }
    u64 ticks = (armGetSystemTickFreq() * (u64)ms) / 1000ULL;
    u64 now = armGetSystemTick();
    if (index == 0 || index == -1) {
        ctrl_rumble_until[0] = now + ticks;
        vibration_hold(0, true);
    }
    if (index == 1 || index == -1) {
        ctrl_rumble_until[1] = now + ticks;
        vibration_hold(1, true);
    }
}

static SDL_Color color_from_npad(u32 c, SDL_Color fallback) {
    if (c == 0) return fallback;
    u8 r = (c >> 24) & 0xFF;
    u8 g = (c >> 16) & 0xFF;
    u8 b = (c >> 8) & 0xFF;
    if (r == 0 && g == 0 && b == 0) {
        r = c & 0xFF;
        g = (c >> 8) & 0xFF;
        b = (c >> 16) & 0xFF;
    }
    if (r == 0 && g == 0 && b == 0) return fallback;
    return (SDL_Color){r, g, b, 255};
}

// Blue light filter
static int blue_light_filter = 0; // 0=off, 1-5 intensity

// Header cache: battery (avoid calling system APIs every frame)
static u32  header_cached_batt    = 0;
static PsmChargerType header_cached_charger = PsmChargerType_Unconnected;
static u64  header_batt_tick      = 0;

// Auto-refresh tracking (global for header access)
static u64 last_refresh = 0;

// Export status message
static char export_msg[128] = {0};
static u64 export_msg_tick = 0;
static char perf_export_msg[160] = {0};
static u64 perf_export_msg_tick = 0;
static bool perf_export_success = false;
static Result clock_last_error = 0;
static float sd_speed_result = 0.0f; // MB/s, 0 = not tested, -2 = testing, -1 = error

// Toast notification system
#define NOTIF_MAX 6
#define NOTIF_DURATION 3
static char notif_msgs[NOTIF_MAX][128];
static u64 notif_ticks[NOTIF_MAX];
static int notif_count = 0;

// FPS history chart
#define FPS_HIST_SIZE 60
static int fps_hist[FPS_HIST_SIZE] = {0};
static int fps_hist_pos = 0;
static int fps_hist_count = 0;
static u64 perf_hist_last_tick = 0;

// CPU/Temp/Memory history
#define PERF_HIST_SIZE 60
static int cpu_hist[PERF_HIST_SIZE] = {0};
static int cpu_hist_pos = 0;
static int cpu_hist_count = 0;
static int temp_hist[PERF_HIST_SIZE] = {0};
static int temp_hist_pos = 0;
static int temp_hist_count = 0;
static int mem_hist[PERF_HIST_SIZE] = {0};
static int mem_hist_pos = 0;
static int mem_hist_count = 0;

static bool history_stats(const int *values, int count, int pos, int capacity,
    int *minimum, int *average, int *maximum) {
    if (!values || count <= 0 || count > capacity || capacity <= 0) return false;
    int first = (pos - count + capacity) % capacity;
    int min_value = values[first], max_value = values[first];
    long long total = 0;
    for (int i = 0; i < count; i++) {
        int value = values[(first + i) % capacity];
        if (value < min_value) min_value = value;
        if (value > max_value) max_value = value;
        total += value;
    }
    if (minimum) *minimum = min_value;
    if (average) *average = (int)(total / count);
    if (maximum) *maximum = max_value;
    return true;
}

static void format_duration(u64 seconds, char *out, size_t out_len) {
    u64 hours = seconds / 3600;
    u64 minutes = (seconds % 3600) / 60;
    u64 remainder = seconds % 60;
    snprintf(out, out_len, "%02llu:%02llu:%02llu",
        (unsigned long long)hours, (unsigned long long)minutes,
        (unsigned long long)remainder);
}

// Alert thresholds (can be exposed to settings later)
static s32 temp_alert_millic = 70000; // 70.0 C
static float mem_alert_frac = 0.85f; // 85%
static bool temp_alert_notified = false;
static bool mem_alert_notified = false;

// ─── Settings: Language & Theme ────────────────────────────
#define LANG_EN 0
#define LANG_FR 1
#define LANG_IT 2
#define LANG_ES 3
#define LANG_DE 4
#define LANG_PT 5
#define LANG_NL 6
#define LANG_JA 7
#define LANG_RU 8
#define LANG_ZH 9
#define LANG_KO 10
#define LANG_MAX 11
static int cur_lang = LANG_EN;

static bool load_ui_fonts(void) {
    int primary = PlSharedFontType_Standard;
    if (cur_lang == LANG_ZH) primary = PlSharedFontType_ChineseSimplified;
    else if (cur_lang == LANG_KO) primary = PlSharedFontType_KO;

    PlFontData next_data[PlSharedFontType_Total] = {{0}};
    bool next_available[PlSharedFontType_Total] = {false};
    for (int family = 0; family < PlSharedFontType_Total; family++) {
        if (R_FAILED(plGetSharedFontByType(&next_data[family], (PlSharedFontType)family)) || !next_data[family].address || !next_data[family].size)
            continue;
        next_available[family] = true;
    }
    if (!next_available[primary]) {
        // Standard is a safe fallback if an optional language pack is absent.
        primary = PlSharedFontType_Standard;
    }
    if (!next_available[primary]) return false;
    // Cached glyph textures belong to the old language fonts.
    text_cache_clear();
    TTF_Font *next_primary[4] = {0};
    for (int size = 0; size < 4; size++) {
        SDL_RWops *rw = SDL_RWFromMem(next_data[primary].address, next_data[primary].size);
        next_primary[size] = rw ? TTF_OpenFontRW(rw, 1, ui_font_sizes[size]) : NULL;
        if (!next_primary[size]) {
            for (int close_size = 0; close_size < 4; close_size++) {
                if (next_primary[close_size]) TTF_CloseFont(next_primary[close_size]);
            }
            return false;
        }
    }
    for (int family = 0; family < PlSharedFontType_Total; family++) {
        for (int size = 0; size < 4; size++) {
            if (ui_font_families[family][size]) TTF_CloseFont(ui_font_families[family][size]);
            ui_font_families[family][size] = NULL;
        }
        ui_font_data[family] = next_data[family];
        ui_font_data_available[family] = next_available[family];
    }
    for (int size = 0; size < 4; size++) ui_font_families[primary][size] = next_primary[size];
    font_xs = ui_font_families[primary][0];
    font_sm = ui_font_families[primary][1];
    font_md = ui_font_families[primary][2];
    font_lg = ui_font_families[primary][3];
    return true;
}

static const char *lang_names[LANG_MAX] = { "English", "Francais", "Italiano", "Espanol", "Deutsch", "Portugues", "Nederlands", "日本語", "Русский", "中文", "한국어" };
static const char *tab_names[LANG_MAX][PGS] = {
    { "System", "Storage", "Network", "Transfer", "Perf", "Controller", "Tools", "About", "Settings" },
    { "Systeme", "Stockage", "Reseau", "Transfert", "Perf", "Manette", "Outils", "A propos", "Reglages" },
    { "Sistema", "Archiv.", "Rete", "Trasf.", "Prest.", "Controller", "Strumenti", "Info", "Impost." },
    { "Sistema", "Almac.", "Red", "Transf.", "Rendim.", "Mando", "Herram.", "Acerca", "Ajustes" },
    { "System", "Speicher", "Netzwerk", "Uebertr.", "Leistung", "Controller", "Werkzeuge", "Ueber", "Einstell." },
    { "Sistema", "Armaz.", "Rede", "Transf.", "Desemp.", "Controle", "Ferramentas", "Sobre", "Config." },
    { "Systeem", "Opslag", "Netwerk", "Overdr.", "Prest.", "Controller", "Gereedschap", "Over", "Instell." },
    { "システム", "ストレージ", "ネットワーク", "転送", "性能", "コントローラー", "ツール", "情報", "設定" },
    { "Система", "Память", "Сеть", "Передача", "Производительность", "Контроллер", "Инструменты", "О программе", "Настройки" },
    { "系统", "存储", "网络", "传输", "性能", "控制器", "工具", "关于", "设置" },
    { "시스템", "저장소", "네트워크", "전송", "성능", "컨트롤러", "도구", "정보", "설정" },
};
static const char *settings_title[LANG_MAX] = {
    "Settings", "Reglages", "Impostazioni", "Ajustes", "Einstellungen", "Configuracoes", "Instellingen", "設定", "Настройки", "设置", "설정"
};
static const char *settings_lang_label[LANG_MAX] = {
    "Language", "Langue", "Lingua", "Idioma", "Sprache", "Idioma", "Taal", "言語", "Язык", "语言", "언어"
};
static const char *settings_theme_label[LANG_MAX] = {
    "Theme", "Theme", "Tema", "Tema", "Design", "Tema", "Thema", "テーマ", "Тема", "主题", "테마"
};
static const char *settings_refresh_label[LANG_MAX] = {
    "Auto Refresh", "Auto Rafraichir", "Aggiorn. Auto", "Auto Actualizar", "Auto Aktual.", "Auto Atualizar", "Auto Ververs.", "自動更新", "Автообновление", "自动刷新", "자동 새로고침"
};
static const char *settings_temp_alert_label[LANG_MAX] = {
    "Temp Alert", "Alerte Temp", "Allerta Temp", "Alerta Temp", "Temp Alarm", "Alerta Temp", "Temp Alarm", "温度警告", "Предупреждение о температуре", "温度警报", "온도 경고"
};
static const char *settings_mem_alert_label[LANG_MAX] = {
    "Memory Alert", "Alerte Memoire", "Allerta Memoria", "Alerta Memoria", "Speicher Alarm", "Alerta Memoria", "Geheugen Alarm", "メモリ警告", "Предупреждение о памяти", "内存警报", "메모리 경고"
};
static const char *settings_info[LANG_MAX] = {
    "Press B to return   |   D-Pad Up/Down to change",
    "Appuyez sur B pour revenir   |   D-Pad Haut/Bas pour changer",
    "Premi B per tornare   |   D-Pad Su/Giu per cambiare",
    "Presione B para volver   |   D-Pad Arriba/Abajo para cambiar",
    "Druecke B zum Zurueck   |   D-Pad Hoch/Runter zum Aendern",
    "Pressione B para voltar   |   D-Pad Cima/Baixo para mudar",
    "Druk B om terug te keren   |   D-Pad Omhoog/Omlaag om te wijzigen",
    "Bで戻る   |   十字キー上下で選択",
    "Нажмите B для возврата | крестовина: выбор",
    "按 B 返回 | 方向键上下选择",
    "B: 뒤로 | 방향키 위/아래: 선택"
};

// ─── Comprehensive i18n string table ────────────────────────

// --- Page 0: System ---
static const char *s_fw_hw[LANG_MAX] = {
    "Firmware & Hardware", "Firmware & Materiel", "Firmware e Hardware", "Firmware y Hardware",
    "Firmware & Hardware", "Firmware e Hardware", "Firmware & Hardware",
    "ファームウェアと本体",
    "Прошивка и устройство",
    "固件与主机",
    "펌웨어 및 본체"};
static const char *s_firmware[LANG_MAX] = {
    "Firmware", "Firmware", "Firmware", "Firmware", "Firmware", "Firmware", "Firmware",
    "ファームウェア",
    "Прошивка",
    "系统固件",
    "시스템 펌웨어"};
static const char *s_serial[LANG_MAX] = {
    "Serial No.", "No. Serie", "N. Seriale", "No. Serie",
    "Serien-Nr.", "N. Serie", "Serie Nr.",
    "製造番号",
    "Серийный номер",
    "序列号",
    "일련 번호"};
static const char *s_hardware[LANG_MAX] = {
    "Hardware", "Materiel", "Hardware", "Hardware",
    "Hardware", "Hardware", "Hardware",
    "本体",
    "Устройство",
    "主机",
    "기기"};
static const char *s_mode[LANG_MAX] = {
    "Mode", "Mode", "Modalita", "Modo",
    "Modus", "Modo", "Modus",
    "モード",
    "Режим",
    "模式",
    "모드"};
static const char *s_docked[LANG_MAX] = {
    "Docked (TV Output)", "Dock (Sortie TV)", "Dock (Uscita TV)", "Acoplado (Salida TV)",
    "Docked (TV-Ausgang)", "Dock (Saida TV)", "Gedockt (TV-uitgang)",
    "TVモード",
    "Режим ТВ",
    "底座模式（电视输出）",
    "TV 모드"};
static const char *s_handheld[LANG_MAX] = {
    "Handheld (Portable)", "Portable", "Portatile", "Portatil",
    "Handheld (Tragbar)", "Portatil", "Handheld (Draagbaar)",
    "携帯モード",
    "Портативный режим",
    "掌机模式",
    "휴대 모드"};
static const char *s_arch[LANG_MAX] = {
    "Architecture", "Architecture", "Architettura", "Arquitectura",
    "Architektur", "Arquitetura", "Architectuur",
    "アーキテクチャ",
    "Архитектура",
    "处理器架构",
    "아키텍처"};
static const char *s_arch_val[LANG_MAX] = {
    "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)",
    "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)",
    "ARMv8-A (4x Cortex-A57)",
    "ARMv8-A (4x Cortex-A57)",
    "ARMv8-A (4x Cortex-A57)",
    "ARMv8-A (4x Cortex-A57)"};
static const char *s_dev_name[LANG_MAX] = {
    "Device Name", "Nom de l'appareil", "Nome dispositivo", "Nombre del dispositivo",
    "Geraetename", "Nome do dispositivo", "Apparaatnaam",
    "本体名",
    "Имя устройства",
    "设备名称",
    "기기 이름"};
static const char *s_region[LANG_MAX] = {
    "Region", "Region", "Regione", "Region",
    "Region", "Regiao", "Regio",
    "地域",
    "Регион",
    "地区",
    "지역"};
static const char *s_sys_lang[LANG_MAX] = {
    "System Language", "Langue du systeme", "Lingua di sistema", "Idioma del sistema",
    "Systemsprache", "Idioma do sistema", "Systeemtaal",
    "本体の言語",
    "Язык системы",
    "系统语言",
    "시스템 언어"};
static const char *s_batt_power[LANG_MAX] = {
    "Battery & Power", "Batterie & Alimentation", "Batteria & Alimentazione", "Bateria y Alimentacion",
    "Akku & Strom", "Bateria & Energia", "Batterij & Stroom",
    "バッテリーと電源",
    "Батарея и питание",
    "电池与电源",
    "배터리 및 전원"};
static const char *s_batt_level[LANG_MAX] = {
    "Battery Level", "Niveau batterie", "Livello batteria", "Nivel de bateria",
    "Akku-Stand", "Nivel da bateria", "Batterijniveau",
    "電池残量",
    "Уровень заряда",
    "电池电量",
    "배터리 잔량"};
static const char *s_charging_label[LANG_MAX] = {
    "Charging", "Charge", "Carica", "Carga",
    "Ladevorgang", "Carregando", "Opladen",
    "充電",
    "Зарядка",
    "充电",
    "충전"};
static const char *s_charging[LANG_MAX] = {
    "Charging", "En charge", "In carica", "Cargando",
    "Laden", "Carregando", "Opladen",
    "充電中",
    "Заряжается",
    "正在充电",
    "충전 중"};
static const char *s_discharging[LANG_MAX] = {
    "Discharging", "Decharge", "In scarica", "Descargando",
    "Entladen", "Descarregando", "Ontladen",
    "放電中",
    "Разряжается",
    "正在放电",
    "방전 중"};
static const char *s_charger[LANG_MAX] = {
    "Charger", "Chargeur", "Caricatore", "Cargador",
    "Ladegeraet", "Carregador", "Lader",
    "充電器",
    "Зарядное устройство",
    "充电器",
    "충전기"};
static const char *s_ac_adapter[LANG_MAX] = {
    "AC Adapter (USB-PD)", "Adaptateur secteur (USB-PD)", "Adattatore AC (USB-PD)", "Adaptador CA (USB-PD)",
    "Netzteil (USB-PD)", "Adaptador AC (USB-PD)", "Netadapter (USB-PD)",
    "ACアダプター（USB-PD）",
    "Адаптер питания (USB-PD)",
    "电源适配器（USB-PD）",
    "전원 어댑터 (USB-PD)"};
static const char *s_usb_slow[LANG_MAX] = {
    "USB Power (Slow)", "USB (Lent)", "USB (Lento)", "USB (Lento)",
    "USB (Langsam)", "USB (Lento)", "USB (Langzaam)",
    "USB給電（低速）",
    "Питание USB (медленно)",
    "USB 供电（低速）",
    "USB 전원 (저속)"};
static const char *s_none[LANG_MAX] = {
    "None", "Aucun", "Nessuno", "Ninguno",
    "Keiner", "Nenhum", "Geen",
    "なし",
    "Нет",
    "无",
    "없음"};
static const char *s_capacity[LANG_MAX] = {
    "Capacity", "Capacite", "Capacita", "Capacidad",
    "Kapazitaet", "Capacidade", "Capaciteit",
    "容量",
    "Ёмкость",
    "容量",
    "용량"};
static const char *s_jc_l[LANG_MAX] = {
    "Joy-Con L", "Joy-Con G", "Joy-Con S", "Joy-Con I",
    "Joy-Con L", "Joy-Con E", "Joy-Con L",
    "Joy-Con L",
    "Joy-Con L",
    "Joy-Con L",
    "Joy-Con L"};
static const char *s_jc_r[LANG_MAX] = {
    "Joy-Con R", "Joy-Con D", "Joy-Con D", "Joy-Con D",
    "Joy-Con R", "Joy-Con D", "Joy-Con R",
    "Joy-Con R",
    "Joy-Con R",
    "Joy-Con R",
    "Joy-Con R"};
static const char *s_thermals[LANG_MAX] = {
    "Thermals & Display", "Thermique & Ecran", "Termiche & Display", "Termica y Pantalla",
    "Thermik & Display", "Termica & Ecra", "Thermiek & Scherm",
    "温度と画面",
    "Температура и экран",
    "温度与显示",
    "온도 및 화면"};
static const char *s_skin_temp[LANG_MAX] = {
    "Skin Temp", "Temp. boitier", "Temp. scocca", "Temp. carcasa",
    "Gehaeuse-Temp.", "Temp. do corpo", "Behuizing temp.",
    "皮膚温度",
    "Температура корпуса",
    "皮肤温度",
    "표면 온도"};
static const char *s_skin_history[LANG_MAX] = {
    "Skin Temperature Samples (60)", "Mesures temperature boitier (60)",
    "Campioni temperatura scocca (60)", "Muestras temperatura carcasa (60)",
    "Gehaeusetemperatur-Messungen (60)", "Amostras temperatura do corpo (60)",
    "Behuizingstemperatuurmetingen (60)", "表面温度のサンプル (60)",
    "Замеры температуры корпуса (60)", "机身温度采样 (60)", "표면 온도 샘플 (60)"};
static const char *s_history_stats_fmt[LANG_MAX] = {
    "App samples  min %d  avg %d  max %d", "Mesures appli  min %d  moy %d  max %d",
    "Campioni app  min %d  media %d  max %d", "Muestras app  min %d  media %d  max %d",
    "App-Messungen  min %d  Mittel %d  max %d", "Amostras app  min %d  media %d  max %d",
    "App-metingen  min %d  gem. %d  max %d", "App samples  min %d  avg %d  max %d",
    "App samples  min %d  avg %d  max %d", "App samples  min %d  avg %d  max %d",
    "App samples  min %d  avg %d  max %d"};
static const char *s_battery_condition[LANG_MAX] = {
    "Estimated battery health", "Sante batterie estimee", "Salute batteria stimata", "Salud estimada de bateria",
    "Geschatzter Akkuzustand", "Saude estimada da bateria", "Geschatte batterijconditie",
    "推定バッテリー状態", "Оценка состояния батареи", "电池健康估算", "배터리 건강 추정치"};
static const char *s_battery_power_est[LANG_MAX] = {
    "Power (est.)", "Puissance (est.)", "Potenza (stim.)",
    "Potencia (est.)", "Leistung (ges.)", "Potencia (est.)",
    "Verm. (ges.)", "Power (est.)", "Power (est.)",
    "Power (est.)", "Power (est.)"};
static const char *s_transfer_eta[LANG_MAX] = {
    "ETA %s", "Temps restant %s", "Tempo rimasto %s", "Tiempo restante %s",
    "Restzeit %s", "Tempo restante %s", "Resterend %s",
    "残り %s", "Осталось %s", "剩余 %s", "남은 시간 %s"};
static const char *s_stick_test_hint[LANG_MAX] = {
    "Release sticks to inspect their resting position", "Relachez les sticks pour observer leur position au repos",
    "Rilascia gli stick per osservarne la posizione a riposo", "Suelta los sticks para observar su posicion en reposo",
    "Sticks loslassen, um die Ruheposition zu pruefen", "Solte os sticks para observar a posicao em repouso",
    "Laat de sticks los om de rustpositie te bekijken", "スティックを離して静止位置を確認",
    "Отпустите стики, чтобы проверить положение покоя", "松开摇杆以观察静止位置", "스틱을 놓고 정지 위치를 확인하세요"};
static const char *s_theme_preview[LANG_MAX] = {
    "Live theme preview", "Apercu du theme", "Anteprima tema", "Vista previa del tema",
    "Designvorschau", "Pre-visualizacao do tema", "Themapreview",
    "テーマプレビュー", "Предпросмотр темы", "主题预览", "테마 미리보기"};
static const char *s_cte_quick[LANG_MAX][3] = {
    {"Ocean", "Sunset", "Forest"}, {"Ocean", "Coucher de soleil", "Foret"},
    {"Oceano", "Tramonto", "Foresta"}, {"Oceano", "Atardecer", "Bosque"},
    {"Ozean", "Sonnenuntergang", "Wald"}, {"Oceano", "Por do sol", "Floresta"},
    {"Oceaan", "Zonsondergang", "Bos"}, {"海", "夕焼け", "森"},
    {"Океан", "Закат", "Лес"}, {"海洋", "日落", "森林"}, {"바다", "노을", "숲"}};
static const char *s_tutorial_title[LANG_MAX] = {
    "Welcome to SwitchInfoNX", "Bienvenue dans SwitchInfoNX", "Benvenuto in SwitchInfoNX",
    "Bienvenido a SwitchInfoNX", "Willkommen bei SwitchInfoNX", "Bem-vindo ao SwitchInfoNX",
    "Welkom bij SwitchInfoNX", "SwitchInfoNXへようこそ", "Добро пожаловать в SwitchInfoNX",
    "欢迎使用 SwitchInfoNX", "SwitchInfoNX에 오신 것을 환영합니다"};
static const char *s_tutorial_steps[LANG_MAX][5] = {
    {"Your dashboard shows console health. Use tabs or L/R to explore. Move the blue cursor with either stick and press A to select.",
     "Scroll with the right stick vertically or the D-pad. Drag the screen to move through lists and details.",
     "Storage shows SD usage. Open the browser to inspect files; tap a folder to enter it.",
     "Performance tracks FPS and temperature. Network and Transfer show connection diagnostics and transfer tools.",
     "Tools includes dated game-save backups. Select a save, make a backup, then choose a backup before restoring. Controller tests show stick and button input."},
    {"Le tableau de bord resume l'etat de la console. Onglets ou L/R pour naviguer; bougez le curseur bleu avec un stick et appuyez sur A pour valider.",
     "Faites defiler avec le stick droit verticalement ou la croix directionnelle. Glissez l'ecran pour parcourir les listes.",
     "Stockage affiche l'espace SD. Ouvrez l'explorateur et touchez un dossier pour voir son contenu.",
     "Performances suit les FPS et la temperature. Reseau et Transfert regroupent diagnostics et outils de transfert.",
     "Outils contient les sauvegardes datees. Choisissez une sauvegarde, creez une copie, puis selectionnez-la avant restauration. La page Manette teste boutons et sticks."},
    {"La dashboard riassume lo stato della console. Usa schede o L/R; muovi il cursore blu con uno stick e premi A per selezionare.",
     "Scorri verticalmente con lo stick destro o la croce direzionale. Trascina lo schermo per vedere gli elenchi.",
     "Archiviazione mostra lo spazio SD. Apri il browser e tocca una cartella per esplorarla.",
     "Prestazioni registra FPS e temperatura. Rete e Trasferimento offrono diagnostica e strumenti di trasferimento.",
     "Strumenti contiene backup datati dei salvataggi. Seleziona un salvataggio, crea una copia e sceglila prima del ripristino. Controller prova pulsanti e stick."},
    {"El panel resume el estado de la consola. Usa pestanas o L/R; mueve el cursor azul con un stick y pulsa A para elegir.",
     "Desplazate verticalmente con el stick derecho o la cruceta. Arrastra para recorrer las listas.",
     "Almacenamiento muestra el espacio SD. Abre el explorador y toca una carpeta para entrar.",
     "Rendimiento registra FPS y temperatura. Red y Transferencia ofrecen diagnosticos y herramientas.",
     "Herramientas incluye copias fechadas de partidas. Elige una partida, crea una copia y seleccionala antes de restaurar. Mando prueba botones y sticks."},
    {"Das Dashboard zeigt den Zustand der Konsole. Tabs oder L/R wechseln Bereiche; bewege den blauen Cursor mit einem Stick und druecke A.",
     "Scrolle vertikal mit dem rechten Stick oder Steuerkreuz. Ziehe fuer Listen und Details.",
     "Speicher zeigt den SD-Platz. Oeffne den Dateibrowser und tippe einen Ordner an.",
     "Leistung zeichnet FPS und Temperatur auf. Netzwerk und Transfer bieten Diagnose und Uebertragung.",
     "Tools enthaelt datierte Spielstand-Sicherungen. Spielstand waehlen, sichern und vor dem Wiederherstellen eine Sicherung auswaehlen. Controller testet Tasten und Sticks."},
    {"O painel resume o estado da consola. Use abas ou L/R; mova o cursor azul com um stick e pressione A para selecionar.",
     "Role verticalmente com o stick direito ou a cruz direcional. Arraste para percorrer as listas.",
     "Armazenamento mostra o espaco SD. Abra o navegador e toque numa pasta.",
     "Desempenho regista FPS e temperatura. Rede e Transferencia oferecem diagnosticos e ferramentas.",
     "Ferramentas guarda copias datadas dos jogos. Escolha um jogo, crie uma copia e selecione-a antes de restaurar. Controlador testa botoes e analógicos."},
    {"Het dashboard toont de consolestatus. Wissel met tabbladen of L/R; beweeg de blauwe cursor met een stick en druk op A.",
     "Scroll verticaal met de rechterstick of richtingsknoppen. Sleep om lijsten te bekijken.",
     "Opslag toont SD-ruimte. Open de bestandsbrowser en tik op een map om die te openen.",
     "Prestaties registreert FPS en temperatuur. Netwerk en Overdracht bieden diagnose en overdracht.",
     "Tools bevat gedateerde gameback-ups. Kies een save, maak een back-up en selecteer die voor herstel. Controller test knoppen en sticks."},
    {"ダッシュボードで本体の状態を確認できます。タブやL/Rで移動し、スティックで青いカーソルを動かしてAで選択します。",
     "右スティック上下または十字ボタンでスクロールします。画面をドラッグして一覧を移動できます。",
     "ストレージでSD容量を確認します。ファイルブラウザーを開き、フォルダーをタップします。",
     "パフォーマンスはFPSと温度を記録します。ネットワークと転送には診断と転送機能があります。",
     "ツールでゲームの日時付きセーブを作成します。セーブを選び、バックアップ後、復元前に対象を選択します。コントローラーではボタンとスティックをテストできます。"},
    {"Панель показывает состояние консоли. Используйте вкладки или L/R; двигайте синий курсор стиком и нажмите A для выбора.",
     "Прокручивайте правым стиком вверх/вниз или крестовиной. Перетаскивайте экран для просмотра списков.",
     "Хранилище показывает место на SD. Откройте файловый браузер и коснитесь папки.",
     "Производительность записывает FPS и температуру. Сеть и Передача содержат диагностику и инструменты.",
     "В Инструментах доступны датированные копии сохранений. Выберите сохранение, создайте копию и выберите ее перед восстановлением. Контроллер проверяет кнопки и стики."},
    {"仪表板概览主机状态。用标签页或 L/R 切换；用摇杆移动蓝色光标并按 A 选择。",
     "用右摇杆上下或方向键滚动页面，也可拖动屏幕浏览列表。",
     "存储页面显示 SD 卡空间。打开文件浏览器并点按文件夹查看内容。",
     "性能页面记录 FPS 和温度。网络与传输页面提供诊断和传输工具。",
     "工具页面可创建带日期的游戏存档备份。选择存档并备份，恢复前先选择备份。控制器页面可测试按键和摇杆。"},
    {"대시보드에서 본체 상태를 확인하세요. 탭 또는 L/R로 이동하고 스틱으로 파란 커서를 움직여 A로 선택합니다.",
     "오른쪽 스틱 위아래 또는 방향 버튼으로 스크롤하세요. 화면을 끌어 목록을 살펴보세요.",
     "저장 공간에서 SD 용량을 확인하세요. 파일 브라우저를 열고 폴더를 눌러 이동합니다.",
     "성능은 FPS와 온도를 기록합니다. 네트워크와 전송에서 진단 및 전송 도구를 사용할 수 있습니다.",
     "도구에서 날짜별 게임 저장 데이터를 백업할 수 있습니다. 저장 데이터를 선택해 백업하고 복원 전에 백업본을 선택하세요. 컨트롤러에서 버튼과 스틱을 테스트합니다."}
};
static const char *s_tutorial_previous[LANG_MAX] = {
    "Back", "Retour", "Indietro", "Atras", "Zuruck", "Voltar", "Terug",
    "戻る", "Назад", "上一步", "이전"};
static const char *s_tutorial_next[LANG_MAX] = {
    "Next", "Suivant", "Avanti", "Siguiente", "Weiter", "Proximo", "Volgende",
    "次へ", "Далее", "下一步", "다음"};
static const char *s_tutorial_finish[LANG_MAX] = {
    "Start exploring", "Commencer", "Inizia", "Empezar", "Loslegen", "Comecar", "Aan de slag",
    "使ってみる", "Начать", "开始使用", "시작하기"};
static const char *s_tutorial_skip[LANG_MAX] = {
    "Skip", "Passer", "Salta", "Omitir", "Ueberspringen", "Pular", "Overslaan",
    "スキップ", "Пропустить", "跳过", "건너뛰기"};
static const char *s_tutorial_settings[LANG_MAX] = {
    "View the tutorial again", "Revoir le tutoriel", "Rivedi il tutorial", "Ver el tutorial de nuevo",
    "Tutorial erneut ansehen", "Ver o tutorial novamente", "Tutorial opnieuw bekijken",
    "チュートリアルをもう一度見る", "Посмотреть обучение снова", "重新查看教程", "튜토리얼 다시 보기"};
static const char *s_tutorial_write_error[LANG_MAX] = {
    "Could not save tutorial status. Check the SD card, then try again.",
    "Impossible d'enregistrer le tutoriel. Verifiez la carte SD puis reessayez.",
    "Impossibile salvare lo stato del tutorial. Controlla la scheda SD.",
    "No se pudo guardar el tutorial. Comprueba la tarjeta SD e intentalo de nuevo.",
    "Tutorialstatus konnte nicht gespeichert werden. SD-Karte pruefen.",
    "Nao foi possivel guardar o tutorial. Verifique o cartao SD.",
    "Tutorialstatus opslaan mislukt. Controleer de SD-kaart.",
    "チュートリアルの状態を保存できません。SDカードを確認してください。",
    "Не удалось сохранить статус обучения. Проверьте SD-карту.",
    "无法保存教程状态。请检查 SD 卡后重试。",
    "튜토리얼 상태를 저장할 수 없습니다. SD 카드를 확인하세요."};
static const char *s_tesla_guide_title[LANG_MAX] = {
    "Ultrahand overlay setup guide", "Guide d'installation d'un overlay Ultrahand",
    "Guida all'installazione di un overlay Ultrahand", "Guia de instalacion de un overlay Ultrahand",
    "Anleitung zur Ultrahand-Overlay-Installation", "Guia de instalacao de um overlay Ultrahand",
    "Installatiegids voor Ultrahand-overlays", "Ultrahandオーバーレイの導入ガイド",
    "Установка оверлея Ultrahand", "Ultrahand 覆盖层安装指南", "Ultrahand 오버레이 설치 안내"};
static const char *s_tesla_guide_steps[LANG_MAX][5] = {
    {"Install Ultrahand first by following the instructions on its GitHub page.",
     "Visit SwitchInfoNX Releases. If no .ovl asset is listed, build the source with make to create overlay/SwitchInfoNX.ovl.",
     "When a compatible .ovl is published, download it. Connect the Switch by USB and enable MTP, or start an FTP server.",
     "Copy the .ovl file to sdmc:/switch/.overlays/ using your MTP or FTP client. Do not copy an archive as if it were the overlay.",
     "Safely disconnect, then open Ultrahand and select the SwitchInfoNX network overlay. If it does not appear, verify the installation and file path."},
    {"Installez d'abord Ultrahand en suivant les instructions de son depot GitHub.",
     "Ouvrez les Releases de SwitchInfoNX. Si aucun .ovl n'est publie, compilez le depot avec make pour creer overlay/SwitchInfoNX.ovl.",
     "Quand un fichier .ovl compatible sera publie, telechargez-le. Connectez la Switch en USB avec MTP, ou lancez un serveur FTP.",
     "Avec MTP ou FTP, copiez le fichier .ovl dans sdmc:/switch/.overlays/. Ne copiez pas une archive a la place de l'overlay.",
     "Deconnectez proprement, ouvrez Ultrahand et selectionnez l'overlay reseau SwitchInfoNX. S'il n'apparait pas, verifiez l'installation et le chemin."},
    {"Installa prima Ultrahand seguendo le istruzioni nella sua pagina GitHub.",
     "Apri Releases di SwitchInfoNX. Se non trovi un .ovl, compila il repository con make per creare overlay/SwitchInfoNX.ovl.",
     "Quando sara pubblicato un file .ovl compatibile, scaricalo. Collega la Switch via USB e attiva MTP, oppure avvia un server FTP.",
     "Con un client MTP o FTP copia il file .ovl in sdmc:/switch/.overlays/. Non copiare un archivio al posto dell'overlay.",
     "Disconnetti in sicurezza, apri Ultrahand e seleziona l'overlay di rete SwitchInfoNX. Se non appare, controlla installazione e percorso."},
    {"Instala primero Ultrahand siguiendo las instrucciones de su pagina de GitHub.",
     "Abre Releases de SwitchInfoNX. Si no hay un .ovl publicado, compila el repositorio con make para crear overlay/SwitchInfoNX.ovl.",
     "Cuando se publique un .ovl compatible, descargalo. Conecta la Switch por USB y activa MTP, o inicia un servidor FTP.",
     "Con un cliente MTP o FTP, copia el .ovl en sdmc:/switch/.overlays/. No uses un archivo comprimido como overlay.",
     "Desconecta de forma segura, abre Ultrahand y selecciona el overlay de red de SwitchInfoNX. Si no aparece, revisa la instalacion y la ruta."},
    {"Installiere zuerst Ultrahand gemaess den Anweisungen auf der GitHub-Seite.",
     "Oeffne SwitchInfoNX-Releases. Falls kein .ovl bereitsteht, erstelle es mit make aus dem Quellcode: overlay/SwitchInfoNX.ovl.",
     "Lade eine kompatible .ovl-Datei herunter, sobald sie veroeffentlicht ist. Verbinde USB mit MTP oder starte einen FTP-Server.",
     "Kopiere die .ovl-Datei per MTP oder FTP nach sdmc:/switch/.overlays/. Ein Archiv ist kein installierter Overlay.",
     "Trenne sicher, oeffne Ultrahand und waehle das SwitchInfoNX-Netzwerk-Overlay. Pruefe bei Fehlern Installation und Pfad."},
    {"Instale primeiro o Ultrahand seguindo as instrucoes na respetiva pagina do GitHub.",
     "Abre Releases do SwitchInfoNX. Se nao houver .ovl publicado, compila o codigo com make para criar overlay/SwitchInfoNX.ovl.",
     "Quando houver um .ovl compativel, transfere-o. Liga a Switch por USB com MTP ou inicia um servidor FTP.",
     "Com MTP ou FTP, copia o .ovl para sdmc:/switch/.overlays/. Nao copies um arquivo comprimido como overlay.",
     "Desliga em seguranca, abre o Ultrahand e seleciona o overlay de rede do SwitchInfoNX. Se nao aparecer, verifica a instalacao e o caminho."},
    {"Installeer eerst Ultrahand volgens de instructies op de GitHub-pagina.",
     "Open SwitchInfoNX-Releases. Als er geen .ovl staat, bouw de broncode met make om overlay/SwitchInfoNX.ovl te maken.",
     "Download een compatibel .ovl-bestand zodra het beschikbaar is. Verbind via USB met MTP of start een FTP-server.",
     "Kopieer het .ovl-bestand met MTP of FTP naar sdmc:/switch/.overlays/. Een archiefbestand is niet de overlay zelf.",
     "Koppel veilig los, open Ultrahand en selecteer de SwitchInfoNX-netwerkoverlay. Controleer bij problemen de installatie en het pad."},
    {"まずGitHubの手順に従ってUltrahandをインストールしてください。",
     "SwitchInfoNXのReleasesを開きます。.ovlがない場合はソースをmakeでビルドするとoverlay/SwitchInfoNX.ovlが作成されます。",
     "互換性のある.ovlが公開されたらダウンロードします。USBで接続してMTPを有効にするか、FTPサーバーを起動します。",
     "MTPまたはFTPで.ovlファイルをsdmc:/switch/.overlays/へコピーします。圧縮ファイルのまま配置しないでください。",
     "安全に取り外し、Ultrahandを開いてSwitchInfoNXネットワークオーバーレイを選択します。表示されない場合は導入方法とパスを確認してください。"},
    {"Сначала установите Ultrahand по инструкции на его странице GitHub.",
     "Откройте Releases SwitchInfoNX. Если .ovl не опубликован, соберите исходники командой make: overlay/SwitchInfoNX.ovl.",
     "Когда появится совместимый .ovl, скачайте его. Подключите Switch по USB с MTP или запустите FTP-сервер.",
     "Через MTP или FTP скопируйте .ovl в sdmc:/switch/.overlays/. Не оставляйте overlay внутри архива.",
     "Безопасно отключите устройство, откройте Ultrahand и выберите сетевой оверлей SwitchInfoNX. Если его нет, проверьте установку и путь."},
    {"请先按照 GitHub 页面上的说明安装 Ultrahand。",
     "打开 SwitchInfoNX Releases。若没有 .ovl，请在源码目录运行 make，生成 overlay/SwitchInfoNX.ovl。",
     "发布兼容的 .ovl 后再下载。通过 USB 启用 MTP，或在主机上启动 FTP 服务器。",
     "使用 MTP 或 FTP 将 .ovl 文件复制到 sdmc:/switch/.overlays/。不要把压缩包当作 overlay。",
     "安全断开后，打开 Ultrahand 并选择 SwitchInfoNX 网络覆盖层。若未显示，请检查安装和文件路径。"},
    {"먼저 GitHub 페이지의 안내에 따라 Ultrahand를 설치하세요.",
     "SwitchInfoNX Releases를 여세요. .ovl이 없으면 소스에서 make를 실행해 overlay/SwitchInfoNX.ovl을 생성하세요.",
     "호환되는 .ovl이 공개되면 다운로드하세요. USB MTP를 활성화하거나 FTP 서버를 실행하세요.",
     "MTP 또는 FTP로 .ovl 파일을 sdmc:/switch/.overlays/에 복사하세요. 압축 파일을 그대로 넣지 마세요.",
     "안전하게 연결을 해제하고 Ultrahand를 열어 SwitchInfoNX 네트워크 오버레이를 선택하세요. 표시되지 않으면 설치와 경로를 확인하세요."}
};
static const char *s_tesla_guide_button[LANG_MAX] = {
    "Ultrahand setup guide", "Guide Ultrahand", "Guida Ultrahand", "Guia Ultrahand", "Ultrahand-Anleitung",
    "Guia Ultrahand", "Ultrahand-handleiding", "Ultrahand導入ガイド", "Гид Ultrahand", "Ultrahand 安装指南", "Ultrahand 안내"};
static const char *s_tesla_guide_close[LANG_MAX] = {
    "Close", "Fermer", "Chiudi", "Cerrar", "Schliessen", "Fechar", "Sluiten",
    "閉じる", "Закрыть", "关闭", "닫기"};
static const char *s_soc_temp[LANG_MAX] = {
    "SoC Temp", "Temp. SoC", "Temp. SoC", "Temp. SoC",
    "SoC-Temp.", "Temp. SoC", "SoC-temp.",
    "SoC温度",
    "Температура SoC",
    "SoC 温度",
    "SoC 온도"};
static const char *s_thermal_state[LANG_MAX] = {
    "Thermal State", "Etat thermique", "Stato termico", "Estado termico",
    "Thermischer Status", "Estado termico", "Thermische status",
    "温度状態",
    "Тепловое состояние",
    "温度状态",
    "온도 상태"};
static const char *s_normal[LANG_MAX] = {
    "Normal", "Normal", "Normale", "Normal",
    "Normal", "Normal", "Normaal",
    "正常",
    "Норма",
    "正常",
    "정상"};
static const char *s_hot[LANG_MAX] = {
    "HOT!", "CHAUDE!", "CALDO!", "CALIENTE!",
    "HEISS!", "QUENTE!", "HEET!",
    "高温！",
    "ЖАРКО!",
    "过热！",
    "고온!"};
static const char *s_warm[LANG_MAX] = {
    "Warm", "Tiede", "Caldo", "Templado",
    "Warm", "Morno", "Warm",
    "温かい",
    "Тепло",
    "偏热",
    "따뜻함"};
static const char *s_brightness[LANG_MAX] = {
    "Brightness", "Luminosite", "Luminosita", "Brillo",
    "Helligkeit", "Brilho", "Helderheid",
    "Brightness",
    "Brightness",
    "Brightness",
    "Brightness"};
static const char *s_na_emu[LANG_MAX] = {
    "N/A (emulator)", "N/D (emulateur)", "N/D (emulatore)", "N/D (emulador)",
    "N/V (Emulator)", "N/D (emulador)", "N/B (emulator)",
    "エミュレーターでは利用不可",
    "Недоступно в эмуляторе",
    "模拟器中不可用",
    "에뮬레이터에서 사용 불가"};
static const char *s_resolution[LANG_MAX] = {
    "Resolution", "Resolution", "Risoluzione", "Resolucion",
    "Aufloesung", "Resolucao", "Resolutie",
    "解像度",
    "Разрешение",
    "分辨率",
    "해상도"};
static const char *s_res_val[LANG_MAX] = {
    "1280x720 (720p)", "1280x720 (720p)", "1280x720 (720p)", "1280x720 (720p)",
    "1280x720 (720p)", "1280x720 (720p)", "1280x720 (720p)",
    "1280x720 (720p)",
    "1280x720 (720p)",
    "1280x720 (720p)",
    "1280x720 (720p)"};
static const char *s_refresh_rate[LANG_MAX] = {
    "Refresh Rate", "Taux de rafraichissement", "Frequenza aggiornamento", "Frecuencia actualizacion",
    "Bildwiederholrate", "Taxa de atualizacao", "Verversingssnelheid",
    "リフレッシュレート",
    "Частота обновления",
    "刷新率",
    "화면 주사율"};
static const char *s_sys_uptime[LANG_MAX] = {
    "System Uptime", "Temps de fonctionnement", "Tempo di accensione", "Tiempo de actividad",
    "System-Betriebszeit", "Tempo de atividade", "Systeem-uptime",
    "システム稼働時間",
    "Время работы системы",
    "系统运行时间",
    "시스템 작동 시간"};
static const char *s_app_uptime[LANG_MAX] = {
    "App Uptime", "Temps d'ouverture", "Tempo apertura app", "Tiempo de ejecucion",
    "App-Laufzeit", "Tempo de execucao", "App-uptime",
    "アプリ稼働時間",
    "Время работы приложения",
    "应用运行时间",
    "앱 실행 시간"};
static const char *s_failed_read_clocks[LANG_MAX] = {
    "Failed to read system clocks.", "Echec de lecture des horloges.", "Lettura orologi fallita.", "Fallo al leer relojes.",
    "Fehler beim Lesen der Takte.", "Falha ao ler clocks.", "Kan systeemklokken niet lezen.",
    "Failed to read system clocks.",
    "Failed to read system clocks.",
    "Failed to read system clocks.",
    "Failed to read system clocks."};
static const char *s_country_names[LANG_MAX][6] = {
    {"Japan","Americas","Europe","Australia/NZ","HK/TW/KR","China"},
    {"Japon","Ameriques","Europe","Australie/NZ","HK/TW/KR","Chine"},
    {"Giappone","Americhe","Europa","Australia/NZ","HK/TW/KR","Cina"},
    {"Japon","Americas","Europa","Australia/NZ","HK/TW/KR","China"},
    {"Japan","Amerika","Europa","Australien/NZ","HK/TW/KR","China"},
    {"Japao","Americas","Europa","Australia/NZ","HK/TW/KR","China"},
    {"Japan","Amerika","Europa","Australie/NZ","HK/TW/KR","China"},
    {"Japan","Americas","Europe","Australia/NZ","HK/TW/KR","China"},
    {"Japan","Americas","Europe","Australia/NZ","HK/TW/KR","China"},
    {"Japan","Americas","Europe","Australia/NZ","HK/TW/KR","China"},
    {"Japan","Americas","Europe","Australia/NZ","HK/TW/KR","China"}};
static const char *s_lang_names_18[LANG_MAX][18] = {
    {"Japanese","English US","French","German","Italian","Spanish","Chinese","Korean","Dutch","Portuguese","Russian","Chinese TW","English UK","French CA","Spanish LA","Chinese Hans","Chinese Hant","Brazilian PT"},
    {"Japonais","Anglais US","Francais","Allemand","Italien","Espagnol","Chinois","Coreen","Neerlandais","Portugais","Russe","Chinois TW","Anglais UK","Francais CA","Espagnol LA","Chinois Hans","Chinois Hant","Portugais BR"},
    {"Giapponese","Inglese US","Francese","Tedesco","Italiano","Spagnolo","Cinese","Coreano","Olandese","Portoghese","Russo","Cinese TW","Inglese UK","Francese CA","Spagnolo LA","Cinese Hans","Cinese Hant","Portoghese BR"},
    {"Japones","Ingles EU","Frances","Aleman","Italiano","Espanol","Chino","Coreano","Neerlandes","Portugues","Ruso","Chino TW","Ingles RU","Frances CA","Espanol LA","Chino Hans","Chino Hant","Portugues BR"},
    {"Japanisch","Englisch US","Franzoesisch","Deutsch","Italienisch","Spanisch","Chinesisch","Koreanisch","Niederlaendisch","Portugiesisch","Russisch","Chinesisch TW","Englisch UK","Franzoesisch CA","Spanisch LA","Chinesisch Hans","Chinesisch Hant","Portugiesisch BR"},
    {"Japones","Ingles EU","Frances","Alemao","Italiano","Espanhol","Chines","Coreano","Holandes","Portugues","Russo","Chines TW","Ingles UK","Frances CA","Espanhol LA","Chines Hans","Chines Hant","Portugues BR"},
    {"Japans","Engels US","Frans","Duits","Italiaans","Spaans","Chinees","Koreaans","Nederlands","Portugees","Russisch","Chinees TW","Engels UK","Frans CA","Spaans LA","Chinees Hans","Chinees Hant","Portugees BR"},
    {"Japanese","English US","French","German","Italian","Spanish","Chinese","Korean","Dutch","Portuguese","Russian","Chinese TW","English UK","French CA","Spanish LA","Chinese Hans","Chinese Hant","Brazilian PT"},
    {"Japanese","English US","French","German","Italian","Spanish","Chinese","Korean","Dutch","Portuguese","Russian","Chinese TW","English UK","French CA","Spanish LA","Chinese Hans","Chinese Hant","Brazilian PT"},
    {"Japanese","English US","French","German","Italian","Spanish","Chinese","Korean","Dutch","Portuguese","Russian","Chinese TW","English UK","French CA","Spanish LA","Chinese Hans","Chinese Hant","Brazilian PT"},
    {"Japanese","English US","French","German","Italian","Spanish","Chinese","Korean","Dutch","Portuguese","Russian","Chinese TW","English UK","French CA","Spanish LA","Chinese Hans","Chinese Hant","Brazilian PT"}};

// --- Page 1: Storage ---
static const char *s_sd_card[LANG_MAX] = {
    "SD Card (sdmc:/)", "Carte SD (sdmc:/)", "SD Card (sdmc:/)", "Tarjeta SD (sdmc:/)",
    "SD-Karte (sdmc:/)", "Cartao SD (sdmc:/)", "SD-kaart (sdmc:/)",
    "SD Card (sdmc:/)",
    "SD Card (sdmc:/)",
    "SD Card (sdmc:/)",
    "SD Card (sdmc:/)"};
static const char *s_total[LANG_MAX] = {
    "Total", "Total", "Totale", "Total",
    "Gesamt", "Total", "Totaal",
    "Total",
    "Total",
    "Total",
    "Total"};
static const char *s_used[LANG_MAX] = {
    "Used", "Utilise", "Usato", "Usado",
    "Belegt", "Usado", "Gebruikt",
    "Used",
    "Used",
    "Used",
    "Used"};
static const char *s_free[LANG_MAX] = {
    "Free", "Libre", "Libero", "Libre",
    "Frei", "Livre", "Vrij",
    "Free",
    "Free",
    "Free",
    "Free"};
static const char *s_storage_usage[LANG_MAX] = {
    "Storage Usage", "Utilisation stockage", "Utilizzo archivio", "Uso de almacenamiento",
    "Speichernutzung", "Uso do armazenamento", "Opslaggebruik",
    "Storage Usage",
    "Storage Usage",
    "Storage Usage",
    "Storage Usage"};
static const char *s_root_fmt[LANG_MAX] = {
    "Root: %d folders, %d files", "Racine: %d dossiers, %d fichiers", "Radice: %d cartelle, %d file", "Raiz: %d carpetas, %d archivos",
    "Root: %d Ordner, %d Dateien", "Raiz: %d pastas, %d arquivos", "Root: %d mappen, %d bestanden",
    "Root: %d folders, %d files",
    "Root: %d folders, %d files",
    "Root: %d folders, %d files",
    "Root: %d folders, %d files"};
static const char *s_sd_fail[LANG_MAX] = {
    "Failed to read SD card info", "Echec de lecture de la carte SD", "Lettura SD fallita", "Fallo al leer tarjeta SD",
    "SD-Karteninfo konnte nicht gelesen werden", "Falha ao ler cartao SD", "Kan SD-kaartinfo niet lezen",
    "Failed to read SD card info",
    "Failed to read SD card info",
    "Failed to read SD card info",
    "Failed to read SD card info"};
static const char *s_read_speed_fmt[LANG_MAX] = {
    "Cached readback: %.1f MB/s", "Lecture cachee: %.1f Mo/s", "Lettura cache: %.1f MB/s", "Lectura en cache: %.1f MB/s",
    "Gelesener Cache: %.1f MB/s", "Leitura em cache: %.1f MB/s", "Cache-lezen: %.1f MB/s",
    "Cached readback: %.1f MB/s", "Cached readback: %.1f MB/s", "Cached readback: %.1f MB/s", "Cached readback: %.1f MB/s"};
static const char *s_read_testing[LANG_MAX] = {
    "Cached read test: Testing...", "Test de lecture cachee...", "Test lettura cache...", "Prueba de lectura en cache...",
    "Cache-Lesetest...", "Teste de leitura em cache...", "Cache-leestest...",
    "Cached read test: Testing...", "Cached read test: Testing...", "Cached read test: Testing...", "Cached read test: Testing..."};
static const char *s_read_error[LANG_MAX] = {
    "Read Speed: Error", "Vitesse: Erreur", "Velocita: Errore", "Velocidad: Error",
    "Lesegeschw.: Fehler", "Velocidade: Erro", "Leessnelheid: Fout",
    "Read Speed: Error",
    "Read Speed: Error",
    "Read Speed: Error",
    "Read Speed: Error"};
static const char *s_read_untested[LANG_MAX] = {
    "Read Speed: Not tested", "Vitesse: Non testee", "Velocita: Non testata", "Velocidad: No probada",
    "Lesegeschw.: Nicht getestet", "Velocidade: Nao testado", "Leessnelheid: Niet getest",
    "Read Speed: Not tested",
    "Read Speed: Not tested",
    "Read Speed: Not tested",
    "Read Speed: Not tested"};
static const char *s_sd_no_mount[LANG_MAX] = {
    "SD Card not inserted or failed to mount", "Carte SD non inseree ou echec montage", "SD non inserita o mount fallito", "Tarjeta SD no insertada o fallo al montar",
    "SD-Karte nicht eingelegt oder Mount fehlgeschlagen", "Cartao SD nao inserido ou falha ao montar", "SD-kaart niet geplaatst of mount mislukt",
    "SD Card not inserted or failed to mount",
    "SD Card not inserted or failed to mount",
    "SD Card not inserted or failed to mount",
    "SD Card not inserted or failed to mount"};
static const char *s_nand_parts[LANG_MAX] = {
    "NAND Partitions (Internal)", "Partitions NAND (Interne)", "Partizioni NAND (Interne)", "Particiones NAND (Interna)",
    "NAND-Partitionen (Intern)", "Particoes NAND (Interno)", "NAND-partities (Intern)",
    "NAND Partitions (Internal)",
    "NAND Partitions (Internal)",
    "NAND Partitions (Internal)",
    "NAND Partitions (Internal)"};
static const char *s_sys_part[LANG_MAX] = {
    "System Partition", "Partition Systeme", "Partizione di Sistema", "Particion del Sistema",
    "Systempartition", "Particao do Sistema", "Systeempartitie",
    "System Partition",
    "System Partition",
    "System Partition",
    "System Partition"};
static const char *s_user_part[LANG_MAX] = {
    "User Partition", "Partition Utilisateur", "Partizione Utente", "Particion de Usuario",
    "Benutzerpartition", "Particao do Usuario", "Gebruikerspartitie",
    "User Partition",
    "User Partition",
    "User Partition",
    "User Partition"};
static const char *s_sd_breakdown[LANG_MAX] = {
    "SD Card Content Breakdown", "Contenu de la carte SD", "Contenuto SD", "Contenido de la tarjeta SD",
    "SD-Karteninhalt", "Conteudo do cartao SD", "SD-kaartinhoud",
    "SD Card Content Breakdown",
    "SD Card Content Breakdown",
    "SD Card Content Breakdown",
    "SD Card Content Breakdown"};
static const char *s_sd_scan_button[LANG_MAX] = {
    "ANALYZE SD", "ANALYSER LA SD", "ANALIZZA SD", "ANALIZAR SD",
    "SD ANALYSIEREN", "ANALISAR SD", "SD ANALYSEREN",
    "SDを分析", "АНАЛИЗ SD", "分析 SD 卡", "SD 분석"};
static const char *s_sd_scan_prompt[LANG_MAX] = {
    "Tap Analyze SD to calculate folder sizes. Results stay cached until the next scan.",
    "Touchez Analyser pour calculer les tailles. Resultats gardes en cache jusqu'au prochain scan.",
    "Tocca Analizza per calcolare le dimensioni. Risultati in cache fino alla prossima scansione.",
    "Toca Analizar para calcular tamanos. Resultados guardados hasta el siguiente analisis.",
    "Tippe Analysieren fuer Ordnergroessen. Ergebnisse bleiben bis zum naechsten Scan gespeichert.",
    "Toque Analisar para calcular tamanhos. Resultados em cache ate a proxima analise.",
    "Tik op Analyseren voor mapgroottes. Resultaten blijven bewaard tot de volgende scan.",
    "分析をタップしてフォルダー容量を計算します。次のスキャンまで結果を保存します。",
    "Нажмите «Анализ», чтобы вычислить размеры. Результаты сохраняются до нового сканирования.",
    "点击“分析”计算文件夹大小。结果会保留到下次扫描。",
    "분석을 눌러 폴더 크기를 계산합니다. 결과는 다음 검사까지 저장됩니다."};
static const char *s_sd_scan_running[LANG_MAX] = {
    "Scanning SD card... %llu files, %llu folders",
    "Analyse SD... %llu fichiers, %llu dossiers",
    "Scansione SD... %llu file, %llu cartelle",
    "Analizando SD... %llu archivos, %llu carpetas",
    "SD-Scan... %llu Dateien, %llu Ordner",
    "A analisar SD... %llu ficheiros, %llu pastas",
    "SD scannen... %llu bestanden, %llu mappen",
    "SDをスキャン中... ファイル %llu 件、フォルダー %llu 件",
    "Сканирование SD... файлов: %llu, папок: %llu",
    "正在扫描 SD 卡... 文件 %llu 个，文件夹 %llu 个",
    "SD 검사 중... 파일 %llu개, 폴더 %llu개"};
static const char *s_sd_scan_done[LANG_MAX] = {
    "Scan complete: %llu files, %llu folders%s",
    "Analyse terminee : %llu fichiers, %llu dossiers%s",
    "Scansione completata: %llu file, %llu cartelle%s",
    "Analisis completa: %llu archivos, %llu carpetas%s",
    "Scan abgeschlossen: %llu Dateien, %llu Ordner%s",
    "Analise concluida: %llu ficheiros, %llu pastas%s",
    "Scan voltooid: %llu bestanden, %llu mappen%s",
    "スキャン完了：ファイル %llu 件、フォルダー %llu 件%s",
    "Сканирование завершено: файлов %llu, папок %llu%s",
    "扫描完成：文件 %llu 个，文件夹 %llu 个%s",
    "검사 완료: 파일 %llu개, 폴더 %llu개%s"};
static const char *s_sd_scan_failed[LANG_MAX] = {
    "SD scan failed. Check the card and try again.",
    "Echec du scan SD. Verifiez la carte et reessayez.",
    "Scansione SD non riuscita. Controlla la scheda.",
    "Fallo al analizar SD. Comprueba la tarjeta.",
    "SD-Scan fehlgeschlagen. Karte pruefen und erneut versuchen.",
    "Falha na analise SD. Verifique o cartao.",
    "SD-scan mislukt. Controleer de kaart.",
    "SDスキャン失敗。カードを確認して再試行してください。",
    "Ошибка сканирования SD. Проверьте карту.",
    "SD 卡扫描失败。请检查存储卡后重试。",
    "SD 검사 실패. 카드를 확인하고 다시 시도하세요."};
static const char *s_sd_scan_categories[LANG_MAX][5] = {
    {"Games / Nintendo", "Homebrew / apps", "CFW / config", "Media", "Other"},
    {"Jeux / Nintendo", "Homebrews / applis", "CFW / configuration", "Medias", "Autres"},
    {"Giochi / Nintendo", "Homebrew / app", "CFW / config", "Media", "Altro"},
    {"Juegos / Nintendo", "Homebrew / apps", "CFW / config", "Multimedia", "Otros"},
    {"Spiele / Nintendo", "Homebrew / Apps", "CFW / Konfig.", "Medien", "Andere"},
    {"Jogos / Nintendo", "Homebrew / apps", "CFW / config", "Media", "Outros"},
    {"Games / Nintendo", "Homebrew / apps", "CFW / config", "Media", "Overig"},
    {"ゲーム / Nintendo", "自作アプリ", "CFW / 設定", "メディア", "その他"},
    {"Игры / Nintendo", "Homebrew / приложения", "CFW / настройки", "Медиа", "Другое"},
    {"游戏 / Nintendo", "自制软件", "CFW / 配置", "媒体", "其他"},
    {"게임 / Nintendo", "홈브류 / 앱", "CFW / 설정", "미디어", "기타"}};
static const char *s_sd_scan_details[LANG_MAX] = {
    "Largest entries (tap a folder to open):",
    "Plus gros elements (touchez un dossier pour l'ouvrir) :",
    "Elementi piu grandi (tocca una cartella per aprirla):",
    "Elementos mayores (toca una carpeta para abrirla):",
    "Groesste Eintraege (Ordner antippen zum Oeffnen):",
    "Maiores itens (toque numa pasta para abrir):",
    "Grootste items (tik op een map om te openen):",
    "大きい項目（フォルダーをタップして開く）：",
    "Крупнейшие элементы (нажмите папку, чтобы открыть):",
    "占用空间最大的项目（点击文件夹打开）：",
    "큰 항목 (폴더를 눌러 열기):"};
static const char *s_sd_scan_total[LANG_MAX] = {
    "Scanned files", "Fichiers analyses", "File analizzati", "Archivos analizados",
    "Analysierte Dateien", "Ficheiros analisados", "Gescande bestanden",
    "スキャン済みファイル", "Размер просмотренных файлов", "已扫描文件", "검사한 파일"};
static const char *s_touch_test[LANG_MAX] = {
    "[Touch] Test Speed", "[Touch] Test Vitesse", "[Touch] Test Velocita", "[Touch] Probar Velocidad",
    "[Touch] Geschw. testen", "[Touch] Testar Velocidade", "[Touch] Snelheid testen",
    "[Touch] Test Speed",
    "[Touch] Test Speed",
    "[Touch] Test Speed",
    "[Touch] Test Speed"};
static const char *s_open_fb[LANG_MAX] = {
    "[Y] Open File Browser", "[Y] Explorateur", "[Y] Esplora file", "[Y] Explorar archivos",
    "[Y] Dateibrowser", "[Y] Explorar arquivos", "[Y] Bestandsverkenner",
    "[Y] Open File Browser",
    "[Y] Open File Browser",
    "[Y] Open File Browser",
    "[Y] Open File Browser"};
static const char *s_empty_dir[LANG_MAX] = {
    "(empty directory)", "(dossier vide)", "(cartella vuota)", "(directorio vacio)",
    "(leeres Verzeichnis)", "(diretorio vazio)", "(lege map)",
    "(empty directory)",
    "(empty directory)",
    "(empty directory)",
    "(empty directory)"};
// --- Page 2: Network ---
static const char *s_ip_config[LANG_MAX] = {
    "IP Configuration", "Configuration IP", "Configurazione IP", "Configuracion IP",
    "IP-Konfiguration", "Configuracao IP", "IP-configuratie",
    "IP設定",
    "Настройки IP",
    "IP 配置",
    "IP 구성"};
static const char *s_ip_addr[LANG_MAX] = {
    "IP Address", "Adresse IP", "Indirizzo IP", "Direccion IP",
    "IP-Adresse", "Endereco IP", "IP-adres",
    "IPアドレス",
    "IP-адрес",
    "IP 地址",
    "IP 주소"};
static const char *s_subnet[LANG_MAX] = {
    "Subnet Mask", "Masque sous-reseau", "Maschera di rete", "Mascara de subred",
    "Subnetzmaske", "Mascara de rede", "Subnetmasker",
    "サブネットマスク",
    "Маска подсети",
    "子网掩码",
    "서브넷 마스크"};
static const char *s_gateway[LANG_MAX] = {
    "Gateway", "Passerelle", "Gateway", "Puerta de enlace",
    "Gateway", "Gateway", "Gateway",
    "ゲートウェイ",
    "Шлюз",
    "网关",
    "게이트웨이"};
static const char *s_primary_dns[LANG_MAX] = {
    "Primary DNS", "DNS Primaire", "DNS Primario", "DNS Primario",
    "Primaer-DNS", "DNS Primario", "Primaire DNS",
    "優先DNS",
    "Основной DNS",
    "首选 DNS",
    "기본 DNS"};
static const char *s_secondary_dns[LANG_MAX] = {
    "Secondary DNS", "DNS Secondaire", "DNS Secondario", "DNS Secundario",
    "Sekundaer-DNS", "DNS Secundario", "Secundaire DNS",
    "代替DNS",
    "Дополнительный DNS",
    "备用 DNS",
    "보조 DNS"};
static const char *s_hostname[LANG_MAX] = {
    "Hostname", "Nom d'hote", "Hostname", "Nombre de host",
    "Hostname", "Hostname", "Hostnaam",
    "ホスト名",
    "Имя узла",
    "主机名",
    "호스트 이름"};
static const char *s_mac_addr[LANG_MAX] = {
    "MAC Address", "Adresse MAC", "Indirizzo MAC", "Direccion MAC",
    "MAC-Adresse", "Endereco MAC", "MAC-adres",
    "MACアドレス",
    "MAC-адрес",
    "MAC 地址",
    "MAC 주소"};
static const char *s_adapter[LANG_MAX] = {
    "Adapter", "Adaptateur", "Adattatore", "Adaptador",
    "Adapter", "Adaptador", "Adapter",
    "アダプター",
    "Адаптер",
    "适配器",
    "어댑터"};
static const char *s_conn_details[LANG_MAX] = {
    "Connection Details", "Details de connexion", "Dettagli connessione", "Detalles de conexion",
    "Verbindungsdetails", "Detalhes da conexao", "Verbindingsdetails",
    "接続情報",
    "Сведения о подключении",
    "连接详情",
    "연결 정보"};
static const char *s_interface[LANG_MAX] = {
    "Interface", "Interface", "Interfaccia", "Interfaz",
    "Schnittstelle", "Interface", "Interface",
    "インターフェース",
    "Интерфейс",
    "网络接口",
    "인터페이스"};
static const char *s_wifi_wireless[LANG_MAX] = {
    "Wi-Fi (Wireless)", "Wi-Fi (Sans fil)", "Wi-Fi (Wireless)", "Wi-Fi (Inalambrico)",
    "WLAN (Kabellos)", "Wi-Fi (Sem fio)", "Wi-Fi (Draadloos)",
    "Wi-Fi（無線）",
    "Wi-Fi (беспроводная)",
    "Wi-Fi（无线）",
    "Wi-Fi (무선)"};
static const char *s_ethernet_wired[LANG_MAX] = {
    "Ethernet (Wired)", "Ethernet (Filaire)", "Ethernet (Cavo)", "Ethernet (Cableado)",
    "Ethernet (Kabel)", "Ethernet (Cabo)", "Ethernet (Bekabeld)",
    "Ethernet（有線）",
    "Ethernet (проводная)",
    "以太网（有线）",
    "이더넷 (유선)"};
static const char *s_internet[LANG_MAX] = {
    "Internet", "Internet", "Internet", "Internet",
    "Internet", "Internet", "Internet",
    "Internet",
    "Internet",
    "Internet",
    "Internet"};
static const char *s_connected[LANG_MAX] = {
    "Connected", "Connecte", "Connesso", "Conectado",
    "Verbunden", "Conectado", "Verbonden",
    "接続中",
    "Подключено",
    "已连接",
    "연결됨"};
static const char *s_limited[LANG_MAX] = {
    "Limited", "Limite", "Limitato", "Limitado",
    "Eingeschraenkt", "Limitado", "Beperkt",
    "制限あり",
    "Ограничено",
    "受限",
    "제한됨"};
static const char *s_signal_quality[LANG_MAX] = {
    "Signal Quality", "Qualite du signal", "Qualita segnale", "Calidad de senhal",
    "Signalqualitaet", "Qualidade do sinal", "Signaalkwaliteit",
    "電波強度",
    "Уровень сигнала",
    "信号强度",
    "신호 강도"};
static const char *s_link[LANG_MAX] = {
    "Link", "Lien", "Collegamento", "Enlace",
    "Verbindung", "Link", "Verbinding",
    "リンク",
    "Связь",
    "链路",
    "링크"};
static const char *s_wired_stable[LANG_MAX] = {
    "Wired (Stable)", "Filaire (Stable)", "Cavo (Stabile)", "Cableado (Estable)",
    "Kabel (Stabil)", "Cabo (Estavel)", "Bekabeld (Stabiel)",
    "有線（安定）",
    "Проводное (стабильно)",
    "有线（稳定）",
    "유선 (안정)"};
static const char *s_connected_for[LANG_MAX] = {
    "Connected For", "Connecte depuis", "Connesso da", "Conectado desde",
    "Verbunden seit", "Conectado ha", "Verbonden sinds",
    "接続時間",
    "Подключено",
    "连接时长",
    "연결 시간"};
static const char *s_band[LANG_MAX] = {
    "Band", "Bande", "Banda", "Banda",
    "Band", "Banda", "Band",
    "周波数帯",
    "Диапазон",
    "频段",
    "주파수 대역"};
static const char *s_status[LANG_MAX] = {
    "Status", "Statut", "Stato", "Estado",
    "Status", "Estado", "Status",
    "状態",
    "Статус",
    "状态",
    "상태"};
static const char *s_full_internet[LANG_MAX] = {
    "Full Internet Access", "Acces Internet complet", "Accesso Internet completo", "Acceso completo a Internet",
    "Voller Internetzugriff", "Acesso total a Internet", "Volledige internettoegang",
    "インターネット接続あり",
    "Полный доступ к Интернету",
    "可访问互联网",
    "인터넷 접속 가능"};
static const char *s_local_only[LANG_MAX] = {
    "Local Network Only", "Reseau local seulement", "Solo rete locale", "Solo red local",
    "Nur lokales Netzwerk", "Apenas rede local", "Alleen lokaal netwerk",
    "ローカルネットワークのみ",
    "Только локальная сеть",
    "仅限本地网络",
    "로컬 네트워크만"};
static const char *s_no_conn[LANG_MAX] = {
    "No Connection", "Pas de connexion", "Nessuna connessione", "Sin conexion",
    "Keine Verbindung", "Sem conexao", "Geen verbinding",
    "未接続",
    "Нет подключения",
    "未连接",
    "연결 안 됨"};
static const char *s_wifi_diag[LANG_MAX] = {
    "Wi-Fi Signal Diagnostics", "Diagnostic signal Wi-Fi", "Diagnostica segnale Wi-Fi", "Diagnostico de senhal Wi-Fi",
    "WLAN-Signal-Diagnose", "Diagnostico de sinal Wi-Fi", "Wi-Fi-signaal diagnostiek",
    "Wi-Fi 電波診断",
    "Диагностика Wi-Fi",
    "Wi-Fi 信号诊断",
    "Wi-Fi 신호 진단"};
static const char *s_ssid[LANG_MAX] = {
    "SSID", "SSID", "SSID", "SSID",
    "SSID", "SSID", "SSID",
    "SSID",
    "SSID",
    "SSID",
    "SSID"};
static const char *s_connected_label[LANG_MAX] = {
    "(connected)", "(connecte)", "(connesso)", "(conectado)",
    "(verbunden)", "(conectado)", "(verbonden)",
    "接続中",
    "подключено",
    "已连接",
    "연결됨"};
static const char *s_rssi[LANG_MAX] = {
    "RSSI", "RSSI", "RSSI", "RSSI",
    "RSSI", "RSSI", "RSSI",
    "受信信号強度",
    "Уровень RSSI",
    "信号强度",
    "RSSI"};
static const char *s_link_quality[LANG_MAX] = {
    "Link Quality", "Qualite du lien", "Qualita collegamento", "Calidad de enlace",
    "Verbindungsqualitaet", "Qualidade do link", "Linkkwaliteit",
    "リンク品質",
    "Качество связи",
    "链路质量",
    "링크 품질"};
static const char *s_signal_power[LANG_MAX] = {
    "Signal Power", "Puissance du signal", "Potenza segnale", "Potencia de senhal",
    "Signalstaerke", "Potencia do sinal", "Signaalsterkte",
    "信号強度",
    "Мощность сигнала",
    "信号功率",
    "신호 세기"};
static const char *s_signal_history[LANG_MAX] = {
    "Signal History (60s):", "Historique signal (60s):", "Cronologia segnale (60s):", "Historial de senhal (60s):",
    "Signalverlauf (60s):", "Historico de sinal (60s):", "Signaalgeschiedenis (60s):",
    "信号履歴（60秒）：",
    "История сигнала (60 с):",
    "信号历史（60秒）：",
    "신호 기록 (60초):"};
static const char *s_traffic_title[LANG_MAX] = {
    "Live Network Traffic (all active interfaces)", "Trafic reseau en direct (interfaces actives)",
    "Traffico di rete in tempo reale (interfacce attive)", "Trafico de red en tiempo real (interfaces activas)",
    "Netzwerkverkehr live (aktive Schnittstellen)", "Trafego de rede em tempo real (interfaces ativas)",
    "Netwerkverkeer live (actieve interfaces)", "リアルタイム通信量（有効なインターフェース）",
    "Сетевой трафик (активные интерфейсы)", "实时网络流量（所有活动接口）",
    "실시간 네트워크 트래픽 (활성 인터페이스)"};
static const char *s_traffic_rx[LANG_MAX] = {
    "Received", "Recu", "Ricevuti", "Recibido", "Empfangen", "Recebido", "Ontvangen",
    "受信", "Получено", "接收", "수신"};
static const char *s_traffic_tx[LANG_MAX] = {
    "Sent", "Envoye", "Inviati", "Enviado", "Gesendet", "Enviado", "Verzonden",
    "送信", "Отправлено", "发送", "송신"};
static const char *s_traffic_received_total[LANG_MAX] = {
    "Received this session", "Recu depuis ouverture", "Ricevuti in sessione",
    "Recibido en esta sesion", "Empfangen in dieser Sitzung", "Recebido nesta sessao",
    "Ontvangen deze sessie", "今回の受信量", "Получено за сеанс", "本次接收", "이번 세션 수신"};
static const char *s_traffic_sent_total[LANG_MAX] = {
    "Sent this session", "Envoye depuis ouverture", "Inviati in sessione",
    "Enviado en esta sesion", "Gesendet in dieser Sitzung", "Enviado nesta sessao",
    "Verzonden deze sessie", "今回の送信量", "Отправлено за сеанс", "本次发送", "이번 세션 송신"};
static const char *s_traffic_unavailable[LANG_MAX] = {
    "Interface counters are unavailable on this system.",
    "Compteurs d'interface indisponibles sur ce systeme.",
    "Contatori di interfaccia non disponibili.",
    "Contadores de interfaz no disponibles en este sistema.",
    "Schnittstellenzaehler sind nicht verfuegbar.",
    "Contadores de interface indisponiveis neste sistema.",
    "Interfacetellers zijn niet beschikbaar op dit systeem.",
    "このシステムではインターフェース統計を利用できません。",
    "Счетчики интерфейсов недоступны.",
    "此系统无法读取网络接口计数。",
    "이 시스템에서 인터페이스 카운터를 사용할 수 없습니다."};
static const char *s_not_connected[LANG_MAX] = {
    "Not connected to any network.", "Non connecte a un reseau.", "Non connesso a nessuna rete.", "No conectado a ninguna red.",
    "Mit keinem Netzwerk verbunden.", "Nao conectado a nenhuma rede.", "Niet verbonden met een netwerk.",
    "ネットワーク未接続",
    "Нет подключения к сети",
    "未连接到网络",
    "네트워크에 연결되지 않음"};
static const char *s_go_to_settings[LANG_MAX] = {
    "Go to Switch System Settings to connect.", "Allez dans Parametres Switch pour vous connecter.", "Vai su Impostazioni Switch per connetterti.", "Ve a Configuracion del Switch para conectarte.",
    "Gehe zu den Switch-Systemeinstellungen.", "Vai para Configuracoes do Switch para conectar.", "Ga naar Switch-systeeminstellingen om te verbinden.",
    "本体設定でネットワークに接続してください。",
    "Подключитесь в системных настройках.",
    "请在主机设置中连接网络。",
    "시스템 설정에서 네트워크에 연결하세요."};
static const char *s_supports_both[LANG_MAX] = {
    "Supports both Wi-Fi and USB Ethernet.", "Prend en charge Wi-Fi et Ethernet USB.", "Supporta Wi-Fi ed Ethernet USB.", "Compatible con Wi-Fi y Ethernet USB.",
    "Unterstuetzt WLAN und USB-Ethernet.", "Suporta Wi-Fi e Ethernet USB.", "Ondersteunt zowel Wi-Fi als USB Ethernet.",
    "Wi-Fi と USB Ethernet に対応",
    "Поддерживаются Wi-Fi и USB Ethernet",
    "支持 Wi-Fi 和 USB 以太网",
    "Wi-Fi 및 USB 이더넷 지원"};
static const char *s_wifi_not_active[LANG_MAX] = {
    "Wi-Fi not active or not connected.", "Wi-Fi inactif ou non connecte.", "Wi-Fi non attivo o non connesso.", "Wi-Fi inactivo o no conectado.",
    "WLAN nicht aktiv oder nicht verbunden.", "Wi-Fi inativo ou nao conectado.", "Wi-Fi niet actief of niet verbonden.",
    "Wi-Fi が無効、または未接続です。",
    "Wi-Fi выключен или не подключён.",
    "Wi-Fi 未启用或未连接。",
    "Wi-Fi가 꺼져 있거나 연결되지 않음"};
static const char *s_wlan_na[LANG_MAX] = {
    "WLAN diagnostics not available.", "Diagnostic WLAN indisponible.", "Diagnostica WLAN non disponibile.", "Diagnostico WLAN no disponible.",
    "WLAN-Diagnose nicht verfuegbar.", "Diagnostico WLAN indisponivel.", "WLAN-diagnostiek niet beschikbaar.",
    "WLAN 診断を利用できません。",
    "Диагностика WLAN недоступна.",
    "无法读取 WLAN 诊断信息。",
    "WLAN 진단을 사용할 수 없음"};

// --- Page 3: Transfer ---
static const char *s_wifi_transfer[LANG_MAX] = {
    "WiFi %s Transfer", "Transfert WiFi %s", "Trasferimento WiFi %s", "Transferencia WiFi %s",
    "WiFi-%s-Uebertragung", "Transferencia WiFi %s", "WiFi %s-overdracht",
    "Wi-Fi %s 転送",
    "Передача по Wi-Fi: %s",
    "Wi-Fi %s 传输",
    "Wi-Fi %s 전송"};
static const char *s_transfer_file_hint[LANG_MAX] = {
    "Send files to the SD card using FTP over Wi-Fi or MTP over USB.",
    "Envoyez des fichiers sur la carte SD par FTP Wi-Fi ou MTP USB.",
    "Invia file sulla scheda SD tramite FTP Wi-Fi o MTP USB.",
    "Envia archivos a la SD mediante FTP por Wi-Fi o MTP por USB.",
    "Dateien per FTP ueber WLAN oder MTP ueber USB auf die SD-Karte senden.",
    "Envie ficheiros para o SD por FTP via Wi-Fi ou MTP via USB.",
    "Bestanden via FTP over wifi of MTP via USB naar de SD-kaart sturen.",
    "Wi-Fi FTP または USB MTP で SD カードにファイルを送信します。",
    "Передавайте файлы на SD по FTP через Wi-Fi или MTP через USB.",
    "使用 Wi-Fi FTP 或 USB MTP 将文件传到 SD 卡。",
    "Wi-Fi FTP 또는 USB MTP로 SD 카드에 파일을 보냅니다."};
static const char *s_game_package_hint[LANG_MAX] = {
    "NSP/NSZ/XCI/XCZ are copied as files; this app does not install game packages.",
    "NSP/NSZ/XCI/XCZ sont copies comme fichiers ; cette app n'installe pas de jeux.",
    "NSP/NSZ/XCI/XCZ vengono copiati come file; questa app non installa giochi.",
    "NSP/NSZ/XCI/XCZ se copian como archivos; esta app no instala juegos.",
    "NSP/NSZ/XCI/XCZ werden als Dateien kopiert; diese App installiert keine Spiele.",
    "NSP/NSZ/XCI/XCZ sao copiados como ficheiros; esta app nao instala jogos.",
    "NSP/NSZ/XCI/XCZ worden als bestanden gekopieerd; deze app installeert geen games.",
    "NSP/NSZ/XCI/XCZ はファイルとしてコピーします。このアプリはゲームをインストールしません。",
    "NSP/NSZ/XCI/XCZ копируются как файлы; приложение не устанавливает игры.",
    "NSP/NSZ/XCI/XCZ 作为文件复制；此应用不会安装游戏。",
    "NSP/NSZ/XCI/XCZ는 파일로 복사되며, 이 앱은 게임을 설치하지 않습니다."};
static const char *s_transfer_dashboard_title[LANG_MAX] = {
    "FTP / USB MTP FILE TRANSFER",
    "TRANSFERT DE FICHIERS FTP / MTP USB",
    "TRASFERIMENTO FILE FTP / MTP USB",
    "TRANSFERENCIA DE ARCHIVOS FTP / MTP USB",
    "FTP- / USB-MTP-DATEIUEBERTRAGUNG",
    "TRANSFERENCIA DE FICHEIROS FTP / MTP USB",
    "FTP- / USB-MTP-BESTANDSOORDRACHT",
    "FTP / USB MTP ファイル転送",
    "ПЕРЕДАЧА ФАЙЛОВ FTP / USB MTP",
    "FTP / USB MTP 文件传输",
    "FTP / USB MTP 파일 전송"};
static const char *s_no_installed_games[LANG_MAX] = {
    "No installed titles found.", "Aucun titre installe trouve.", "Nessun titolo installato trovato.",
    "No se encontraron titulos instalados.", "Keine installierten Titel gefunden.",
    "Nenhum titulo instalado encontrado.", "Geen geinstalleerde titels gevonden.",
    "インストール済みタイトルがありません。", "Установленные приложения не найдены.",
    "未找到已安装的标题。", "설치된 타이틀이 없습니다."};
static const char *s_running[LANG_MAX] = {
    "RUNNING", "ACTIF", "ATTIVO", "ACTIVO",
    "AKTIV", "ATIVO", "ACTIEF",
    "動作中",
    "РАБОТАЕТ",
    "运行中",
    "실행 중"};
static const char *s_stopped[LANG_MAX] = {
    "STOPPED", "ARRETE", "FERMO", "DETENIDO",
    "GESTOPPT", "PARADO", "GESTOPT",
    "停止中",
    "ОСТАНОВЛЕН",
    "已停止",
    "중지됨"};
static const char *s_address[LANG_MAX] = {
    "Address", "Adresse", "Indirizzo", "Direccion",
    "Adresse", "Endereco", "Adres",
    "アドレス",
    "Адрес",
    "地址",
    "주소"};
static const char *s_current_dir[LANG_MAX] = {
    "Current Dir", "Repertoire courant", "Cartella corrente", "Directorio actual",
    "Aktuelles Verz.", "Diretorio atual", "Huidige map",
    "現在のフォルダー",
    "Текущая папка",
    "当前目录",
    "현재 폴더"};
static const char *s_transfers[LANG_MAX] = {
    "Transfers", "Transferts", "Trasferimenti", "Transferencias",
    "Uebertragungen", "Transferencias", "Overdrachten",
    "転送回数",
    "Передачи",
    "传输次数",
    "전송 횟수"};
static const char *s_data_total[LANG_MAX] = {
    "Data Total", "Total donnees", "Dati totali", "Datos totales",
    "Daten gesamt", "Total de dados", "Gegevens totaal",
    "合計データ量",
    "Всего данных",
    "数据总量",
    "총 데이터"};
static const char *s_ftp_uploading[LANG_MAX] = {
    "FTP upload", "Envoi FTP", "Invio FTP", "Subida FTP",
    "FTP-Upload", "Envio FTP", "FTP-upload",
    "FTP アップロード", "Загрузка FTP", "FTP 上传", "FTP 업로드"};
static const char *s_ftp_downloading[LANG_MAX] = {
    "FTP download", "Reception FTP", "Download FTP", "Descarga FTP",
    "FTP-Download", "Download FTP", "FTP-download",
    "FTP ダウンロード", "Скачивание FTP", "FTP 下载", "FTP 다운로드"};
static const char *s_toggle_wifi[LANG_MAX] = {
    "[A] Toggle WiFi  |  Touch FTP/FTPD", "[A] WiFi ON/OFF  |  Toucher FTP/FTPD", "[A] WiFi ON/OFF  |  Tocca FTP/FTPD", "[A] WiFi ON/OFF  |  Tocar FTP/FTPD",
    "[A] WiFi ein/aus  |  FTP/FTPD beruehren", "[A] Alternar WiFi  |  Tocar FTP/FTPD", "[A] WiFi aan/uit  |  Raak FTP/FTPD",
    "[A] Wi-Fi切替 | FTP/FTPDをタッチ",
    "[A] Wi-Fi | коснитесь FTP/FTPD",
    "[A] 切换 Wi-Fi | 触摸 FTP/FTPD",
    "[A] Wi-Fi 전환 | FTP/FTPD 터치"};
static const char *s_mtp_title[LANG_MAX] = {
    "MTP File Transfer", "Transfert MTP", "Trasferimento MTP", "Transferencia MTP",
    "MTP-Dateiuebertragung", "Transferencia MTP", "MTP-bestandsoverdracht",
    "MTP ファイル転送",
    "Передача файлов MTP",
    "MTP 文件传输",
    "MTP 파일 전송"};
static const char *s_active[LANG_MAX] = {
    "ACTIVE", "ACTIF", "ATTIVO", "ACTIVO",
    "AKTIV", "ATIVO", "ACTIEF",
    "動作中",
    "АКТИВНО",
    "运行中",
    "실행 중"};
static const char *s_protocol[LANG_MAX] = {
    "Protocol", "Protocole", "Protocollo", "Protocolo",
    "Protokoll", "Protocolo", "Protocol",
    "プロトコル",
    "Протокол",
    "协议",
    "프로토콜"};
static const char *s_mtp_protocol[LANG_MAX] = {
    "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)",
    "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)",
    "MTP (Media Transfer Protocol)",
    "MTP (Media Transfer Protocol)",
    "MTP (Media Transfer Protocol)",
    "MTP (Media Transfer Protocol)"};
static const char *s_mtp_xfers_fmt[LANG_MAX] = {
    "%llu transfers", "%llu transferts", "%llu trasferimenti", "%llu transferencias",
    "%llu Uebertragungen", "%llu transferencias", "%llu overdrachten",
    "転送回数: %llu",
    "Передач: %llu",
    "传输次数：%llu",
    "전송 횟수: %llu"};
static const char *s_mtp_desc[LANG_MAX] = {
    "Appears as MTP device on PC (no driver needed)", "Apparait comme peripherique MTP sur PC (pas de pilote)", "Appare come dispositivo MTP su PC (driver non richiesto)", "Aparece como dispositivo MTP en PC (sin controlador)",
    "Erscheint als MTP-Geraet am PC (kein Treiber noetig)", "Aparece como dispositivo MTP no PC (sem driver)", "Verschijnt als MTP-apparaat op pc (geen stuurprogramma nodig)",
    "PC に MTP デバイスとして表示されます（ドライバー不要）",
    "На ПК отображается как MTP-устройство (драйвер не нужен)",
    "在电脑上显示为 MTP 设备（无需驱动程序）",
    "PC에 MTP 장치로 표시됩니다 (드라이버 불필요)"};
static const char *s_toggle_mtp[LANG_MAX] = {
    "[X] Toggle MTP  |  Touch badge", "[X] MTP ON/OFF  |  Toucher badge", "[X] MTP ON/OFF  |  Tocca badge", "[X] MTP ON/OFF  |  Tocar indicador",
    "[X] MTP ein/aus  |  Symbol beruehren", "[X] Alternar MTP  |  Tocar indicador", "[X] MTP aan/uit  |  Raak badge",
    "[X] MTP切替 | バッジをタッチ",
    "[X] MTP | коснитесь значка",
    "[X] 切换 MTP | 触摸标记",
    "[X] MTP 전환 | 배지 터치"};
static const char *s_activity_log[LANG_MAX] = {
    "Activity Log", "Journal d'activite", "Registro attivita", "Registro de actividad",
    "Aktivitaetsprotokoll", "Registro de atividade", "Activiteitenlogboek",
    "アクティビティログ",
    "Журнал событий",
    "活动日志",
    "활동 로그"};
static const char *s_no_activity[LANG_MAX] = {
    "No activity. Press [A] or touch badge to start.", "Aucune activite. Appuyez sur [A] ou touchez le badge.", "Nessuna attivita. Premi [A] o tocca il badge.", "Sin actividad. Presione [A] o toque el indicador.",
    "Keine Aktivitaet. [A] druecken oder Symbol beruehren.", "Sem atividade. Pressione [A] ou toque no indicador.", "Geen activiteit. Druk [A] of raak badge aan.",
    "履歴はありません。[A] または表示をタッチして開始",
    "Нет событий. Нажмите [A] или коснитесь значка.",
    "暂无活动。按 [A] 或触摸标记以启动。",
    "활동 없음. [A] 또는 배지를 눌러 시작"};

// --- Page 4: Performance ---
static const char *s_live_clocks[LANG_MAX] = {
    "Live Clock Frequencies", "Frequences en direct", "Frequenze in tempo reale", "Frecuencias en vivo",
    "Live-Taktfrequenzen", "Frequencias ao vivo", "Live klokfrequenties",
    "現在のクロック周波数",
    "Текущие частоты",
    "实时时钟频率",
    "실시간 클럭 주파수"};
static const char *s_cpu_clock[LANG_MAX] = {
    "CPU Core Clock", "Horloge CPU", "Clock CPU", "Reloj CPU",
    "CPU-Takt", "Clock CPU", "CPU-klok",
    "CPU コア周波数",
    "Частота ядер CPU",
    "CPU 核心频率",
    "CPU 코어 클럭"};
static const char *s_gpu_clock[LANG_MAX] = {
    "GPU Core Clock", "Horloge GPU", "Clock GPU", "Reloj GPU",
    "GPU-Takt", "Clock GPU", "GPU-klok",
    "GPU コア周波数",
    "Частота GPU",
    "GPU 核心频率",
    "GPU 코어 클럭"};
static const char *s_mem_bus[LANG_MAX] = {
    "Memory Bus Clock", "Horloge bus memoire", "Clock bus memoria", "Reloj bus memoria",
    "Speicherbus-Takt", "Clock barramento memoria", "Geheugenbusklok",
    "メモリバス周波数",
    "Частота шины памяти",
    "内存总线频率",
    "메모리 버스 클럭"};
static const char *s_thermal_status[LANG_MAX] = {
    "Thermal Status", "Etat thermique", "Stato termico", "Estado termico",
    "Thermischer Status", "Estado termico", "Thermische status",
    "温度状態",
    "Температура",
    "温度状态",
    "온도 상태"};
static const char *s_core_skin_temp[LANG_MAX] __attribute__((unused)) = {
    "Core Skin Temp", "Temp. boitier coeur", "Temp. scocca core", "Temp. carcasa nucleo",
    "Kern-Gehaeuse-Temp.", "Temp. corpo nucleo", "Kern-behuizing temp.",
    "Core Skin Temp",
    "Core Skin Temp",
    "Core Skin Temp",
    "Core Skin Temp"};
static const char *s_heat_index[LANG_MAX] __attribute__((unused)) = {
    "Heat Index", "Indice de chaleur", "Indice di calore", "Indice de calor",
    "Hitzeindex", "Indice de calor", "Hitte-index",
    "Heat Index",
    "Heat Index",
    "Heat Index",
    "Heat Index"};
static const char *s_perf_profile[LANG_MAX] __attribute__((unused)) = {
    "Performance Profile", "Profil de performances", "Profilo prestazioni", "Perfil de rendimiento",
    "Leistungsprofil", "Perfil de desempenho", "Prestatieprofiel",
    "Performance Profile",
    "Performance Profile",
    "Performance Profile",
    "Performance Profile"};
static const char *s_active_profile[LANG_MAX] = {
    "Active Profile", "Profil actif", "Profilo attivo", "Perfil activo",
    "Aktives Profil", "Perfil ativo", "Actief profiel",
    "現在のプロファイル",
    "Активный профиль",
    "当前性能配置",
    "현재 프로필"};
static const char *s_dock_status[LANG_MAX] __attribute__((unused)) = {
    "Dock Status", "Etat du dock", "Stato dock", "Estado del dock",
    "Dock-Status", "Estado do dock", "Dockstatus",
    "Dock Status",
    "Dock Status",
    "Dock Status",
    "Dock Status"};
static const char *s_docked_high[LANG_MAX] = {
    "Docked (High Speed)", "Dock (Haute vitesse)", "Dock (Alta velocita)", "Acoplado (Alta velocidad)",
    "Docked (Hochgeschw.)", "Dock (Alta velocidade)", "Gedockt (Hoge snelheid)",
    "TVモード（高性能）",
    "Режим ТВ (высокая частота)",
    "底座模式（高性能）",
    "TV 모드 (고성능)"};
static const char *s_handheld_throttled[LANG_MAX] = {
    "Handheld (Throttled)", "Portable (Limite)", "Portatile (Limitato)", "Portatil (Limitado)",
    "Handheld (Gedaempft)", "Portatil (Limitado)", "Handheld (Beperkt)",
    "携帯モード（制限あり）",
    "Портативный режим (ограничен)",
    "掌机模式（受限）",
    "휴대 모드 (제한됨)"};
static const char *s_frame_rate[LANG_MAX] __attribute__((unused)) = {
    "Frame Rate", "Taux d'images", "Frame rate", "Velocidad de fotogramas",
    "Bildrate", "Taxa de quadros", "Framerate",
    "Frame Rate",
    "Frame Rate",
    "Frame Rate",
    "Frame Rate"};
static const char *s_perf_score[LANG_MAX] __attribute__((unused)) = {
    "Perf Score", "Score perf", "Punteggio prest.", "Puntuacion rend.",
    "Leistungswert", "Pontuacao desem.", "Prestatiescore",
    "Perf Score",
    "Perf Score",
    "Perf Score",
    "Perf Score"};
static const char *s_cpu_gpu[LANG_MAX] __attribute__((unused)) = {
    "CPU/GPU", "CPU/GPU", "CPU/GPU", "CPU/GPU",
    "CPU/GPU", "CPU/GPU", "CPU/GPU",
    "CPU/GPU",
    "CPU/GPU",
    "CPU/GPU",
    "CPU/GPU"};
static const char *s_thermal[LANG_MAX] __attribute__((unused)) = {
    "Thermal", "Thermique", "Termico", "Termico",
    "Thermisch", "Termico", "Thermisch",
    "Thermal",
    "Thermal",
    "Thermal",
    "Thermal"};
static const char *s_cpu_load[LANG_MAX] __attribute__((unused)) = {
    "CPU Load", "Charge CPU", "Carico CPU", "Carga CPU",
    "CPU-Last", "Carga CPU", "CPU-belasting",
    "CPU Load",
    "CPU Load",
    "CPU Load",
    "CPU Load"};
static const char *s_gpu_load[LANG_MAX] __attribute__((unused)) = {
    "GPU Load", "Charge GPU", "Carico GPU", "Carga GPU",
    "GPU-Last", "Carga GPU", "GPU-belasting",
    "GPU Load",
    "GPU Load",
    "GPU Load",
    "GPU Load"};
static const char *s_mem_load[LANG_MAX] __attribute__((unused)) = {
    "MEM Load", "Charge MEM", "Carico MEM", "Carga MEM",
    "Speicherlast", "Carga MEM", "GEHEUGEN-belasting",
    "MEM Load",
    "MEM Load",
    "MEM Load",
    "MEM Load"};
static const char *s_fps_history[LANG_MAX] = {
    "App Loop Rate History (60 samples):", "Historique cadence appli (60 mesures) :", "Storico frequenza app (60 campioni):", "Historial de frecuencia de la app (60 muestras):",
    "App-Rate-Verlauf (60 Messungen):", "Historico da frequencia do app (60 amostras):", "App-frequentiegeschiedenis (60 metingen):",
    "App Loop Rate History (60 samples):", "App Loop Rate History (60 samples):", "App Loop Rate History (60 samples):", "App Loop Rate History (60 samples):"};
static const char *s_history_oldest[LANG_MAX] = {
    "oldest", "ancien", "precedente", "anterior", "aeltester", "anterior", "oudste",
    "oldest", "oldest", "oldest", "oldest"};
static const char *s_history_now[LANG_MAX] = {
    "now", "maintenant", "ora", "ahora", "jetzt", "agora", "nu",
    "now", "now", "now", "now"};
static const char *s_history_mid[LANG_MAX] = {
    "mid", "milieu", "meta", "mitad", "Mitte", "meio", "midden",
    "mid", "mid", "mid", "mid"};
static const char *s_ram_usage[LANG_MAX] = {
    "App Memory Usage", "Memoire utilisee par l'application", "Memoria usata dall'app", "Memoria usada por la aplicacion",
    "App-Speichernutzung", "Memoria usada pelo app", "App-geheugengebruik",
    "App Memory Usage", "App Memory Usage", "App Memory Usage", "App Memory Usage"};
static const char *s_used_total[LANG_MAX] __attribute__((unused)) = {
    "Used / Total", "Utilise / Total", "Usato / Totale", "Usado / Total",
    "Belegt / Gesamt", "Usado / Total", "Gebruikt / Totaal",
    "Used / Total",
    "Used / Total",
    "Used / Total",
    "Used / Total"};
static const char *s_mem_usage[LANG_MAX] __attribute__((unused)) = {
    "Memory Usage", "Utilisation memoire", "Utilizzo memoria", "Uso de memoria",
    "Speichernutzung", "Uso de memoria", "Geheugengebruik",
    "Memory Usage",
    "Memory Usage",
    "Memory Usage",
    "Memory Usage"};
static const char *s_ram_info_na[LANG_MAX] = {
    "RAM info unavailable", "Infos RAM indisponibles", "Info RAM non disponibile", "Informacion RAM no disponible",
    "RAM-Info nicht verfuegbar", "Info RAM indisponivel", "RAM-info niet beschikbaar",
    "メモリ情報を取得できません",
    "Данные ОЗУ недоступны",
    "内存信息不可用",
    "RAM 정보를 사용할 수 없음"};
static const char *s_cool[LANG_MAX] = {
    "Cool", "Frais", "Fresco", "Frio",
    "Kuehl", "Frio", "Koel",
    "Cool",
    "Cool",
    "Cool",
    "Cool"};
static const char *s_critical_limit[LANG_MAX] = {
    "CRITICAL LIMIT", "LIMITE CRITIQUE", "LIMITE CRITICO", "LIMITE CRITICO",
    "KRITISCHE GRENZE", "LIMITE CRITICO", "KRITIEKE GRENS",
    "危険温度",
    "КРИТИЧЕСКАЯ ТЕМПЕРАТУРА",
    "温度过高",
    "위험 온도"};
static const char *s_warm_hot[LANG_MAX] = {
    "Warm / Hot", "Tiede / Chaud", "Caldo / Molto caldo", "Templado / Caliente",
    "Warm / Heiss", "Morno / Quente", "Warm / Heet",
    "温かい／高温",
    "Тепло / жарко",
    "偏热 / 过热",
    "따뜻함 / 고온"};
static const char *s_power_save[LANG_MAX] = {
    "Power Saving Mode", "Mode economie d'energie", "Modalita risparmio energetico", "Modo ahorro de energia",
    "Stromsparmodus", "Modo economia de energia", "Energiebesparingsmodus",
    "省電力モード",
    "Режим энергосбережения",
    "省电模式",
    "절전 모드"};
static const char *s_high_perf[LANG_MAX] = {
    "High Performance", "Haute performance", "Alte prestazioni", "Alto rendimiento",
    "Hohe Leistung", "Alto desempenho", "Hoge prestatie",
    "高性能",
    "Высокая производительность",
    "高性能",
    "고성능"};
static const char *s_boost_profile[LANG_MAX] = {
    "Horizon Boost Profile", "Profil Boost Horizon", "Profilo Boost Horizon", "Perfil Boost Horizon",
    "Horizon-Boost-Profil", "Perfil Boost Horizon", "Horizon Boost-profiel",
    "Horizon Boost Profile",
    "Horizon Boost Profile",
    "Horizon Boost Profile",
    "Horizon Boost Profile"};
static const char *s_perf_score_fmt[LANG_MAX] = {
    "Performance Score: %u", "Score performance: %u", "Punteggio prestazioni: %u", "Puntuacion rendimiento: %u",
    "Leistungswert: %u", "Pontuacao de desempenho: %u", "Prestatiescore: %u",
    "Performance Score: %u",
    "Performance Score: %u",
    "Performance Score: %u",
    "Performance Score: %u"};
static const char *s_current_fps_fmt[LANG_MAX] = {
    "App loop: %d FPS", "Application: %d FPS", "App: %d FPS", "App: %d FPS",
    "App: %d FPS", "App: %d FPS", "App: %d FPS",
    "App loop: %d FPS", "App loop: %d FPS", "App loop: %d FPS", "App loop: %d FPS"};
static const char *s_mem_status_high[LANG_MAX] = {
    "HIGH", "ELEVE", "ALTO", "ALTO",
    "HOCH", "ALTO", "HOOG",
    "高",
    "ВЫСОКАЯ",
    "高",
    "높음"};
static const char *s_mem_status_elevated[LANG_MAX] = {
    "Elevated", "Eleve", "Elevato", "Elevado",
    "Erhoeht", "Elevado", "Verhoogd",
    "やや高い",
    "Повышенная",
    "偏高",
    "높은 편"};
// --- Page 5: Controller ---
static const char *s_jc_title[LANG_MAX] = {
    "Joy-Con Inputs & Analog Sticks Diagnostics", "Test Joy-Con & Joysticks", "Test Joy-Con & Stick analogici", "Prueba de mandos y joysticks",
    "Joy-Con-Eingaben & Analogsticks", "Teste Joy-Con & Analógicos", "Joy-Con-ingangen & analoge sticks test",
    "Joy-Con Inputs & Analog Sticks Diagnostics",
    "Joy-Con Inputs & Analog Sticks Diagnostics",
    "Joy-Con Inputs & Analog Sticks Diagnostics",
    "Joy-Con Inputs & Analog Sticks Diagnostics"};
static const char *s_triggers[LANG_MAX] = {
    "Triggers:", "Gachettes:", "Grilletto:", "Gatillos:",
    "Trigger:", "Gatilhos:", "Triggers:",
    "Triggers:",
    "Triggers:",
    "Triggers:",
    "Triggers:"};
static const char *s_controller_type[LANG_MAX] = {
    "Controller Type", "Type de manette", "Tipo di controller", "Tipo de mando",
    "Controller-Typ", "Tipo de controle", "Controllertype",
    "Controller Type",
    "Controller Type",
    "Controller Type",
    "Controller Type"};
static const char *s_gyro_accel[LANG_MAX] = {
    "Gyroscope & Accelerometer", "Gyroscope & Accelerometre", "Giroscopio & Accelerometro", "Giroscopio & Acelerometro",
    "Gyroskop & Beschleunigungsmesser", "Giroscopio & Acelerometro", "Gyroscoop & Accelerometer",
    "Gyroscope & Accelerometer",
    "Gyroscope & Accelerometer",
    "Gyroscope & Accelerometer",
    "Gyroscope & Accelerometer"};
static const char *s_left_jc[LANG_MAX] = {
    "Left Joy-Con", "Joy-Con Gauche", "Joy-Con Sinistro", "Joy-Con Izquierdo",
    "Linker Joy-Con", "Joy-Con Esquerdo", "Linker Joy-Con",
    "Left Joy-Con",
    "Left Joy-Con",
    "Left Joy-Con",
    "Left Joy-Con"};
static const char *s_right_jc[LANG_MAX] = {
    "Right Joy-Con", "Joy-Con Droit", "Joy-Con Destro", "Joy-Con Derecho",
    "Rechter Joy-Con", "Joy-Con Direito", "Rechter Joy-Con",
    "Right Joy-Con",
    "Right Joy-Con",
    "Right Joy-Con",
    "Right Joy-Con"};
static const char *s_sixaxis_na[LANG_MAX] = {
    "Six-axis sensors not available (unsupported controller or emulator)", "Capteurs 6 axes non disponibles (manette non compatible ou emulateur)", "Sensori 6-assi non disponibili (controller non supportato o emulatore)", "Sensores de 6 ejes no disponibles (mando no compatible o emulador)",
    "6-Achsen-Sensoren nicht verfuegbar (nicht unterstuetzter Controller oder Emulator)", "Sensores 6 eixos indisponiveis (controle nao suportado ou emulador)", "6-assige sensoren niet beschikbaar (niet-ondersteunde controller of emulator)",
    "Six-axis sensors not available (unsupported controller or emulator)",
    "Six-axis sensors not available (unsupported controller or emulator)",
    "Six-axis sensors not available (unsupported controller or emulator)",
    "Six-axis sensors not available (unsupported controller or emulator)"};

// --- Page 6: Tools ---
static const char *s_br_ctrl[LANG_MAX] = {
    "Display Brightness Control", "Controle de luminosite", "Controllo luminosita", "Control de brillo",
    "Display-Helligkeit", "Controle de brilho", "Helderheidsbediening",
    "Display Brightness Control",
    "Display Brightness Control",
    "Display Brightness Control",
    "Display Brightness Control"};
static const char *s_current_br_fmt[LANG_MAX] = {
    "Current: %.0f%%", "Actuel: %.0f%%", "Attuale: %.0f%%", "Actual: %.0f%%",
    "Aktuell: %.0f%%", "Atual: %.0f%%", "Huidig: %.0f%%",
    "Current: %.0f%%",
    "Current: %.0f%%",
    "Current: %.0f%%",
    "Current: %.0f%%"};
static const char *s_auto_on_off[LANG_MAX] = {
    "Auto: ON", "Auto: active", "Auto: attiva", "Auto: activo",
    "Auto: AN", "Auto: ativo", "Auto: AAN",
    "Auto: ON",
    "Auto: ON",
    "Auto: ON",
    "Auto: ON"};
static const char *s_auto_off_on[LANG_MAX] = {
    "Auto: OFF", "Auto: inactive", "Auto: disattiva", "Auto: inactivo",
    "Auto: AUS", "Auto: inativo", "Auto: UIT",
    "Auto: OFF",
    "Auto: OFF",
    "Auto: OFF",
    "Auto: OFF"};
static const char *s_br_adjust[LANG_MAX] = {
    "[DPad] Adjust  |  Touch drag", "[DPad] Ajuster  |  Toucher glisser", "[DPad] Regola  |  Tocca trascina", "[DPad] Ajustar  |  Tocar arrastrar",
    "[DPad] Einstellen  |  Beruehren ziehen", "[DPad] Ajustar  |  Tocar arrastar", "[DPad] Aanpassen  |  Raak en sleep",
    "[DPad] Adjust  |  Touch drag",
    "[DPad] Adjust  |  Touch drag",
    "[DPad] Adjust  |  Touch drag",
    "[DPad] Adjust  |  Touch drag"};
static const char *s_br_na[LANG_MAX] = {
    "Brightness N/A (emulator)", "Luminosite N/D (emulateur)", "Luminosita N/D (emulatore)", "Brillo N/D (emulador)",
    "Helligkeit N/V (Emulator)", "Brilho N/D (emulador)", "Helderheid N/B (emulator)",
    "Brightness N/A (emulator)",
    "Brightness N/A (emulator)",
    "Brightness N/A (emulator)",
    "Brightness N/A (emulator)"};
static const char *s_haptic_test[LANG_MAX] = {
    "Haptic Vibration Test", "Test vibration haptique", "Test vibrazione aptica", "Prueba de vibracion",
    "Haptischer Vibrationstest", "Teste de vibracao haptica", "Haptische vibratietest",
    "Haptic Vibration Test",
    "Haptic Vibration Test",
    "Haptic Vibration Test",
    "Haptic Vibration Test"};
static const char *s_test_haptic[LANG_MAX] = {
    "Test Joy-Con haptic motors:", "Tester moteurs haptiques Joy-Con:", "Test motori aptici Joy-Con:", "Probar motores hapticos Joy-Con:",
    "Joy-Con-Haptikmotoren testen:", "Testar motores hapticos Joy-Con:", "Test Joy-Con haptische motoren:",
    "Test Joy-Con haptic motors:",
    "Test Joy-Con haptic motors:",
    "Test Joy-Con haptic motors:",
    "Test Joy-Con haptic motors:"};
static const char *s_left_rumble[LANG_MAX] = {
    "[X] Left Rumble", "[X] Rumble Gauche", "[X] Rumble Sinistro", "[X] Vibracion Izquierda",
    "[X] Links Rumble", "[X] Rumble Esquerdo", "[X] Links trillen",
    "[X] Left Rumble",
    "[X] Left Rumble",
    "[X] Left Rumble",
    "[X] Left Rumble"};
static const char *s_right_rumble[LANG_MAX] = {
    "[Y] Right Rumble", "[Y] Rumble Droit", "[Y] Rumble Destro", "[Y] Vibracion Derecha",
    "[Y] Rechts Rumble", "[Y] Rumble Direito", "[Y] Rechts trillen",
    "[Y] Right Rumble",
    "[Y] Right Rumble",
    "[Y] Right Rumble",
    "[Y] Right Rumble"};
static const char *s_report_export[LANG_MAX] = {
    "System Report Export", "Export rapport systeme", "Esportazione rapporto", "Exportar informe",
    "Systembericht exportieren", "Exportar relatorio", "Systeemrapport exporteren",
    "System Report Export",
    "System Report Export",
    "System Report Export",
    "System Report Export"};
static const char *s_perf_export[LANG_MAX] = {
    "Perf Metrics Export", "Export metrics Perf", "Esporta metriche Perf", "Exportar metrics Perf",
    "Perf-Metriken exportieren", "Exportar metricas Perf", "Exporteren perf-metrics",
    "Perf Metrics Export",
    "Perf Metrics Export",
    "Perf Metrics Export",
    "Perf Metrics Export"};
static const char *s_perf_export_desc[LANG_MAX] __attribute__((unused)) = {
    "Export live perf metrics to SD card:", "Exporter les metrics Perf vers SD:", "Esporta metriche Perf su SD:", "Exportar metricas Perf a SD:",
    "Perf-Metriken auf SD exportieren:", "Exportar metricas Perf para SD:", "Exporteren perf-metrics naar SD:",
    "Export live perf metrics to SD card:",
    "Export live perf metrics to SD card:",
    "Export live perf metrics to SD card:",
    "Export live perf metrics to SD card:"};
static const char *s_export_desc[LANG_MAX] = {
    "Export full diagnostics to SD card:", "Exporter le diagnostic complet vers carte SD:", "Esporta diagnostica completa su SD:", "Exportar diagnostico completo a tarjeta SD:",
    "Vollstaendige Diagnose auf SD-Karte exportieren:", "Exportar diagnostico completo para cartao SD:", "Volledige diagnostiek naar SD-kaart exporteren:",
    "Export full diagnostics to SD card:",
    "Export full diagnostics to SD card:",
    "Export full diagnostics to SD card:",
    "Export full diagnostics to SD card:"};
static const char *s_export_btn[LANG_MAX] = {
    "[B] Export Report", "[B] Exporter rapport", "[B] Esporta rapporto", "[B] Exportar informe",
    "[B] Bericht exportieren", "[B] Exportar relatorio", "[B] Rapport exporteren",
    "[B] Export Report",
    "[B] Export Report",
    "[B] Export Report",
    "[B] Export Report"};
static const char *s_export_perf_hint[LANG_MAX] = {
    "[Touch] Export CSV or press Y", "[Tactile] Exporter CSV ou appuyer sur Y",
    "[Tocco] Esporta CSV o premi Y", "[Tocar] Exportar CSV o pulsar Y",
    "[Touch] CSV exportieren oder Y druecken", "[Toque] Exportar CSV ou pressionar Y",
    "[Tik] CSV exporteren of druk op Y", "[タッチ] CSV出力、またはY",
    "[Касание] Экспорт CSV или кнопка Y", "[触摸] 导出 CSV 或按 Y", "[터치] CSV 내보내기 또는 Y"};
static const char *s_perf_export_ok[LANG_MAX] = {
    "Metrics saved to perf_metrics_v06.csv", "Mesures enregistrees dans perf_metrics_v06.csv",
    "Metriche salvate in perf_metrics_v06.csv", "Metricas guardadas en perf_metrics_v06.csv",
    "Metriken in perf_metrics_v06.csv gespeichert", "Metricas salvas em perf_metrics_v06.csv",
    "Metrieken opgeslagen in perf_metrics_v06.csv", "perf_metrics_v06.csv に保存しました",
    "Метрики сохранены в perf_metrics_v06.csv", "指标已保存到 perf_metrics_v06.csv",
    "측정값을 perf_metrics_v06.csv에 저장했습니다"};
static const char *s_perf_export_fail[LANG_MAX] = {
    "Export failed; check SD card and free space", "Echec export ; verifier carte SD et espace libre",
    "Esportazione non riuscita; controlla SD e spazio", "Error de exportacion; revisa SD y espacio",
    "Export fehlgeschlagen; SD-Karte und Speicher pruefen",
    "Falha na exportacao; verifique o SD e o espaco",
    "Export mislukt; controleer SD-kaart en vrije ruimte",
    "出力失敗。SDカードと空き容量を確認してください",
    "Ошибка экспорта; проверьте SD-карту и место",
    "导出失败；请检查 SD 卡和剩余空间",
    "내보내기 실패; SD 카드와 여유 공간을 확인하세요"};
static const char *s_export_save[LANG_MAX] = {
    "Saves to sdmc:/switch/SwitchInfoNX/", "Sauvegarde dans sdmc:/switch/SwitchInfoNX/", "Salvato in sdmc:/switch/SwitchInfoNX/", "Guardado en sdmc:/switch/SwitchInfoNX/",
    "Speichert nach sdmc:/switch/SwitchInfoNX/", "Salvo em sdmc:/switch/SwitchInfoNX/", "Opslaan naar sdmc:/switch/SwitchInfoNX/",
    "Saves to sdmc:/switch/SwitchInfoNX/",
    "Saves to sdmc:/switch/SwitchInfoNX/",
    "Saves to sdmc:/switch/SwitchInfoNX/",
    "Saves to sdmc:/switch/SwitchInfoNX/"};
static const char *s_runtime_env[LANG_MAX] = {
    "Runtime Environment", "Env. d'execution", "Ambiente di esecuzione", "Entorno de ejecucion",
    "Laufzeitumgebung", "Ambiente de execucao", "Runtime-omgeving",
    "Runtime Environment",
    "Runtime Environment",
    "Runtime Environment",
    "Runtime Environment"};
static const char *s_app_status[LANG_MAX] = {
    "App Status", "Etat de l'app", "Stato app", "Estado de la app",
    "App-Status", "Estado do app", "App-status",
    "App Status",
    "App Status",
    "App Status",
    "App Status"};
static const char *s_running_ok[LANG_MAX] = {
    "Running Normally", "Fonctionne normalement", "In esecuzione normale", "Funcionando normalmente",
    "Laeuft normal", "Executando normalmente", "Normaal actief",
    "Running Normally",
    "Running Normally",
    "Running Normally",
    "Running Normally"};
static const char *s_render[LANG_MAX] = {
    "Render", "Rendu", "Render", "Render",
    "Render", "Render", "Render",
    "Render",
    "Render",
    "Render",
    "Render"};
static const char *s_env_label[LANG_MAX] = {
    "Environment", "Environnement", "Ambiente", "Entorno",
    "Umgebung", "Ambiente", "Omgeving",
    "Environment",
    "Environment",
    "Environment",
    "Environment"};
static const char *s_emulator[LANG_MAX] = {
    "Emulator", "Emulateur", "Emulatore", "Emulador",
    "Emulator", "Emulador", "Emulator",
    "Emulator",
    "Emulator",
    "Emulator",
    "Emulator"};
static const char *s_retail[LANG_MAX] = {
    "Retail Hardware", "Materiel retail", "Hardware retail", "Hardware minorista",
    "Retail-Hardware", "Hardware original", "Retail hardware",
    "Retail Hardware",
    "Retail Hardware",
    "Retail Hardware",
    "Retail Hardware"};
static const char *s_dead_pixel[LANG_MAX] = {
    "[Touch] Dead-Pixel Test", "[Touch] Test pixel mort", "[Touch] Test pixel morto", "[Touch] Prueba de pixeles",
    "[Touch] Dead-Pixel-Test", "[Touch] Teste pixel morto", "[Touch] Dode pixel test",
    "[Touch] Dead-Pixel Test",
    "[Touch] Dead-Pixel Test",
    "[Touch] Dead-Pixel Test",
    "[Touch] Dead-Pixel Test"};
static const char *s_fan_ctrl[LANG_MAX] = {
    "Custom Fan Speed Control", "Controle ventilateur personnalise", "Controllo ventola personalizzato", "Control de ventilador personalizado",
    "Benutzerdefinierte Lueftersteuerung", "Controle de ventoinha personalizado", "Aangepaste ventilatorbediening",
    "カスタムファン速度制御",
    "Настройка скорости вентилятора",
    "自定义风扇速度控制",
    "사용자 지정 팬 속도 제어"};
static const char *s_fan_dev[LANG_MAX] = {
    "Manual control while app is open", "Controle manuel pendant l'app", "Controllo manuale con app aperta", "Control manual con la app abierta",
    "Manuellsteuerung bei geoeffneter App", "Controlo manual com a app aberta", "Handmatige bediening zolang app open is",
    "アプリ起動中のみ手動制御", "Ручное управление, пока приложение открыто", "应用运行时手动控制", "앱이 실행 중일 때 수동 제어"};
static const char *s_fan_req[LANG_MAX] = {
    "[Touch] Open controls (30-100%); auto restores on exit",
    "[Tactile] Ouvrir (30-100 %); auto restauree a la sortie",
    "[Tocco] Apri (30-100%); auto al termine",
    "[Tocar] Abrir (30-100%); auto al salir",
    "[Touch] Steuerung (30-100%); Auto bei Ende",
    "[Toque] Abrir (30-100%); auto ao sair",
    "[Tik] Open (30-100%); auto bij afsluiten",
    "[タッチ] 操作を開く（30-100%）。終了時に自動制御へ",
    "[Касание] Управление (30-100%). Авто при выходе",
    "[触摸] 打开控制 (30-100%)；退出时恢复自动",
    "[터치] 제어 열기 (30-100%); 종료 시 자동 복원"};
static const char *s_fan_title[LANG_MAX] = {
    "Fan Speed Control", "Vitesse du ventilateur", "Velocita ventola", "Velocidad del ventilador", "Lueftergeschwindigkeit", "Velocidade da ventoinha", "Ventilatorsnelheid",
    "ファン速度", "Скорость вентилятора", "风扇转速", "팬 속도"};
static const char *s_fan_auto[LANG_MAX] = {
    "AUTO", "AUTO", "AUTO", "AUTO", "AUTO", "AUTO", "AUTO", "自動", "АВТО", "自动", "자동"};
static const char *s_fan_apply[LANG_MAX] = {
    "APPLY", "APPLIQUER", "APPLICA", "APLICAR", "ANWENDEN", "APLICAR", "TOEPASSEN", "適用", "ПРИМЕНИТЬ", "应用", "적용"};
static const char *s_fan_safe_hint[LANG_MAX] = {
    "Target 30-100%; thermal safety may override or limit it",
    "Cible 30-100 % ; la securite thermique peut la limiter",
    "Obiettivo 30-100%; la protezione termica puo limitarlo",
    "Objetivo 30-100%; la seguridad termica puede limitarlo",
    "Ziel 30-100%; der Hitzeschutz kann den Wert begrenzen",
    "Alvo 30-100%; a protecao termica pode limita-lo",
    "Doel 30-100%; thermische beveiliging kan begrenzen",
    "目標 30-100%。温度保護で制限される場合があります",
    "Цель 30-100%. Термозащита может ограничить скорость",
    "目标 30-100%；温度保护可能会限制转速",
    "목표 30-100%; 온도 보호가 제한할 수 있습니다"};
static const char *s_fan_feedback_fmt[LANG_MAX] = {
    "Controller reports %d%%", "Ventilateur indique %d%%", "Controller indica %d%%",
    "Controlador indica %d%%", "Controller meldet %d%%", "Controlador indica %d%%",
    "Controller meldt %d%%", "コントローラー表示 %d%%", "Контроллер сообщает %d%%",
    "Контроллер显示 %d%%", "컨트롤러 표시 %d%%"};
static const char *s_fan_feedback_na[LANG_MAX] = {
    "Not active; touch to open controls", "Inactif ; toucher pour regler",
    "Non attivo; tocca per aprire i controlli", "Inactivo; toca para abrir controles",
    "Inaktiv; zum Oeffnen beruehren", "Inativo; toque para abrir os controles",
    "Inactief; tik om bediening te openen", "未启用；触摸打开控制",
    "Не запущен; коснитесь для настройки", "未启用；触摸打开控制", "비활성; 터치하여 제어 열기"};
static const char *s_app_mode[LANG_MAX] = {
    "App Mode", "Mode App", "Modalita App", "Modo de App",
    "App-Modus", "Modo do App", "App-modus",
    "App Mode",
    "App Mode",
    "App Mode",
    "App Mode"};
static const char *s_modes[LANG_MAX][3] = {
    {"Normal App", "Overlay", "Sysmodule"},
    {"App normale", "Overlay", "Sysmodule"},
    {"App normale", "Overlay", "Sysmodule"},
    {"App normal", "Overlay", "Sysmodule"},
    {"Normale App", "Overlay", "Sysmodule"},
    {"App normal", "Overlay", "Sysmodule"},
    {"Normale app", "Overlay", "Sysmodule"},
    {"Normal App", "Overlay", "Sysmodule"},
    {"Normal App", "Overlay", "Sysmodule"},
    {"Normal App", "Overlay", "Sysmodule"},
    {"Normal App", "Overlay", "Sysmodule"}};
static const char *s_mode_switch[LANG_MAX] = {
    "This build runs as a normal homebrew app.", "Cette version fonctionne comme une app homebrew.", "Questa versione funziona come app homebrew.", "Esta version funciona como app homebrew.",
    "Diese Version laeuft als Homebrew-App.", "Esta versao funciona como app homebrew.", "Deze versie werkt als homebrew-app.",
    "このビルドは通常の自作アプリです。", "Эта сборка работает как обычное приложение.", "此版本以普通自制软件运行。", "이 빌드는 일반 홈브류 앱으로 실행됩니다."};
static const char *s_overlay_detected[LANG_MAX] = {
    "OVL detected: SwitchInfoNX.ovl is installed.",
    "OVL detecte : SwitchInfoNX.ovl est installe.",
    "OVL rilevato: SwitchInfoNX.ovl e installato.",
    "OVL detectado: SwitchInfoNX.ovl esta instalado.",
    "OVL erkannt: SwitchInfoNX.ovl ist installiert.",
    "OVL detetado: SwitchInfoNX.ovl esta instalado.",
    "OVL gevonden: SwitchInfoNX.ovl is geinstalleerd.",
    "OVLを検出しました：SwitchInfoNX.ovlはインストール済みです。",
    "OVL обнаружен: SwitchInfoNX.ovl установлен.",
    "已检测到 OVL：SwitchInfoNX.ovl 已安装。",
    "OVL 감지됨: SwitchInfoNX.ovl이 설치되어 있습니다."};
static const char *s_overlay_missing[LANG_MAX] = {
    "OVL not found. Copy it to /switch/.overlays/SwitchInfoNX.ovl.",
    "OVL introuvable. Copiez-le dans /switch/.overlays/SwitchInfoNX.ovl.",
    "OVL non trovato. Copialo in /switch/.overlays/SwitchInfoNX.ovl.",
    "OVL no encontrado. Copialo en /switch/.overlays/SwitchInfoNX.ovl.",
    "OVL nicht gefunden. Kopiere es nach /switch/.overlays/SwitchInfoNX.ovl.",
    "OVL nao encontrado. Copia-o para /switch/.overlays/SwitchInfoNX.ovl.",
    "OVL niet gevonden. Kopieer het naar /switch/.overlays/SwitchInfoNX.ovl.",
    "OVLが見つかりません。/switch/.overlays/SwitchInfoNX.ovlにコピーしてください。",
    "OVL не найден. Скопируйте его в /switch/.overlays/SwitchInfoNX.ovl.",
    "未找到 OVL。请将其复制到 /switch/.overlays/SwitchInfoNX.ovl。",
    "OVL을 찾을 수 없습니다. /switch/.overlays/SwitchInfoNX.ovl에 복사하세요."};
static const char *s_overlay_invalid[LANG_MAX] = {
    "File found, but its ULTR signature is missing or invalid.",
    "Fichier trouve, mais sa signature ULTR est absente ou invalide.",
    "File trovato, ma la firma ULTR manca o non e valida.",
    "Archivo encontrado, pero falta la firma ULTR o no es valida.",
    "Datei gefunden, aber die ULTR-Signatur fehlt oder ist ungueltig.",
    "Ficheiro encontrado, mas falta a assinatura ULTR ou e invalida.",
    "Bestand gevonden, maar de ULTR-handtekening ontbreekt of is ongeldig.",
    "ファイルはありますが、ULTR署名がないか無効です。",
    "Файл найден, но подпись ULTR отсутствует или недействительна.",
    "已找到文件，但缺少 ULTR 签名或签名无效。",
    "파일을 찾았지만 ULTR 서명이 없거나 올바르지 않습니다."};
enum {
    OVERLAY_STATUS_UNKNOWN = -1,
    OVERLAY_STATUS_MISSING,
    OVERLAY_STATUS_INVALID,
    OVERLAY_STATUS_DETECTED
};
static int overlay_status = OVERLAY_STATUS_UNKNOWN;

static int detect_ultrahand_overlay(void) {
    FILE *file = fopen("sdmc:/switch/.overlays/SwitchInfoNX.ovl", "rb");
    if (!file)
        return OVERLAY_STATUS_MISSING;

    int status = OVERLAY_STATUS_INVALID;
    if (fseek(file, -4, SEEK_END) == 0) {
        char signature[4];
        if (fread(signature, 1, sizeof(signature), file) == sizeof(signature) &&
            memcmp(signature, "ULTR", sizeof(signature)) == 0)
            status = OVERLAY_STATUS_DETECTED;
    }
    fclose(file);
    return status;
}

static const char *overlay_status_text(int status) {
    if (status == OVERLAY_STATUS_DETECTED)
        return s_overlay_detected[cur_lang];
    if (status == OVERLAY_STATUS_INVALID)
        return s_overlay_invalid[cur_lang];
    return s_overlay_missing[cur_lang];
}
static const char *s_sysmodule_desc[LANG_MAX] = {
    "A sysmodule must be a separate Horizon service; this app is not one.", "Un sysmodule est un service Horizon distinct; cette app n'en est pas un.", "Un sysmodule e un servizio Horizon separato; questa app non lo e.", "Un sysmodule es un servicio Horizon separado; esta app no lo es.",
    "Ein Sysmodul ist ein eigener Horizon-Dienst; diese App ist keiner.", "Um sysmodule e um servico Horizon separado; este app nao e.", "Een sysmodule is een aparte Horizon-service; deze app is dat niet.",
    "Sysmoduleは別のHorizonサービスです。このアプリとは別です。", "Системный модуль — отдельная служба Horizon, не это приложение.", "系统模块是独立的 Horizon 服务，本应用不是。", "시스템 모듈은 별도 Horizon 서비스이며 이 앱과 다릅니다."};
static const char *s_console_info[LANG_MAX] = {
    "Console Information", "Informations console", "Informazioni console", "Informacion de la consola",
    "Konsoleninformation", "Informacoes do console", "Console-informatie",
    "Console Information",
    "Console Information",
    "Console Information",
    "Console Information"};
static const char *s_serial_emu[LANG_MAX] = {
    "(emulator/dev unit)", "(emulateur/unite dev)", "(emulatore/unita dev)", "(emulador/unidad dev)",
    "(Emulator/Entwicklung)", "(emulador/unidade dev)", "(emulator/dev-eenheid)",
    "(emulator/dev unit)",
    "(emulator/dev unit)",
    "(emulator/dev unit)",
    "(emulator/dev unit)"};
static const char *s_nickname[LANG_MAX] = {
    "Nickname", "Surnom", "Soprannome", "Apodo",
    "Spitzname", "Apelido", "Bijnaam",
    "Nickname",
    "Nickname",
    "Nickname",
    "Nickname"};
static const char *s_sd_info_speed[LANG_MAX] = {
    "SD Card Info & Speed Test", "Infos carte SD & test vitesse", "Info SD e test velocita", "Informacion SD y prueba velocidad",
    "SD-Karteninfo & Geschw.-Test", "Info cartao SD e teste velocidade", "SD-kaartinfo & snelheidstest",
    "SD Card Info & Speed Test",
    "SD Card Info & Speed Test",
    "SD Card Info & Speed Test",
    "SD Card Info & Speed Test"};
static const char *s_total_capacity[LANG_MAX] = {
    "Total Capacity", "Capacite totale", "Capacita totale", "Capacidad total",
    "Gesamtkapazitaet", "Capacidade total", "Totale capaciteit",
    "Total Capacity",
    "Total Capacity",
    "Total Capacity",
    "Total Capacity"};
static const char *s_free_space[LANG_MAX] = {
    "Free Space", "Espace libre", "Spazio libero", "Espacio libre",
    "Freier Speicher", "Espaco livre", "Vrije ruimte",
    "Free Space",
    "Free Space",
    "Free Space",
    "Free Space"};
static const char *s_pct_used_fmt[LANG_MAX] = {
    "%.1f%% used", "%.1f%% utilise", "%.1f%% usato", "%.1f%% usado",
    "%.1f%% belegt", "%.1f%% usado", "%.1f%% gebruikt",
    "%.1f%% used",
    "%.1f%% used",
    "%.1f%% used",
    "%.1f%% used"};
static const char *s_read_speed_btn[LANG_MAX] = {
    "[Touch] Test Read Speed", "[Touch] Test vitesse lecture", "[Touch] Test velocita lettura", "[Touch] Probar velocidad lectura",
    "[Touch] Lese-Geschw. testen", "[Touch] Testar velocidade leitura", "[Touch] Lees snelheid testen",
    "[Touch] Test Read Speed",
    "[Touch] Test Read Speed",
    "[Touch] Test Read Speed",
    "[Touch] Test Read Speed"};
static const char *s_testing[LANG_MAX] = {
    "Testing...", "Test...", "Test...", "Probando...",
    "Test...", "Testando...", "Testen...",
    "Testing...",
    "Testing...",
    "Testing...",
    "Testing..."};
static const char *s_error[LANG_MAX] = {
    "Error!", "Erreur!", "Errore!", "Error!",
    "Fehler!", "Erro!", "Fout!",
    "Error!",
    "Error!",
    "Error!",
    "Error!"};
static const char *s_not_tested[LANG_MAX] = {
    "Not tested", "Non teste", "Non testato", "No probado",
    "Nicht getestet", "Nao testado", "Niet getest",
    "Not tested",
    "Not tested",
    "Not tested",
    "Not tested"};
static const char *s_sd_info_na[LANG_MAX] = {
    "SD card info unavailable", "Infos carte SD indisponibles", "Info SD non disponibile", "Info SD no disponible",
    "SD-Karteninfo nicht verfuegbar", "Info cartao SD indisponivel", "SD-kaartinfo niet beschikbaar",
    "SD card info unavailable",
    "SD card info unavailable",
    "SD card info unavailable",
    "SD card info unavailable"};

// --- Page 7: About ---
static const char *s_about_title[LANG_MAX] = {
    "About Switch Info NX", "A propos de Switch Info NX", "Informazioni su Switch Info NX", "Acerca de Switch Info NX",
    "Ueber Switch Info NX", "Sobre Switch Info NX", "Over Switch Info NX",
    "Switch Info NXについて",
    "О Switch Info NX",
    "关于 Switch Info NX",
    "Switch Info NX 정보"};
static const char *s_created_by[LANG_MAX] = {
    "v" APP_VERSION "  |  Created by dodosi", "v" APP_VERSION "  |  Cree par dodosi", "v" APP_VERSION "  |  Creato da dodosi", "v" APP_VERSION "  |  Creado por dodosi",
    "v" APP_VERSION "  |  Erstellt von dodosi", "v" APP_VERSION "  |  Criado por dodosi", "v" APP_VERSION "  |  Gemaakt door dodosi",
    "v" APP_VERSION "  |  dodosi 作成", "v" APP_VERSION "  |  Автор: dodosi", "v" APP_VERSION "  |  作者：dodosi", "v" APP_VERSION "  |  제작: dodosi"};
static const char *s_desc_line1[LANG_MAX] = {
    "A premium system information and hardware diagnostic utility", "Un utilitaire d'information systeme et de diagnostic materiel", "Un utility di informazioni di sistema e diagnostica hardware", "Una utilidad de informacion del sistema y diagnostico de hardware",
    "Ein Premium-Systeminformations- und Hardware-Diagnose-Tool", "Um utilitario de informacoes do sistema e diagnostico de hardware", "Een premium systeeminformatie- en hardwarediagnosetool",
    "高機能なシステム情報・ハードウェア診断ツール",
    "Утилита системной информации и диагностики оборудования",
    "系统信息与硬件诊断工具",
    "시스템 정보 및 하드웨어 진단 도구"};
static const char *s_desc_line2[LANG_MAX] = {
    "for Nintendo Switch homebrew custom firmware.", "pour Nintendo Switch homebrew custom firmware.", "per Nintendo Switch homebrew custom firmware.", "para Nintendo Switch homebrew custom firmware.",
    "fuer Nintendo Switch Homebrew Custom Firmware.", "para Nintendo Switch homebrew custom firmware.", "voor Nintendo Switch homebrew custom firmware.",
    "Nintendo Switch のカスタムファームウェア向け。",
    "для Nintendo Switch с кастомной прошивкой.",
    "适用于 Nintendo Switch 自制系统环境。",
    "Nintendo Switch 커스텀 펌웨어용입니다."};

// --- Dead Pixel Test ---
static const char *s_dead_pixel_pages[LANG_MAX][6] = {
    {"","Red","Green","Blue","White","Black"},
    {"","Rouge","Vert","Bleu","Blanc","Noir"},
    {"","Rosso","Verde","Blu","Bianco","Nero"},
    {"","Rojo","Verde","Azul","Blanco","Negro"},
    {"","Rot","Gruen","Blau","Weiss","Schwarz"},
    {"","Vermelho","Verde","Azul","Branco","Preto"},
    {"","Rood","Groen","Blauw","Wit","Zwart"},
    {"","Red","Green","Blue","White","Black"},
    {"","Red","Green","Blue","White","Black"},
    {"","Red","Green","Blue","White","Black"},
    {"","Red","Green","Blue","White","Black"}};
static const char *s_dead_pixel_fmt[LANG_MAX] = {
    "Dead-Pixel: %s  |  [A] Next  [B] Exit",
    "Pixel mort: %s  |  [A] Suivant  [B] Quitter",
    "Pixel morto: %s  |  [A] Successivo  [B] Esci",
    "Pixel muerto: %s  |  [A] Siguiente  [B] Salir",
    "Dead-Pixel: %s  |  [A] Weiter  [B] Beenden",
    "Pixel morto: %s  |  [A] Proximo  [B] Sair",
    "Dode pixel: %s  |  [A] Volgende  [B] Afsluiten",
    "Dead-Pixel: %s  |  [A] Next  [B] Exit",
    "Dead-Pixel: %s  |  [A] Next  [B] Exit",
    "Dead-Pixel: %s  |  [A] Next  [B] Exit",
    "Dead-Pixel: %s  |  [A] Next  [B] Exit"};

// --- Exit hint ---
static const char *s_exit_hint[LANG_MAX] = {
    "[+] Exit", "[+] Quitter", "[+] Esci", "[+] Salir",
    "[+] Beenden", "[+] Sair", "[+] Afsluiten",
    "[+] Exit",
    "[+] Exit",
    "[+] Exit",
    "[+] Exit"};

// --- Export ---
static const char *s_export_error[LANG_MAX] = {
    "Error: Cannot write to SD card", "Erreur: Impossible d'ecrire sur la carte SD", "Errore: Impossibile scrivere su SD", "Error: No se puede escribir en la tarjeta SD",
    "Fehler: Kann nicht auf SD-Karte schreiben", "Erro: Nao e possivel escrever no cartao SD", "Fout: Kan niet naar SD-kaart schrijven",
    "Error: Cannot write to SD card",
    "Error: Cannot write to SD card",
    "Error: Cannot write to SD card",
    "Error: Cannot write to SD card"};
static const char *s_export_saved[LANG_MAX] = {
    "Report saved to sdmc:/switch/SwitchInfoNX/system_report.txt", "Rapport sauvegarde dans sdmc:/switch/SwitchInfoNX/system_report.txt", "Report salvato in sdmc:/switch/SwitchInfoNX/system_report.txt", "Informe guardado en sdmc:/switch/SwitchInfoNX/system_report.txt",
    "Bericht gespeichert unter sdmc:/switch/SwitchInfoNX/system_report.txt", "Relatorio salvo em sdmc:/switch/SwitchInfoNX/system_report.txt", "Rapport opgeslagen naar sdmc:/switch/SwitchInfoNX/system_report.txt",
    "Report saved to sdmc:/switch/SwitchInfoNX/system_report.txt",
    "Report saved to sdmc:/switch/SwitchInfoNX/system_report.txt",
    "Report saved to sdmc:/switch/SwitchInfoNX/system_report.txt",
    "Report saved to sdmc:/switch/SwitchInfoNX/system_report.txt"};

// --- File Browser ---
static const char *s_file_browser[LANG_MAX] = {
    "File Browser", "Explorateur", "Esplora file", "Explorador",
    "Dateibrowser", "Explorador", "Bestandsverkenner",
    "File Browser",
    "File Browser",
    "File Browser",
    "File Browser"};
static const char *s_name_col[LANG_MAX] = {
    "Name", "Nom", "Nome", "Nombre",
    "Name", "Nome", "Naam",
    "Name",
    "Name",
    "Name",
    "Name"};
static const char *s_size_col[LANG_MAX] = {
    "Size", "Taille", "Dimensione", "Tamahho",
    "Groesse", "Tamanho", "Grootte",
    "Size",
    "Size",
    "Size",
    "Size"};
static const char *s_size_sort_names[LANG_MAX][3] = {
    {"TAP TO SORT", "LARGEST FIRST", "SMALLEST FIRST"},
    {"TOUCHER POUR TRIER", "PLUS GROS D'ABORD", "PLUS PETIT D'ABORD"},
    {"TOCCA PER ORDINARE", "PIU GRANDE PRIMA", "PIU PICCOLO PRIMA"},
    {"TOCAR PARA ORDENAR", "MAYOR PRIMERO", "MENOR PRIMERO"},
    {"ZUM SORTIEREN", "GROESSTE ZUERST", "KLEINSTE ZUERST"},
    {"TOQUE PARA ORDENAR", "MAIOR PRIMEIRO", "MENOR PRIMEIRO"},
    {"TIK OM TE SORTEREN", "GROOTSTE EERST", "KLEINSTE EERST"},
    {"タップで並べ替え", "大きい順", "小さい順"},
    {"НАЖМИТЕ ДЛЯ СОРТИРОВКИ", "СНАЧАЛА БОЛЬШИЕ", "СНАЧАЛА МАЛЕНЬКИЕ"},
    {"点击排序", "从大到小", "从小到大"},
    {"탭하여 정렬", "큰 항목 우선", "작은 항목 우선"}};
static const char *s_dir_tag[LANG_MAX] = {
    "<DIR>", "<DOS>", "<CART>", "<DIR>",
    "<VERZ>", "<DIR>", "<MAP>",
    "<DIR>",
    "<DIR>",
    "<DIR>",
    "<DIR>"};
static const char *s_fb_delete_q[LANG_MAX] = {
    "Delete selected item?", "Supprimer l'element selectionne?", "Eliminare l'elemento selezionato?", "Eliminar elemento seleccionado?",
    "Ausgewaehltes Element loeschen?", "Excluir item selecionado?", "Geselecteerd item verwijderen?",
    "Delete selected item?",
    "Delete selected item?",
    "Delete selected item?",
    "Delete selected item?"};
static const char *s_fb_confirm_del[LANG_MAX] = {
    "[A] Confirm Delete  [B] Cancel", "[A] Confirmer suppr.  [B] Annuler", "[A] Conferma elim.  [B] Annulla", "[A] Conf. eliminar  [B] Cancelar",
    "[A] Loeschen best.  [B] Abbrechen", "[A] Conf. exclusao  [B] Cancelar", "[A] Bevestig verwij.  [B] Annuleren",
    "[A] Confirm Delete  [B] Cancel",
    "[A] Confirm Delete  [B] Cancel",
    "[A] Confirm Delete  [B] Cancel",
    "[A] Confirm Delete  [B] Cancel"};
static const char *s_fb_delete_btn[LANG_MAX] = {
    "Delete", "Supprimer", "Elimina", "Eliminar", "Loeschen", "Excluir", "Verwijderen",
    "削除", "Удалить", "删除", "삭제"};
static const char *s_fb_rename_btn[LANG_MAX] = {
    "Rename", "Renommer", "Rinomina", "Renombrar", "Umbenennen", "Renomear", "Hernoemen",
    "名前変更", "Переименовать", "重命名", "이름 변경"};
static const char *s_fb_browse_hint[LANG_MAX] = {
    "[A] Enter  [B] Back  [X] Delete  [Y] Rename",
    "[A] Entrer  [B] Retour  [X] Suppr.  [Y] Renommer",
    "[A] Entra  [B] Indietro  [X] Elim.  [Y] Rinomina",
    "[A] Entrar  [B] Volver  [X] Elim.  [Y] Renombrar",
    "[A] Oeffnen  [B] Zuru.  [X] Loesch.  [Y] Umben.",
    "[A] Entrar  [B] Voltar  [X] Excluir  [Y] Renomear",
    "[A] Open  [B] Terug  [X] Verwijd.  [Y] Hernoem",
    "[A] Enter  [B] Back  [X] Delete  [Y] Rename",
    "[A] Enter  [B] Back  [X] Delete  [Y] Rename",
    "[A] Enter  [B] Back  [X] Delete  [Y] Rename",
    "[A] Enter  [B] Back  [X] Delete  [Y] Rename"};
static const char *s_fb_paste[LANG_MAX] = {
    "[L] Paste", "[L] Coller", "[L] Incolla", "[L] Pegar",
    "[L] Einfuegen", "[L] Colar", "[L] Plakken",
    "[L] Paste",
    "[L] Paste",
    "[L] Paste",
    "[L] Paste"};
static const char *s_fb_other_hint[LANG_MAX] = {
    "[R] Copy  [ZL] Cut  [-] Home", "[R] Copier  [ZL] Couper  [-] Accueil", "[R] Copia  [ZL] Taglia  [-] Home", "[R] Copiar  [ZL] Cortar  [-] Inicio",
    "[R] Kopieren  [ZL] Aussch.  [-] Start", "[R] Copiar  [ZL] Recortar  [-] Inicio", "[R] Kopieer  [ZL] Knip  [-] Home",
    "[R] Copy  [ZL] Cut  [-] Home",
    "[R] Copy  [ZL] Cut  [-] Home",
    "[R] Copy  [ZL] Cut  [-] Home",
    "[R] Copy  [ZL] Cut  [-] Home"};
static const char *s_fb_rename_hint[LANG_MAX] = {
    "[Up/Dn] cycle char  [L/R] cursor  [ZL] del char  [A] Confirm  [B] Cancel",
    "[Haut/Bas] car. suiv./prec.  [G/D] curseur  [ZL] suppr. car.  [A] Conf.  [B] Annul.",
    "[Su/Giu] ciclo char  [S/D] cursore  [ZL] elim. char  [A] Conf.  [B] Annul.",
    "[Arr/Ab] caract. sig./ant.  [I/D] cursor  [ZL] elim. car.  [A] Conf.  [B] Canc.",
    "[Hoch/Runt] Zeichen  [L/R] Cursor  [ZL] loesch.  [A] Best.  [B] Abbr.",
    "[Cima/Baixo] ciclo char  [E/D] cursor  [ZL] elim. char  [A] Conf.  [B] Canc.",
    "[Omh/Oml] teken cyc.  [L/R] cursor  [ZL] del teken  [A] Bevest.  [B] Annul.",
    "[Up/Dn] cycle char  [L/R] cursor  [ZL] del char  [A] Confirm  [B] Cancel",
    "[Up/Dn] cycle char  [L/R] cursor  [ZL] del char  [A] Confirm  [B] Cancel",
    "[Up/Dn] cycle char  [L/R] cursor  [ZL] del char  [A] Confirm  [B] Cancel",
    "[Up/Dn] cycle char  [L/R] cursor  [ZL] del char  [A] Confirm  [B] Cancel"};
static const char *s_fb_rename_empty[LANG_MAX] = {
    "(empty)", "(vide)", "(vuoto)", "(vacio)",
    "(leer)", "(vazio)", "(leeg)",
    "(empty)",
    "(empty)",
    "(empty)",
    "(empty)"};

// --- Popup dismiss ---
static const char *s_popup_dismiss[LANG_MAX] = {
    "Tap anywhere to dismiss", "Touchez pour fermer", "Tocca per chiudere", "Toca para cerrar",
    "Zum Schliessen tippen", "Toque para fechar", "Tik om te sluiten",
    "Tap anywhere to dismiss",
    "Tap anywhere to dismiss",
    "Tap anywhere to dismiss",
    "Tap anywhere to dismiss"};

// --- Settings info (replaces original multi-line) ---
static const char *s_settings_info2[LANG_MAX] = {
    "[DPad] Switch item  |  [L/R] Change value  |  [B] Back",
    "[DPad] Changer item  |  [L/G] Changer valeur  |  [B] Retour",
    "[DPad] Cambia item  |  [L/S] Cambia valore  |  [B] Indietro",
    "[DPad] Cambiar item  |  [I/D] Cambiar valor  |  [B] Volver",
    "[DPad] Wechseln  |  [L/R] Wert aendern  |  [B] Zurueck",
    "[DPad] Mudar item  |  [L/E] Mudar valor  |  [B] Voltar",
    "[DPad] Wissele  |  [L/R] Waarde wijz.  |  [B] Terug",
    "[十字キー] 項目選択 | [L/R] 値変更 | [B] 戻る",
    "[Крестовина] выбор | [L/R] изменить | [B] назад",
    "[方向键] 选择项目 | [L/R] 更改数值 | [B] 返回",
    "[방향키] 항목 선택 | [L/R] 값 변경 | [B] 뒤로"};

// --- Settings refresh names ---
static const char *s_refresh_names[LANG_MAX][5] = {
    {"Off", "1s", "3s", "5s", "10s"},
    {"Arret", "1s", "3s", "5s", "10s"},
    {"Spento", "1s", "3s", "5s", "10s"},
    {"Apag.", "1s", "3s", "5s", "10s"},
    {"Aus", "1s", "3s", "5s", "10s"},
    {"Desl.", "1s", "3s", "5s", "10s"},
    {"Uit", "1s", "3s", "5s", "10s"},
    {"Off", "1s", "3s", "5s", "10s"},
    {"Off", "1s", "3s", "5s", "10s"},
    {"Off", "1s", "3s", "5s", "10s"},
    {"Off", "1s", "3s", "5s", "10s"}};

// --- Custom Theme Editor ---
static const char *s_cte_title[LANG_MAX] = {
    "Custom Theme Editor", "Editeur de theme", "Editor tema personalizzato", "Editor de tema",
    "Benutzerdefinierter Theme-Editor", "Editor de tema personalizado", "Aangepaste thema-editor",
    "カスタムテーマエディター",
    "Редактор пользовательской темы",
    "自定义主题编辑器",
    "사용자 지정 테마 편집기"};
static const char *s_cte_names[LANG_MAX][10] = {
    {"Background","Card background","Border","Accent cyan","Accent green","Text","Grey","Dark grey","Header background","Tab background"},
    {"Fond","Fond de carte","Bordure","Accent cyan","Accent vert","Texte","Gris","Gris fonce","Fond d'en-tete","Fond d'onglet"},
    {"Sfondo","Sfondo scheda","Bordo","Accento ciano","Accento verde","Testo","Grigio","Grigio scuro","Sfondo intestazione","Sfondo scheda"},
    {"Fondo","Fondo de tarjeta","Borde","Acento cian","Acento verde","Texto","Gris","Gris oscuro","Fondo de cabecera","Fondo de pestana"},
    {"Hintergrund","Kartenhintergrund","Rahmen","Akzent Cyan","Akzentgruen","Text","Grau","Dunkelgrau","Kopfhintergrund","Registerhintergrund"},
    {"Fundo","Fundo do cartao","Borda","Destaque ciano","Destaque verde","Texto","Cinza","Cinza escuro","Fundo do cabecalho","Fundo da guia"},
    {"Achtergrond","Kaartachtergrond","Rand","Cyaanaccent","Groenaccent","Tekst","Grijs","Donkergrijs","Kopachtergrond","Tabachtergrond"},
    {"背景","カード背景","枠線","シアンのアクセント","緑のアクセント","文字","グレー","濃いグレー","ヘッダー背景","タブ背景"},
    {"Фон","Фон карточки","Рамка","Акцент: голубой","Акцент: зелёный","Текст","Серый","Тёмно-серый","Фон заголовка","Фон вкладки"},
    {"背景","卡片背景","边框","青色强调","绿色强调","文字","灰色","深灰色","标题栏背景","标签栏背景"},
    {"배경","카드 배경","테두리","청록 강조색","녹색 강조색","텍스트","회색","진회색","헤더 배경","탭 배경"}
};
static const char *s_cte_edit_hint[LANG_MAX] = {
    "Selected Color - [L/R] Channel  [Up/Down/ZL/ZR] Value  [Touch] Drag",
    "Couleur - [L/G] Canal  [Haut/Bas/ZL/ZR] Valeur  [Touch] Glisser",
    "Colore - [L/S] Canale  [Su/Giu/ZL/ZR] Valore  [Touch] Trascina",
    "Color - [I/D] Canal  [Arr/Ab/ZL/ZR] Valor  [Touch] Arrastrar",
    "Farbe - [L/R] Kanal  [Hoch/Runt/ZL/ZR] Wert  [Touch] Ziehen",
    "Cor - [L/E] Canal  [Cima/Baixo/ZL/ZR] Valor  [Touch] Arrastar",
    "Kleur - [L/R] Kanaal  [Omh/Oml/ZL/ZR] Waarde  [Touch] Sleep",
    "選択色：[L/R] チャンネル [上下/ZL/ZR] 値 [タッチ] 操作",
    "Цвет: [L/R] канал [Вверх/Вниз/ZL/ZR] значение [касание]",
    "当前颜色：[L/R] 通道 [上下/ZL/ZR] 调整 [触摸] 滑动",
    "선택 색상: [L/R] 채널 [위/아래/ZL/ZR] 값 [터치] 조절"};
static const char *s_cte_save[LANG_MAX] = {
    "[A] Save & Apply", "[A] Sauver & Appliquer", "[A] Salva & Applica", "[A] Guardar & Aplicar",
    "[A] Speichern & Anwenden", "[A] Salvar & Aplicar", "[A] Opslaan & Toepassen",
    "[A] 保存并应用",
    "[A] Сохранить и применить",
    "[A] 保存并应用",
    "[A] 저장 및 적용"};
static const char *s_cte_cancel[LANG_MAX] = {
    "[B] Cancel", "[B] Annuler", "[B] Annulla", "[B] Cancelar",
    "[B] Abbrechen", "[B] Cancelar", "[B] Annuleren",
    "[B] キャンセル",
    "[B] Отмена",
    "[B] 取消",
    "[B] 취소"};
static const char *s_cte_touch_hint[LANG_MAX] = {
    "Touch color slot to select | Touch RGB bar to set value | A=Save  B=Cancel",
    "Toucher couleur pour selectionner | Toucher barre RVB | A=Sauv.  B=Annul.",
    "Tocca colore per selezionare | Tocca barra RGB per valore | A=Salva  B=Annulla",
    "Tocar color para seleccionar | Tocar barra RGB | A=Guardar  B=Cancelar",
    "Farbe antippen zum Ausw. | RGB-Balken antippen | A=Speich.  B=Abbr.",
    "Tocar cor para selecionar | Tocar barra RGB | A=Salvar  B=Cancelar",
    "Raak kleur aan om te selecteren | Raak RGB-balk | A=Opslaan  B=Annul.",
    "色をタッチして選択 | RGBバーを操作 | A=保存 B=取消",
    "Коснитесь цвета или полосы RGB | A=сохранить B=отмена",
    "触摸色块选择 | 滑动 RGB 色条 | A=保存 B=取消",
    "색상 선택 및 RGB 막대 조절 | A=저장 B=취소"};

// --- Existing i18n strings ---

// Theme
#define THEME_DARK 0
#define THEME_LIGHT 1
#define THEME_BLUE 2
#define THEME_GREEN 3
#define THEME_PURPLE 4
#define THEME_RED 5
#define THEME_PINK 6
#define THEME_ORANGE 7
#define THEME_AMBER 8
#define THEME_TEAL 9
#define THEME_CYAN2 10
#define THEME_MINT 11
#define THEME_CORAL 12
#define THEME_LAVENDER 13
#define THEME_MIDNIGHT 14
#define THEME_OLED 15
#define THEME_CYBERPUNK 16
#define THEME_CUSTOM 17
#define THEME_MAX 18
static int cur_theme = THEME_DARK;
static int auto_refresh_interval = 3; // seconds, 0 = off

static const char *theme_names[THEME_MAX] = {
    "Dark", "Light", "Blue", "Green", "Purple", "Red", "Pink", "Orange", "Amber", "Teal", "Cyan", "Mint", "Coral", "Lavender", "Midnight", "OLED", "Cyberpunk", "Custom"
};

static int settings_sel = 0; // 0=language, 1=theme, 2=auto-refresh
static int ui_cursor_x = W / 2;
static int ui_cursor_y = H / 2;
static bool tutorial_completed = false;
static bool tutorial_active = false;
static bool tutorial_write_failed = false;
static int tutorial_step = 0;
static bool tesla_guide_active = false;
static int tesla_guide_step = 0;
static int save_backup_result = 0;

// Custom theme editable colors
static SDL_Color custom_bg       = {18,18,20,255};
static SDL_Color custom_card     = {28,28,32,255};
static SDL_Color custom_border   = {42,42,48,255};
static SDL_Color custom_cyan     = {0,210,255,255};
static SDL_Color custom_green    = {0,255,136,255};
static SDL_Color custom_white    = {255,255,255,255};
static SDL_Color custom_grey     = {150,150,160,255};
static SDL_Color custom_dark_grey = {55,55,62,255};
static SDL_Color custom_bg2       = {22,22,26,255};
static SDL_Color custom_bg3       = {34,34,42,255};
static int custom_theme_editing = 0; // 1 when custom editor is open
static int cte_sel = 0; // which color slot is selected in editor (0-9)
static int cte_chan = 0; // 0=R, 1=G, 2=B
static SDL_Color cte_backup[10]; // backup for cancel

// Theme color palettes [theme][color_index]
static const SDL_Color theme_bg[THEME_MAX]       = { {18,18,20,255}, {235,235,240,255}, {10,18,35,255}, {10,28,12,255}, {28,10,35,255}, {35,10,10,255}, {40,15,25,255}, {30,18,8,255}, {30,24,8,255}, {8,30,28,255}, {8,25,35,255}, {8,35,25,255}, {40,18,15,255}, {25,15,40,255}, {5,5,15,255}, {0,0,0,255}, {14,4,30,255}, {18,18,20,255} };
static const SDL_Color theme_card[THEME_MAX]     = { {28,28,32,255}, {215,215,222,255}, {18,30,52,255}, {18,40,22,255}, {40,18,52,255}, {52,18,18,255}, {55,20,30,255}, {45,28,14,255}, {45,34,14,255}, {14,42,38,255}, {14,35,50,255}, {14,48,35,255}, {55,25,22,255}, {38,22,55,255}, {10,10,25,255}, {14,14,16,255}, {28,10,50,255}, {28,28,32,255} };
static const SDL_Color theme_border[THEME_MAX]   = { {42,42,48,255}, {190,190,200,255}, {30,45,70,255}, {30,55,35,255}, {55,30,70,255}, {70,30,30,255}, {75,35,45,255}, {65,42,24,255}, {65,48,24,255}, {24,55,50,255}, {24,48,65,255}, {24,65,48,255}, {75,38,35,255}, {55,35,75,255}, {18,18,38,255}, {32,32,38,255}, {244,0,118,255}, {42,42,48,255} };
static const SDL_Color theme_cyan[THEME_MAX]     = { {0,210,255,255}, {0,120,180,255}, {0,200,255,255}, {0,220,200,255}, {100,200,255,255}, {255,100,100,255}, {255,120,180,255}, {255,180,80,255}, {255,210,80,255}, {0,220,200,255}, {0,210,255,255}, {0,255,180,255}, {255,140,100,255}, {180,140,255,255}, {80,120,255,255}, {0,220,255,255}, {0,240,255,255}, {0,210,255,255} };
static const SDL_Color theme_green[THEME_MAX]    = { {0,255,136,255}, {0,180,80,255}, {0,240,140,255}, {0,255,136,255}, {100,255,180,255}, {255,100,100,255}, {255,120,180,255}, {255,180,80,255}, {255,210,80,255}, {0,255,200,255}, {100,255,220,255}, {0,255,200,255}, {255,180,140,255}, {200,180,255,255}, {100,180,255,255}, {0,255,136,255}, {0,255,160,255}, {0,255,136,255} };
static const SDL_Color theme_white[THEME_MAX]    = { {255,255,255,255}, {20,20,25,255}, {220,230,255,255}, {200,255,210,255}, {230,210,255,255}, {255,210,210,255}, {255,210,220,255}, {255,230,200,255}, {255,240,200,255}, {200,240,235,255}, {200,230,240,255}, {200,255,230,255}, {255,230,220,255}, {230,220,255,255}, {200,210,240,255}, {255,255,255,255}, {255,245,255,255}, {255,255,255,255} };
static const SDL_Color theme_grey[THEME_MAX]     = { {150,150,160,255}, {100,100,110,255}, {130,140,160,255}, {130,160,140,255}, {160,140,170,255}, {170,130,130,255}, {170,130,140,255}, {170,150,130,255}, {170,160,130,255}, {130,160,155,255}, {130,150,160,255}, {130,170,155,255}, {170,145,140,255}, {155,140,170,255}, {120,130,155,255}, {140,140,145,255}, {180,140,195,255}, {150,150,160,255} };
static const SDL_Color theme_dark_grey[THEME_MAX] = { {55,55,62,255}, {170,170,178,255}, {45,55,75,255}, {45,65,50,255}, {65,45,75,255}, {75,45,45,255}, {75,48,55,255}, {70,55,40,255}, {70,60,40,255}, {40,65,60,255}, {40,55,70,255}, {40,70,60,255}, {75,55,50,255}, {60,50,75,255}, {30,35,55,255}, {40,40,46,255}, {70,30,85,255}, {55,55,62,255} };
static const SDL_Color theme_bg2[THEME_MAX]      = { {22,22,26,255}, {225,225,232,255}, {14,22,39,255}, {14,32,16,255}, {32,14,39,255}, {39,14,14,255}, {44,18,28,255}, {34,22,12,255}, {34,28,12,255}, {12,34,32,255}, {12,28,39,255}, {12,39,30,255}, {44,22,18,255}, {28,18,44,255}, {8,8,22,255}, {8,8,10,255}, {20,6,40,255}, {22,22,26,255} };
static const SDL_Color theme_bg3[THEME_MAX]      = { {34,34,42,255}, {195,195,202,255}, {22,34,56,255}, {22,44,26,255}, {44,22,56,255}, {56,22,22,255}, {60,25,35,255}, {50,32,18,255}, {50,38,18,255}, {18,48,44,255}, {18,40,56,255}, {18,52,42,255}, {60,32,28,255}, {40,28,60,255}, {14,14,32,255}, {20,20,24,255}, {40,16,70,255}, {34,34,42,255} };

// ─── CPU/GPU Frequency Scaling targets (declared before save/load_config) ───
static int cpu_scale_target_mhz = 0; // 0 = auto/default
static int gpu_scale_target_mhz = 0;
static int mem_scale_target_mhz = 0;

static void apply_theme(void) {
    if (cur_theme == THEME_CUSTOM) {
        color_bg        = custom_bg;
        color_card      = custom_card;
        color_card_border = custom_border;
        color_cyan      = custom_cyan;
        color_green     = custom_green;
        color_white     = custom_white;
        color_grey      = custom_grey;
        color_dark_grey = custom_dark_grey;
        color_bg2       = custom_bg2;
        color_bg3       = custom_bg3;
    } else {
        color_bg        = theme_bg[cur_theme];
        color_card      = theme_card[cur_theme];
        color_card_border = theme_border[cur_theme];
        color_cyan      = theme_cyan[cur_theme];
        color_green     = theme_green[cur_theme];
        color_white     = theme_white[cur_theme];
        color_grey      = theme_grey[cur_theme];
        color_dark_grey = theme_dark_grey[cur_theme];
        color_bg2       = theme_bg2[cur_theme];
        color_bg3       = theme_bg3[cur_theme];
    }
    // red, yellow, purple, orange stay the same
}

static void apply_custom_theme_palette(int palette_index) {
    static const SDL_Color palettes[3][10] = {
        {
            {8, 20, 36, 255}, {16, 36, 60, 255}, {32, 90, 130, 255},
            {0, 195, 255, 255}, {80, 255, 190, 255}, {238, 248, 255, 255},
            {145, 175, 198, 255}, {40, 65, 88, 255}, {12, 27, 46, 255},
            {24, 48, 74, 255}
        },
        {
            {32, 12, 22, 255}, {58, 22, 34, 255}, {128, 48, 56, 255},
            {255, 125, 70, 255}, {255, 205, 100, 255}, {255, 242, 228, 255},
            {205, 160, 150, 255}, {88, 48, 49, 255}, {42, 16, 28, 255},
            {74, 30, 40, 255}
        },
        {
            {9, 24, 18, 255}, {18, 43, 30, 255}, {42, 100, 68, 255},
            {92, 230, 150, 255}, {185, 245, 110, 255}, {240, 250, 230, 255},
            {155, 190, 155, 255}, {44, 72, 50, 255}, {13, 31, 22, 255},
            {26, 53, 34, 255}
        }
    };
    if (palette_index < 0 || palette_index >= 3) return;
    SDL_Color *colors[10] = {
        &custom_bg, &custom_card, &custom_border, &custom_cyan, &custom_green,
        &custom_white, &custom_grey, &custom_dark_grey, &custom_bg2, &custom_bg3
    };
    for (int i = 0; i < 10; i++) *colors[i] = palettes[palette_index][i];
    apply_theme();
}

// Config save/load
#define CONFIG_PATH "sdmc:/switch/SwitchInfoNX/config.txt"
#define TUTORIAL_PATH "sdmc:/switch/SwitchInfoNX/tutorial.json"
static void save_config(void) {
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "language=%d\n", cur_lang);
    fprintf(f, "theme=%d\n", cur_theme);
    fprintf(f, "autorefresh=%d\n", auto_refresh_interval);
    fprintf(f, "custom_bg=%d,%d,%d\n", custom_bg.r, custom_bg.g, custom_bg.b);
    fprintf(f, "custom_card=%d,%d,%d\n", custom_card.r, custom_card.g, custom_card.b);
    fprintf(f, "custom_border=%d,%d,%d\n", custom_border.r, custom_border.g, custom_border.b);
    fprintf(f, "custom_cyan=%d,%d,%d\n", custom_cyan.r, custom_cyan.g, custom_cyan.b);
    fprintf(f, "custom_green=%d,%d,%d\n", custom_green.r, custom_green.g, custom_green.b);
    fprintf(f, "custom_white=%d,%d,%d\n", custom_white.r, custom_white.g, custom_white.b);
    fprintf(f, "custom_grey=%d,%d,%d\n", custom_grey.r, custom_grey.g, custom_grey.b);
    fprintf(f, "custom_dark_grey=%d,%d,%d\n", custom_dark_grey.r, custom_dark_grey.g, custom_dark_grey.b);
    fprintf(f, "custom_bg2=%d,%d,%d\n", custom_bg2.r, custom_bg2.g, custom_bg2.b);
    fprintf(f, "custom_bg3=%d,%d,%d\n", custom_bg3.r, custom_bg3.g, custom_bg3.b);
    fprintf(f, "temp_alert_millic=%d\n", temp_alert_millic);
    fprintf(f, "mem_alert_frac=%f\n", mem_alert_frac);
    fprintf(f, "cpu_scale_target=%d\n", cpu_scale_target_mhz);
    fprintf(f, "gpu_scale_target=%d\n", gpu_scale_target_mhz);
    fclose(f);
}
static void load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    int lang = cur_lang, theme = cur_theme;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "language=%d", &lang) == 1 && lang >= 0 && lang < LANG_MAX) cur_lang = lang;
        if (sscanf(line, "theme=%d", &theme) == 1 && theme >= 0 && theme < THEME_MAX) cur_theme = theme;
        if (sscanf(line, "autorefresh=%d", &auto_refresh_interval) == 1) {
            if (auto_refresh_interval != 0 && auto_refresh_interval != 1 && auto_refresh_interval != 3 && auto_refresh_interval != 5 && auto_refresh_interval != 10)
                auto_refresh_interval = 3;
        }
        int r,g,b;
        if (sscanf(line, "custom_bg=%d,%d,%d", &r,&g,&b)==3) {custom_bg.r=r;custom_bg.g=g;custom_bg.b=b;}
        if (sscanf(line, "custom_card=%d,%d,%d", &r,&g,&b)==3) {custom_card.r=r;custom_card.g=g;custom_card.b=b;}
        if (sscanf(line, "custom_border=%d,%d,%d", &r,&g,&b)==3) {custom_border.r=r;custom_border.g=g;custom_border.b=b;}
        if (sscanf(line, "custom_cyan=%d,%d,%d", &r,&g,&b)==3) {custom_cyan.r=r;custom_cyan.g=g;custom_cyan.b=b;}
        if (sscanf(line, "custom_green=%d,%d,%d", &r,&g,&b)==3) {custom_green.r=r;custom_green.g=g;custom_green.b=b;}
        if (sscanf(line, "custom_white=%d,%d,%d", &r,&g,&b)==3) {custom_white.r=r;custom_white.g=g;custom_white.b=b;}
        if (sscanf(line, "custom_grey=%d,%d,%d", &r,&g,&b)==3) {custom_grey.r=r;custom_grey.g=g;custom_grey.b=b;}
        if (sscanf(line, "custom_dark_grey=%d,%d,%d", &r,&g,&b)==3) {custom_dark_grey.r=r;custom_dark_grey.g=g;custom_dark_grey.b=b;}
        if (sscanf(line, "custom_bg2=%d,%d,%d", &r,&g,&b)==3) {custom_bg2.r=r;custom_bg2.g=g;custom_bg2.b=b;}
        if (sscanf(line, "custom_bg3=%d,%d,%d", &r,&g,&b)==3) {custom_bg3.r=r;custom_bg3.g=g;custom_bg3.b=b;}
        if (sscanf(line, "temp_alert_millic=%d", &r) == 1) { temp_alert_millic = r; }
        float ff;
        if (sscanf(line, "mem_alert_frac=%f", &ff) == 1) { mem_alert_frac = ff; }
        int st;
        if (sscanf(line, "cpu_scale_target=%d", &st) == 1) cpu_scale_target_mhz = st;
        if (sscanf(line, "gpu_scale_target=%d", &st) == 1) gpu_scale_target_mhz = st;
    }
    fclose(f);
    apply_theme();
}

static bool tutorial_load_completed(bool *state_found) {
    *state_found = false;
    FILE *f = fopen(TUTORIAL_PATH, "r");
    if (!f) return false;
    char json[128] = {0};
    size_t length = fread(json, 1, sizeof(json) - 1, f);
    bool read_ok = !ferror(f);
    fclose(f);
    if (!read_ok || length == 0) return false;

    char *value = strstr(json, "\"tuto\"");
    if (!value) return false;
    value = strchr(value, ':');
    if (!value) return false;
    value++;
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
    if (strncmp(value, "true", 4) == 0) {
        *state_found = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0) *state_found = true;
    return false;
}

static bool tutorial_save_state(bool completed) {
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    FILE *f = fopen(TUTORIAL_PATH, "w");
    if (!f) return false;
    bool write_ok = fprintf(f, "{\n  \"tuto\": %s\n}\n",
        completed ? "true" : "false") > 0;
    if (fclose(f) != 0) write_ok = false;
    return write_ok;
}

static void tutorial_finish(void) {
    if (!tutorial_save_state(true)) {
        tutorial_write_failed = true;
        return;
    }
    tutorial_completed = true;
    tutorial_active = false;
    tutorial_write_failed = false;
}

static void tutorial_next(void) {
    if (tutorial_step < 4) tutorial_step++;
    else tutorial_finish();
}

// ─── Game Card Status ────────────────────────────
static bool gamecart_present = false;
static bool gamecart_status_available = false;
static u64 gamecart_last_check = 0;
static const char *s_gamecard_inserted[LANG_MAX] = {
    "Game card inserted", "Cartouche inseree", "Cartuccia inserita", "Cartucho insertado",
    "Spielkarte eingelegt", "Cartucho inserido", "Gamecard geplaatst",
    "Game card inserted", "Game card inserted", "Game card inserted", "Game card inserted"
};
static const char *s_no_gamecart[LANG_MAX] = {
    "No game card inserted", "Aucune cartouche inseree", "Nessuna cartuccia inserita", "Ningun cartucho insertado",
    "Kein Modul eingelegt", "Nenhum cartucho inserido", "Geen spelcartridge geplaatst",
    "No game card inserted", "No game card inserted", "No game card inserted", "No game card inserted"
};
static const char *s_gamecard_status_na[LANG_MAX] = {
    "Game card status unavailable", "Etat de la cartouche indisponible", "Stato cartuccia non disponibile",
    "Estado del cartucho no disponible", "Spielkartenstatus nicht verfuegbar",
    "Estado do cartucho indisponivel", "Gamecardstatus niet beschikbaar",
    "Game card status unavailable", "Game card status unavailable", "Game card status unavailable", "Game card status unavailable"
};

// ─── Battery Charge/Discharge Rate ────────────────────────
static float batt_rate_ma = 0.0f; // mA
static u64 batt_rate_tick = 0;
static u32 batt_prev_pct = 0;
static u64 batt_prev_tick = 0;
static bool batt_rate_initialized = false;
static bool batt_prev_charging = false;
static bool batt_diag_valid = false;
static u32 batt_milli_volt = 0;
static bool batt_is_charging = false;

// ─── Network Speed Test ────────────────────────
static float net_dl_mbps = 0.0f;
static float net_ul_mbps = 0.0f;
static float net_ping_ms = 0.0f;
static float net_jitter_ms = 0.0f;
static Thread net_test_thr;

// ─── Installed Games Scanner ────────────────────────
#define MAX_GAMES 64
#define VISIBLE_INSTALLED_GAMES 8
#define INSTALLED_GAME_ROW_HEIGHT 40
static u64 installed_game_ids[MAX_GAMES];
static char installed_game_names[MAX_GAMES][256];
static int installed_game_count = 0;
static bool installed_games_scanned = false;

// ─── Process Manager ────────────────────────
#define MAX_PROC 32
static u64 proc_ids[MAX_PROC];
static char proc_names[MAX_PROC][64];
static int proc_count = 0;
static u64 proc_scan_tick = 0;
static SDL_Texture *proc_icons[4] = {0};
static u64 proc_icon_cache_ids[4] = {0};
static SDL_Texture *installed_icons[VISIBLE_INSTALLED_GAMES] = {0};
static u64 installed_icon_cache_ids[VISIBLE_INSTALLED_GAMES] = {0};

static SDL_Texture *get_installed_game_icon(SDL_Renderer *renderer, int row, u64 program_id) {
    if (row < 0 || row >= VISIBLE_INSTALLED_GAMES || !program_id) return NULL;
    if (installed_icon_cache_ids[row] != program_id) {
        if (installed_icons[row]) SDL_DestroyTexture(installed_icons[row]);
        installed_icons[row] = NULL;
        installed_icon_cache_ids[row] = program_id;
        SDL_Surface *surface = game_icon_load(program_id);
        if (surface) {
            installed_icons[row] = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_FreeSurface(surface);
        }
    }
    return installed_icons[row];
}

static SDL_Texture *get_process_icon(SDL_Renderer *renderer, int row, u64 program_id) {
    if (row < 0 || row >= 4 || !program_id) return NULL;
    if (proc_icon_cache_ids[row] != program_id) {
        if (proc_icons[row]) SDL_DestroyTexture(proc_icons[row]);
        proc_icons[row] = NULL;
        proc_icon_cache_ids[row] = program_id;
        SDL_Surface *surface = game_icon_load(program_id);
        if (surface) {
            proc_icons[row] = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_FreeSurface(surface);
        }
    }
    return proc_icons[row];
}

static void scan_running_processes(void) {
    u64 now = armGetSystemTick();
    if (now - proc_scan_tick < armGetSystemTickFreq() * 2) return;
    proc_scan_tick = now;
    proc_count = 0;
    memset(proc_ids, 0, sizeof(proc_ids));
    memset(proc_names, 0, sizeof(proc_names));
    
    // Get running PIDs via kernel SVC
    s32 num = 0;
    u64 pids[32];
    Result rc = svcGetProcessList(&num, pids, 32);
    if (R_FAILED(rc)) { proc_count = -1; return; }
    
    for (s32 i = 0; i < num && proc_count < MAX_PROC; i++) {
        u64 program_id = 0;
        // Try to get the program ID for this process
        rc = svcGetInfo(&program_id, InfoType_ProgramId, pids[i], 0);
        if (R_SUCCEEDED(rc) && program_id > 0) {
            proc_ids[proc_count] = program_id;
            // Look up the name from installed games
            bool found = false;
            for (int j = 0; j < installed_game_count; j++) {
                if (installed_game_ids[j] == program_id) {
                    strncpy(proc_names[proc_count], installed_game_names[j], sizeof(proc_names[0])-1);
                    found = true; break;
                }
            }
            if (!found) {
                snprintf(proc_names[proc_count], sizeof(proc_names[0])-1,
                    "System [%08X%08X]",
                    (u32)(program_id >> 32), (u32)(program_id & 0xFFFFFFFF));
            }
            proc_count++;
        }
    }
}
// forward declaration
static void add_alert(const char *msg);

static bool read_clock_rate(PcvModuleId module_id, u32 *out_hz) {
    ClkrstSession session;
    Result rc = clkrstOpenSession(&session, module_id, 3);
    if (R_FAILED(rc)) {
        clock_last_error = rc;
        return false;
    }
    rc = clkrstGetClockRate(&session, out_hz);
    clkrstCloseSession(&session);
    clock_last_error = rc;
    return R_SUCCEEDED(rc);
}

static Result scale_set_clock(PcvModuleId module_id, int requested_mhz, int *applied_mhz) {
    if (requested_mhz <= 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    ClkrstSession session;
    Result rc = clkrstOpenSession(&session, module_id, 3);
    if (R_FAILED(rc)) {
        clock_last_error = rc;
        return rc;
    }

    u32 rates[32] = {0};
    s32 rate_count = 0;
    PcvClockRatesListType list_type = PcvClockRatesListType_Invalid;
    rc = clkrstGetPossibleClockRates(&session, rates, 32, &list_type, &rate_count);
    if (R_FAILED(rc) || rate_count <= 0 || rate_count > 32) {
        clkrstCloseSession(&session);
        clock_last_error = R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return clock_last_error;
    }

    u32 requested_hz = (u32)requested_mhz * 1000000u;
    u32 selected_hz = rates[0];
    if (list_type == PcvClockRatesListType_Discrete) {
        u64 best_diff = selected_hz > requested_hz
            ? (u64)selected_hz - requested_hz : (u64)requested_hz - selected_hz;
        for (s32 i = 1; i < rate_count; ++i) {
            u64 diff = rates[i] > requested_hz
                ? (u64)rates[i] - requested_hz : (u64)requested_hz - rates[i];
            if (diff < best_diff) {
                selected_hz = rates[i];
                best_diff = diff;
            }
        }
    } else if (list_type == PcvClockRatesListType_Range && rate_count >= 2) {
        u32 min_hz = rates[0] < rates[1] ? rates[0] : rates[1];
        u32 max_hz = rates[0] > rates[1] ? rates[0] : rates[1];
        selected_hz = requested_hz < min_hz ? min_hz :
            (requested_hz > max_hz ? max_hz : requested_hz);
    } else {
        clkrstCloseSession(&session);
        clock_last_error = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return clock_last_error;
    }

    rc = clkrstSetClockRate(&session, selected_hz);
    if (R_SUCCEEDED(rc) && applied_mhz)
        *applied_mhz = (int)(selected_hz / 1000000u);
    clkrstCloseSession(&session);
    clock_last_error = rc;
    return rc;
}

static void batt_rate_update(void);
static void update_battery_diagnostics(void);

static void report_perf_export(bool success) {
    perf_export_success = success;
    snprintf(perf_export_msg, sizeof(perf_export_msg), "%s",
        success ? s_perf_export_ok[cur_lang] : s_perf_export_fail[cur_lang]);
    perf_export_msg_tick = armGetSystemTick();
    add_alert(perf_export_msg);
}

// Export current performance metrics snapshot to CSV
static void save_perf_metrics(void) {
    batt_rate_update();
    update_battery_diagnostics();
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    const char *path = "sdmc:/switch/SwitchInfoNX/perf_metrics_v06.csv";
    FILE *f = fopen(path, "a+");
    if (!f) {
        report_perf_export(false);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        report_perf_export(false);
        return;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        report_perf_export(false);
        return;
    }
    bool write_ok = true;
    if (sz == 0) {
        write_ok = fprintf(f, "timestamp,local_time,fps,cpu_mhz,gpu_mhz,mem_mhz,skin_mC,mem_used_bytes,mem_total_bytes,batt_pct,batt_rate_mA,batt_mV,batt_charging,gamecart_present,cpu_target,gpu_target,net_dl_mbps,net_ul_mbps,installed_games\n") > 0;
    }

    // collect metrics
    time_t now = time(NULL);
    char timestr[64] = "unknown";
    struct tm *lt = localtime(&now);
    if (lt) strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", lt);

    // FPS
    int fps = current_fps;

    // clocks
    u32 cpu=0,gpu=0,memclk=0;
    read_clock_rate(PcvModuleId_CpuBus, &cpu);
    read_clock_rate(PcvModuleId_GPU, &gpu);
    read_clock_rate(PcvModuleId_EMC, &memclk);

    // skin temp
    s32 skin = 0;
    if (R_SUCCEEDED(tcInitialize())) { tcGetSkinTemperatureMilliC(&skin); tcExit(); }

    // memory usage
    u64 mem_total = 0, mem_used = 0;
    if (!(R_SUCCEEDED(svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
          R_SUCCEEDED(svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)))) {
        mem_total = 0; mem_used = 0;
    }

    u32 batt = 0;
    psmGetBatteryChargePercentage(&batt);
    float batt_rate = batt_rate_initialized ? batt_rate_ma : 0.0f;
    int row_len = fprintf(f, "%llu,%s,%d,%u,%u,%u,%d,%llu,%llu,%u,%.1f,%d,%d,%d,%d,%d,%.1f,%.1f,%d\n",
        (unsigned long long)now,
        timestr,
        fps,
        (unsigned)cpu/1000000u,
        (unsigned)gpu/1000000u,
        (unsigned)memclk/1000000u,
        (int)skin,
        (unsigned long long)mem_used,
        (unsigned long long)mem_total,
        batt,
        batt_rate,
        batt_diag_valid ? (int)batt_milli_volt : -1,
        batt_diag_valid ? (batt_is_charging ? 1 : 0) : -1,
        gamecart_status_available ? (gamecart_present ? 1 : 0) : -1,
        cpu_scale_target_mhz,
        gpu_scale_target_mhz,
        net_dl_mbps,
        net_ul_mbps,
        installed_game_count);
    if (row_len < 0 || ferror(f)) write_ok = false;
    if (fclose(f) != 0) write_ok = false;
    report_perf_export(write_ok);
}

static bool serial_looks_retail(const char *sn) {
    static const char *prefixes[] = { "XAW", "XAJ", "XKW", "XKJ", "XWW", "XWJ", "XWL", "XWR",
        "XTW", "XTJ", "XTS", "XTV", "HDH", "HDJ", "HEG", "HEJ", "HEC", "HED", "HFL", "HFT",
        "XJE", "XJS", "XJH", "XJK", "XJ700", "XJ710",
        "HAE", "HAD", "HAP", "PAT", NULL };
    for (int i = 0; prefixes[i]; i++) {
        if (strncmp(sn, prefixes[i], strlen(prefixes[i])) == 0) return true;
    }
    return sn[0] != 0;
}

static const char *detect_hardware_type(const char *sn) {
    if (strncmp(sn, "XAW", 3) == 0 || strncmp(sn, "XAJ", 3) == 0 || strncmp(sn, "XWW", 3) == 0 || strncmp(sn, "XWJ", 3) == 0)
        return "Switch V1 (Erista / HAC-001)";
    if (strncmp(sn, "XKW", 3) == 0 || strncmp(sn, "XKJ", 3) == 0)
        return "Switch V2 (Mariko / HAC-001-01)";
    if (strncmp(sn, "HDH", 3) == 0 || strncmp(sn, "HDJ", 3) == 0)
        return "Switch Lite (HDH-001)";
    if (strncmp(sn, "HEG", 3) == 0 || strncmp(sn, "HEJ", 3) == 0 || strncmp(sn, "HEC", 3) == 0 || strncmp(sn, "HED", 3) == 0)
        return "Switch OLED (HEG-001)";
    if (strncmp(sn, "HAE", 3) == 0)
        return "Dev Unit (SDEV)";
    if (strncmp(sn, "HFL", 3) == 0 || strncmp(sn, "HFT", 3) == 0)
        return "Switch Lite (HDH-001)";
    if (strncmp(sn, "XJE", 3) == 0 || strncmp(sn, "XJS", 3) == 0 || strncmp(sn, "XJH", 3) == 0 || strncmp(sn, "XJK", 3) == 0)
        return "Switch OLED (HEG-001)";
    return "Unknown Model";
}

static bool lbl_safe_on_device(void) {
    SetSysSerialNumber sn = {0};
    if (!R_SUCCEEDED(setsysGetSerialNumber(&sn)) || !serial_looks_retail(sn.number))
        return false;

    return true;
}

static void lbl_detect_environment(void) {
    lbl_emulator = !lbl_safe_on_device();
    lbl_ready = !lbl_emulator;
}

static Result brightness_read(float *out) {
    if (lbl_emulator) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result rc = lblInitialize();
    if (R_FAILED(rc)) return rc;
    rc = lblGetCurrentBrightnessSetting(out);
    lblExit();
    if (R_SUCCEEDED(rc)) lbl_ready = true;
    return rc;
}

static Result brightness_apply(float value) {
    if (lbl_emulator) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result rc = lblInitialize();
    if (R_FAILED(rc)) return rc;
    rc = lblSetCurrentBrightnessSetting(value);
    if (R_SUCCEEDED(rc))
        rc = lblApplyCurrentBrightnessSettingToBacklight();
    lblExit();
    return rc;
}

static bool lbl_auto_supported(void) {
    return lbl_ready && !lbl_emulator;
}

// ─── Unified Byte Formatter (v0.4 fix) ─────────────────────
static void format_bytes(u64 bytes, char *out, size_t out_len) {
    if (bytes >= 1073741824ULL) {
        snprintf(out, out_len, "%.2f GB", (double)bytes / 1073741824.0);
    } else if (bytes >= 1048576ULL) {
        snprintf(out, out_len, "%.2f MB", (double)bytes / 1048576.0);
    } else if (bytes >= 1024ULL) {
        snprintf(out, out_len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

// ─── EmuMMC Detection ───────────────────────────────────────
static const char *detect_emummc_str(void) {
    FILE *f = fopen("sdmc:/emuMMC/emummc.ini", "r");
    if (f) {
        char buf[256];
        bool enabled = false;
        while (fgets(buf, sizeof(buf), f)) {
            if (strstr(buf, "enabled = 1") || strstr(buf, "enabled=1")) {
                enabled = true;
                break;
            }
        }
        fclose(f);
        if (enabled) return cur_lang == 1 ? "EmuMMC (Active)" : "EmuMMC (Enabled)";
    }
    struct stat st;
    if (stat("sdmc:/emuMMC", &st) == 0 && S_ISDIR(st.st_mode)) {
        return cur_lang == 1 ? "EmuMMC (Detecte)" : "EmuMMC (Detected)";
    }
    return cur_lang == 1 ? "SysNAND (Interne)" : "SysNAND (Internal)";
}

// ─── Battery Health & Voltage Diagnostics (v0.4 feature) ───
static double batt_age_pct = -1.0;
static u64 batt_diag_last_tick = 0;
static const char *s_battery_diag_na[LANG_MAX] = {
    "Battery telemetry unavailable", "Donnees batterie indisponibles", "Dati batteria non disponibili",
    "Datos de bateria no disponibles", "Batteriedaten nicht verfuegbar",
    "Dados da bateria indisponiveis", "Batterijgegevens niet beschikbaar",
    "Battery telemetry unavailable", "Данные батареи недоступны", "电池遥测不可用", "배터리 정보를 사용할 수 없음"};

static void update_battery_diagnostics(void) {
    u64 now = armGetSystemTick();
    if (batt_diag_last_tick != 0 && now - batt_diag_last_tick < armGetSystemTickFreq() * 5) return;
    batt_diag_last_tick = now;

    double age = 0;
    if (R_SUCCEEDED(psmGetBatteryAgePercentage(&age))) {
        batt_age_pct = age;
    }
    PsmBatteryChargeInfoFields fields;
    memset(&fields, 0, sizeof(fields));
    if (R_SUCCEEDED(psmGetBatteryChargeInfoFields(&fields))) {
        batt_milli_volt = fields.battery_charge_milli_voltage;
        batt_diag_valid = batt_milli_volt > 0;
        batt_is_charging = fields.battery_charging;
    } else {
        batt_milli_volt = 0;
        batt_diag_valid = false;
    }
}

// ─── Modern Scrollbar Component (v0.4 design) ──────────────
static void draw_modern_scrollbar(SDL_Renderer *r, int top, int bottom, int current_scroll, int max_scroll) {
    if (max_scroll <= 0) return;
    int track_x = W - 14;
    int track_w = 6;
    int track_h = bottom - top;
    // Track background
    roundedBoxRGBA(r, track_x, top, track_x + track_w, bottom, 3, color_card_border.r, color_card_border.g, color_card_border.b, 90);
    // Thumb
    int thumb_h = (track_h * track_h) / (track_h + max_scroll);
    if (thumb_h < 26) thumb_h = 26;
    int thumb_y = top + (int)((float)current_scroll / (float)max_scroll * (float)(track_h - thumb_h));
    roundedBoxRGBA(r, track_x, thumb_y, track_x + track_w, thumb_y + thumb_h, 3, color_cyan.r, color_cyan.g, color_cyan.b, 220);
}

// ─── Drawing helpers ──────────────────────────────────────

static int ui_font_size_slot(TTF_Font *font) {
    if (font == font_xs) return 0;
    if (font == font_sm) return 1;
    if (font == font_md) return 2;
    return 3;
}

static Uint32 utf8_next_codepoint(const unsigned char **cursor) {
    const unsigned char *p = *cursor;
    Uint32 cp;
    if (*p < 0x80) { cp = *p++; }
    else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
    else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) { cp = ((p[0] & 7) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
    else { cp = *p++; }
    *cursor = p;
    return cp;
}

static TTF_Font *ui_font_for_codepoint(TTF_Font *primary, Uint32 cp) {
    if (TTF_GlyphIsProvided32(primary, cp)) return primary;
    int slot = ui_font_size_slot(primary);
    for (int family = 0; family < PlSharedFontType_Total; family++) {
        if (!ui_font_data_available[family]) continue;
        TTF_Font *candidate = ui_font_families[family][slot];
        if (!candidate) {
            SDL_RWops *rw = SDL_RWFromMem(ui_font_data[family].address, ui_font_data[family].size);
            candidate = rw ? TTF_OpenFontRW(rw, 1, ui_font_sizes[slot]) : NULL;
            ui_font_families[family][slot] = candidate;
        }
        if (candidate && TTF_GlyphIsProvided32(candidate, cp)) return candidate;
    }
    return primary;
}

// Text is rendered repeatedly on every frame. Retaining a small LRU set avoids
// allocating an SDL surface and GPU texture for every label on every frame.
#define TEXT_CACHE_CAPACITY 256
#define TEXT_CACHE_TEXT_MAX 160
typedef struct {
    SDL_Renderer *renderer;
    TTF_Font *font;
    SDL_Texture *texture;
    SDL_Color color;
    int width, height;
    u64 used;
    char text[TEXT_CACHE_TEXT_MAX];
} TextCacheEntry;
static TextCacheEntry text_cache[TEXT_CACHE_CAPACITY];
static u64 text_cache_clock = 1;

static void text_cache_clear(void) {
    for (int i = 0; i < TEXT_CACHE_CAPACITY; ++i) {
        if (text_cache[i].texture) SDL_DestroyTexture(text_cache[i].texture);
        memset(&text_cache[i], 0, sizeof(text_cache[i]));
    }
    text_cache_clock = 1;
}

static TextCacheEntry *text_cache_get(SDL_Renderer *r, TTF_Font *font, const char *text, SDL_Color color) {
    size_t len = strlen(text);
    if (len >= TEXT_CACHE_TEXT_MAX) return NULL;
    int slot = -1;
    u64 oldest = UINT64_MAX;
    for (int i = 0; i < TEXT_CACHE_CAPACITY; ++i) {
        TextCacheEntry *entry = &text_cache[i];
        if (entry->texture && entry->renderer == r && entry->font == font &&
            entry->color.r == color.r && entry->color.g == color.g && entry->color.b == color.b && entry->color.a == color.a &&
            strcmp(entry->text, text) == 0) {
            entry->used = text_cache_clock++;
            return entry;
        }
        if (!entry->texture) { slot = i; break; }
        if (entry->used < oldest) { oldest = entry->used; slot = i; }
    }
    if (slot < 0) return NULL;
    TextCacheEntry *entry = &text_cache[slot];
    if (entry->texture) SDL_DestroyTexture(entry->texture);
    memset(entry, 0, sizeof(*entry));
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return NULL;
    entry->texture = SDL_CreateTextureFromSurface(r, surface);
    entry->width = surface->w;
    entry->height = surface->h;
    SDL_FreeSurface(surface);
    if (!entry->texture) { memset(entry, 0, sizeof(*entry)); return NULL; }
    entry->renderer = r;
    entry->font = font;
    entry->color = color;
    memcpy(entry->text, text, len + 1);
    entry->used = text_cache_clock++;
    return entry;
}

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *text, int x, int y, SDL_Color color, int align) {
    if (!text || !text[0]) return;
    bool needs_fallback = false;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        Uint32 cp = utf8_next_codepoint(&p);
        if (!TTF_GlyphIsProvided32(font, cp)) { needs_fallback = true; break; }
    }
    if (needs_fallback) {
        int total_w = 0;
        const unsigned char *p = (const unsigned char *)text;
        while (*p) {
            const unsigned char *start = p;
            Uint32 cp = utf8_next_codepoint(&p);
            char glyph[5] = {0}; size_t n = (size_t)(p - start);
            memcpy(glyph, start, n > 4 ? 4 : n);
            TTF_Font *chosen = ui_font_for_codepoint(font, cp);
            int gw = 0; TTF_SizeUTF8(chosen, glyph, &gw, NULL); total_w += gw;
        }
        int draw_x = align == 0 ? x : (align == 1 ? x - total_w / 2 : x - total_w);
        p = (const unsigned char *)text;
        while (*p) {
            const unsigned char *start = p;
            Uint32 cp = utf8_next_codepoint(&p);
            char glyph[5] = {0}; size_t n = (size_t)(p - start);
            memcpy(glyph, start, n > 4 ? 4 : n);
            TTF_Font *chosen = ui_font_for_codepoint(font, cp);
            TextCacheEntry *part = text_cache_get(r, chosen, glyph, color);
            if (part) {
                SDL_Rect dst = {draw_x, y, part->width, part->height};
                SDL_RenderCopy(r, part->texture, NULL, &dst);
                draw_x += part->width;
            }
        }
        return;
    }
    TextCacheEntry *entry = text_cache_get(r, font, text, color);
    if (!entry) return;
    SDL_Rect rect;
    rect.y = y;
    rect.w = entry->width;
    rect.h = entry->height;
    rect.x = align == 0 ? x : (align == 1 ? x - entry->width / 2 : x - entry->width);
    SDL_RenderCopy(r, entry->texture, NULL, &rect);
}

static size_t utf8_prefix_bytes(const char *text, size_t characters, size_t max_bytes) {
    size_t bytes = 0;
    while (text[bytes] && characters > 0) {
        unsigned char lead = (unsigned char)text[bytes];
        size_t width = lead < 0x80 ? 1 : ((lead & 0xE0) == 0xC0 ? 2 : ((lead & 0xF0) == 0xE0 ? 3 : ((lead & 0xF8) == 0xF0 ? 4 : 1)));
        size_t i;
        for (i = 1; i < width && text[bytes + i]; i++) {
            if (((unsigned char)text[bytes + i] & 0xC0) != 0x80) break;
        }
        if (i != width) width = 1;
        if (bytes + width > max_bytes) break;
        bytes += width;
        characters--;
    }
    return bytes;
}

// Draw text truncated to a maximum pixel width (appends "..." when clipped).
static void draw_text_w(SDL_Renderer *r, TTF_Font *font, const char *text, int x, int y, SDL_Color color, int align, int max_w) {
    if (!text || !text[0] || max_w <= 0) return;
    int tw = 0;
    TTF_SizeUTF8(font, text, &tw, NULL);
    if (tw <= max_w) {
        draw_text(r, font, text, x, y, color, align);
        return;
    }
    // Binary search the longest prefix + "..." that still fits.
    char buf[256];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    size_t lo = 1, hi = n, best = 0, best_bytes = 0;
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t copy = utf8_prefix_bytes(text, mid, sizeof(buf) - 4);
        memcpy(buf, text, copy);
        buf[copy] = '.'; buf[copy+1] = '.'; buf[copy+2] = '.'; buf[copy+3] = 0;
        int w2 = 0;
        TTF_SizeUTF8(font, buf, &w2, NULL);
        if (w2 <= max_w) { best = mid; best_bytes = copy; lo = mid + 1; }
        else { hi = mid - 1; }
    }
    if (best > 0) {
        memcpy(buf, text, best_bytes);
        buf[best_bytes] = '.'; buf[best_bytes+1] = '.'; buf[best_bytes+2] = '.'; buf[best_bytes+3] = 0;
        draw_text(r, font, buf, x, y, color, align);
    } else {
        draw_text(r, font, "...", x, y, color, align);
    }
}

// Draw the degree mark as a tiny circle instead of relying on a platform font
// glyph; this avoids the mojibake seen on some shared Switch fonts.
static void draw_temperature(SDL_Renderer *r, TTF_Font *font, int milli_c, int x, int y, SDL_Color color, int align) {
    char value[32];
    snprintf(value, sizeof(value), "%d.%d", milli_c / 1000, (milli_c % 1000) / 100);
    int value_w = 0, c_w = 0;
    TTF_SizeUTF8(font, value, &value_w, NULL);
    TTF_SizeUTF8(font, "C", &c_w, NULL);
    int total_w = value_w + 7 + c_w;
    int left = align == 1 ? x - total_w / 2 : (align == 2 ? x - total_w : x);
    int glyph_y = y + TTF_FontHeight(font) / 2 - 3;
    draw_text(r, font, value, left, y, color, 0);
    circleRGBA(r, left + value_w + 3, glyph_y, 3, color.r, color.g, color.b, color.a);
    draw_text(r, font, "C", left + value_w + 8, y, color, 0);
}

static void draw_rounded_box(SDL_Renderer *r, int x, int y, int w, int h, int rad, SDL_Color color) {
    roundedBoxRGBA(r, x, y, x + w, y + h, rad, color.r, color.g, color.b, color.a);
}

static void draw_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int rad, SDL_Color color) {
    roundedRectangleRGBA(r, x, y, x + w, y + h, rad, color.r, color.g, color.b, color.a);
}

static void draw_card_shadow(SDL_Renderer *r, int x, int y, int w, int h) {
    for (int i = 1; i <= 4; i++)
        roundedBoxRGBA(r, x + i, y + i, x + w + i, y + h + i, 8, 0, 0, 0, 20 - i * 4);
}

static void draw_card(SDL_Renderer *r, int x, int y, int w, int h, const char *title) {
    draw_card_shadow(r, x, y, w, h);
    draw_rounded_box(r, x, y, w, h, 8, color_card);
    draw_rounded_rect(r, x, y, w, h, 8, color_card_border);
    if (title && title[0]) {
        filledCircleRGBA(r, x + 24, y + 26, 4, color_cyan.r, color_cyan.g, color_cyan.b, 255);
        circleRGBA(r, x + 24, y + 26, 7, color_cyan.r, color_cyan.g, color_cyan.b, 100);
        draw_text_w(r, font_md, title, x + 38, y + 14, color_cyan, 0, w - 55);
        // Accent line under title
        thickLineRGBA(r, x + 20, y + 48, x + w - 20, y + 48, 2, color_cyan.r, color_cyan.g, color_cyan.b, 120);
    }
}

static void draw_key_value(SDL_Renderer *r, const char *key, const char *val, int x, int y, SDL_Color val_color) {
    draw_text(r, font_sm, key, x, y, color_grey, 0);
    draw_text(r, font_sm, val, x + 215, y, val_color, 0);
}

static void draw_key_value_wide(SDL_Renderer *r, const char *key, const char *val, int x, int y, int offset, SDL_Color val_color) {
    draw_text(r, font_sm, key, x, y, color_grey, 0);
    draw_text(r, font_sm, val, x + offset, y, val_color, 0);
}

static void draw_progress_bar(SDL_Renderer *r, int x, int y, int w, int h, float progress, SDL_Color fg, SDL_Color bg) {
    draw_rounded_box(r, x, y, w, h, 4, bg);
    if (progress > 0.0f) {
        if (progress > 1.0f) progress = 1.0f;
        int pw = (int)(w * progress);
        if (pw < 8) pw = 8;
        draw_rounded_box(r, x, y, pw, h, 4, fg);
    }
}

static void draw_battery_icon(SDL_Renderer *r, int x, int y, int w, int h, u32 pct, bool charging) {
    // Outer shell
    SDL_Rect shell = {x, y, w - 5, h};
    SDL_SetRenderDrawColor(r, color_grey.r, color_grey.g, color_grey.b, 255);
    SDL_RenderDrawRect(r, &shell);
    
    // Battery tip
    SDL_Rect tip = {x + w - 5, y + h / 4, 5, h / 2};
    SDL_RenderFillRect(r, &tip);

    // Inner level
    if (pct > 0) {
        int fill_w = (int)((w - 9) * (pct / 100.f));
        if (fill_w < 2) fill_w = 2;
        SDL_Rect level = {x + 2, y + 2, fill_w, h - 4};
        
        SDL_Color color = color_green;
        if (pct <= 20) color = color_red;
        else if (pct <= 60) color = color_yellow;
        
        SDL_SetRenderDrawColor(r, color.r, color.g, color.b, 255);
        SDL_RenderFillRect(r, &level);
    }
    
    if (charging) {
        draw_text(r, font_sm, "+", x + w / 2 - 4, y - 2, color_white, 0);
    }
}

static void draw_wifi_bars(SDL_Renderer *r, int x, int y, int w, int h, u32 strength) {
    int bar_w = w / 3 - 3;
    if (strength > 3) strength = 3;
    for (int i = 0; i < 3; i++) {
        int bar_h = (h * (i + 1)) / 3;
        SDL_Rect bar = {x + i * (bar_w + 2), y + h - bar_h, bar_w, bar_h};
        if (i < (int)strength) {
            SDL_SetRenderDrawColor(r, color_cyan.r, color_cyan.g, color_cyan.b, 255);
        } else {
            SDL_SetRenderDrawColor(r, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
        }
        SDL_RenderFillRect(r, &bar);
    }
}

static SDL_Color get_usage_color(u32 pct) {
    if (pct < 60) return color_green;
    if (pct < 80) return color_yellow;
    return color_red;
}

static SDL_Color get_temp_color(s32 milliC) {
    if (milliC < 40000) return color_green;
    if (milliC < 55000) return color_yellow;
    return color_red;
}

static const char *get_joycon_battery_str(u32 level) {
    switch (level) {
        case 0: return cur_lang == LANG_EN ? "Empty" : cur_lang == LANG_FR ? "Vide" : cur_lang == LANG_IT ? "Vuoto" : cur_lang == LANG_ES ? "Vacio" : cur_lang == LANG_DE ? "Leer" : cur_lang == LANG_PT ? "Vazio" : "Leeg";
        case 1: return cur_lang == LANG_EN ? "Critical (25%)" : cur_lang == LANG_FR ? "Critique (25%)" : cur_lang == LANG_IT ? "Critico (25%)" : cur_lang == LANG_ES ? "Critico (25%)" : cur_lang == LANG_DE ? "Kritisch (25%)" : cur_lang == LANG_PT ? "Critico (25%)" : "Kritiek (25%)";
        case 2: return cur_lang == LANG_EN ? "Low (50%)" : cur_lang == LANG_FR ? "Faible (50%)" : cur_lang == LANG_IT ? "Basso (50%)" : cur_lang == LANG_ES ? "Bajo (50%)" : cur_lang == LANG_DE ? "Niedrig (50%)" : cur_lang == LANG_PT ? "Baixo (50%)" : "Laag (50%)";
        case 3: return cur_lang == LANG_EN ? "Medium (75%)" : cur_lang == LANG_FR ? "Moyen (75%)" : cur_lang == LANG_IT ? "Medio (75%)" : cur_lang == LANG_ES ? "Medio (75%)" : cur_lang == LANG_DE ? "Mittel (75%)" : cur_lang == LANG_PT ? "Medio (75%)" : "Gemiddeld (75%)";
        case 4: return cur_lang == LANG_EN ? "Full (100%)" : cur_lang == LANG_FR ? "Plein (100%)" : cur_lang == LANG_IT ? "Pieno (100%)" : cur_lang == LANG_ES ? "Lleno (100%)" : cur_lang == LANG_DE ? "Voll (100%)" : cur_lang == LANG_PT ? "Cheio (100%)" : "Vol (100%)";
        default: return cur_lang == LANG_EN ? "Unknown" : cur_lang == LANG_FR ? "Inconnu" : cur_lang == LANG_IT ? "Sconosciuto" : cur_lang == LANG_ES ? "Desconocido" : cur_lang == LANG_DE ? "Unbekannt" : cur_lang == LANG_PT ? "Desconhecido" : "Onbekend";
    }
}

static SDL_Color get_joycon_battery_color(u32 level) {
    if (level <= 1) return color_red;
    if (level == 2) return color_yellow;
    return color_green;
}

// ─── SoC (internal) temperature ───────────────────────────
static s32 soc_temp_c = 0;
static bool soc_temp_valid = false;
static u64 soc_temp_tick = 0;

static void soc_temp_update(void) {
    u64 now = armGetSystemTick();
    if (now - soc_temp_tick < armGetSystemTickFreq()) return;
    soc_temp_tick = now;
    soc_temp_valid = false;
    if (R_SUCCEEDED(tsInitialize())) {
        s32 t = 0;
        if (R_SUCCEEDED(tsGetTemperature(TsLocation_External, &t))) {
            soc_temp_c = t;
            soc_temp_valid = true;
        }
        tsExit();
    }
}

// ─── FTP Server ───────────────────────────────────────────
#define FTP_PORT 5000
#define FTP_MAXCL 4
#define FTP_BUF 4096
#define FTP_XFER_BUF (64 * 1024)
#define FTP_LOG_MAX 48
#define MTP_LOG_MAX 24

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static volatile int ftp_on = 0;
static volatile int ftp_srv = -1, ftp_data = -1;
static volatile int ftp_cli_fd = -1;
static volatile int ftp_xfer_fd = -1;
static Thread ftp_thr;
static char ftp_cwd[256] = "/";
static u32 ftp_ip = 0;
static u32 ftp_xfer_count = 0;
static u64 ftp_bytes_total = 0;
static u32 ftp_upload_count = 0;
static u32 ftp_download_count = 0;
static volatile int ftp_xfer_active = 0;
static volatile u64 ftp_xfer_done = 0;
static volatile u64 ftp_xfer_total = 0; // Zero means the FTP client did not announce a size.
static volatile u64 ftp_xfer_start_tick = 0;
static char ftp_xfer_name[96] = {0};
static int ftp_xfer_is_upload = 0;
static u64 ftp_expected_xfer_size = 0;

// Activity log
static char ftp_log[FTP_LOG_MAX][160];
static unsigned int ftp_log_idx = 0;
static Mutex ftp_log_mtx;
static int transfer_log_scroll = 0;
static int transfer_log_scroll_max = 0;
static int transfer_log_touch_y = -1;

static int ftp_mode = 0; // 0=FTP, 1=FTPD
static int tools_game_scroll = 0;
static int tools_game_touch_y = -1;
static int ftp_get_port(void) { return ftp_mode ? 5001 : 5000; }

// libnx defaults to 32 KiB TX / 64 KiB RX buffers capped at 256 KiB. Larger
// TCP windows are used by established Switch FTP servers to improve LAN rates.
static const SocketInitConfig ftp_socket_config = {
    .tcp_tx_buf_size = 1 * 1024 * 1024,
    .tcp_rx_buf_size = 1 * 1024 * 1024,
    .tcp_tx_buf_max_size = 4 * 1024 * 1024,
    .tcp_rx_buf_max_size = 4 * 1024 * 1024,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 4,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};

static char ftp_rnfr[260] = {0};
static u64 ftp_rest_offset = 0;

static void ftp_addlog(const char *msg) {
    mutexLock(&ftp_log_mtx);
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt)
        snprintf(ftp_log[ftp_log_idx % FTP_LOG_MAX], sizeof(ftp_log[0]), "%02d:%02d:%02d  %s",
            lt->tm_hour, lt->tm_min, lt->tm_sec, msg);
    else
        snprintf(ftp_log[ftp_log_idx % FTP_LOG_MAX], sizeof(ftp_log[0]), "%s", msg);
    ftp_log_idx++;
    mutexUnlock(&ftp_log_mtx);
}

static void add_alert(const char *msg) {
    // Shift older notifications to make room
    if (notif_count >= NOTIF_MAX) {
        for (int i = 1; i < NOTIF_MAX; i++) {
            strncpy(notif_msgs[i-1], notif_msgs[i], sizeof(notif_msgs[0])-1);
            notif_ticks[i-1] = notif_ticks[i];
        }
        notif_count = NOTIF_MAX - 1;
    }
    strncpy(notif_msgs[notif_count], msg, sizeof(notif_msgs[0])-1);
    notif_msgs[notif_count][sizeof(notif_msgs[0])-1] = 0;
    notif_ticks[notif_count] = armGetSystemTick();
    notif_count++;
    // Also keep legacy export_msg for tools page display
    snprintf(export_msg, sizeof(export_msg), "%s", msg);
    export_msg_tick = armGetSystemTick();
}

static int ftp_send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len) {
        // Switch's BSD socket layer does not need MSG_NOSIGNAL; unsupported
        // flags can make even the initial 220 greeting fail with EINVAL.
        ssize_t sent = send(fd, p, len, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        p += sent;
        len -= (size_t)sent;
    }
    return 0;
}
static int fsend(int fd, const char *m) { return ftp_send_all(fd, m, strlen(m)); }

static void get_real_path(char *dst, size_t dst_size, const char *virtual_path) {
    if (virtual_path[0] == '/') {
        snprintf(dst, dst_size, "sdmc:%s", virtual_path);
    } else {
        snprintf(dst, dst_size, "sdmc:/%s", virtual_path);
    }
}

static void fclean(char *d, const char *b, const char *r) {
    char input[768];
    const char *path = r ? r : "";
    if (strncmp(path, "sdmc:", 5) == 0) path += 5;
    if (!path[0]) path = b;
    if (path[0] == '/') snprintf(input, sizeof(input), "%s", path);
    else snprintf(input, sizeof(input), "%s/%s", (b && b[0]) ? b : "/", path);

    // Canonicalize every path component and clamp `..` at the FTP SD root.
    // This keeps CWD/RETR/LIST and write commands consistent at any depth.
    size_t out = 0;
    d[out++] = '/';
    d[out] = 0;
    const char *p = input;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t part = (size_t)(p - start);
        if (part == 1 && start[0] == '.') continue;
        if (part == 2 && start[0] == '.' && start[1] == '.') {
            if (out > 1) {
                if (d[out - 1] == '/') out--;
                while (out > 1 && d[out - 1] != '/') out--;
                if (out > 1 && d[out - 1] == '/') out--;
                d[out] = 0;
            }
            continue;
        }
        size_t need = part + (out > 1 ? 1 : 0);
        if (out + need >= 255) {
            snprintf(d, 256, "/__path_too_long__");
            return;
        }
        if (out > 1) d[out++] = '/';
        memcpy(d + out, start, part);
        out += part;
        d[out] = 0;
    }
}

static int ftp_recv_line(int fd, char *buf, size_t capacity) {
    if (capacity < 2) return -1;
    size_t used = 0;
    bool overflow = false;
    while (ftp_on) {
        char c;
        ssize_t got = recv(fd, &c, 1, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (used + 1 < capacity) buf[used++] = c;
        else overflow = true;
    }
    buf[used] = 0;
    return overflow ? -2 : (int)used;
}

static int ftp_list_recursive(int data_fd, const char *virtual_dir, int depth) {
    if (depth > 24) return 0;
    char real_dir[540];
    get_real_path(real_dir, sizeof(real_dir), virtual_dir);
    DIR *dir = opendir(real_dir);
    if (!dir) return 0;
    char header[320];
    int hn = snprintf(header, sizeof(header), "%s:\r\n", virtual_dir);
    if (hn > 0 && ftp_send_all(data_fd, header, (size_t)hn) < 0) { closedir(dir); return -1; }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[256], real_child[540], line[640];
        fclean(child, virtual_dir, entry->d_name);
        get_real_path(real_child, sizeof(real_child), child);
        struct stat st;
        if (stat(real_child, &st) != 0) continue;
        char size[32] = "-", date[32] = "Jan  1 00:00";
        if (!S_ISDIR(st.st_mode)) snprintf(size, sizeof(size), "%lu", (unsigned long)st.st_size);
        struct tm *mt = gmtime(&st.st_mtime);
        if (mt) {
            static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
            snprintf(date, sizeof(date), "%s %2d %02d:%02d", months[mt->tm_mon], mt->tm_mday, mt->tm_hour, mt->tm_min);
        }
        int ln = snprintf(line, sizeof(line), "%s 1 switch switch %12s %s %s\r\n",
            S_ISDIR(st.st_mode) ? "drwxr-xr-x" : "-rw-r--r--", size, date, entry->d_name);
        if (ln > 0 && ftp_send_all(data_fd, line, (size_t)ln) < 0) { closedir(dir); return -1; }
        if (S_ISDIR(st.st_mode) && ftp_list_recursive(data_fd, child, depth + 1) < 0) { closedir(dir); return -1; }
    }
    closedir(dir);
    return 0;
}

static void fcli(int fd, u32 ip) {
    ftp_cli_fd = fd;
    // The server processes control sessions on its accept thread. Without an
    // idle timeout, a client that connected and then stopped sending commands
    // can hold that thread forever; later clients complete TCP connect but sit
    // waiting for a 220 greeting until their own timeout expires. Keep the
    // timeout short so a queued FileZilla control connection is released fast.
    struct timeval control_idle_timeout = {4, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&control_idle_timeout,
                   sizeof(control_idle_timeout)) < 0) {
        char err[128];
        snprintf(err, sizeof(err), "Could not set FTP control idle timeout: %s", strerror(errno));
        ftp_addlog(err);
    }
    char buf[FTP_BUF];
    const char *mode_name = ftp_mode ? "FTPD" : "FTP";
    int port = ftp_get_port();
    char banner[128];
    snprintf(banner, sizeof(banner), "220 SwitchInfoNX %s v" APP_VERSION " ready on port %d\r\n", mode_name, port);
    ftp_addlog("Client accepted; sending 220 greeting");
    if (fsend(fd, banner) < 0) {
        char err[128];
        snprintf(err, sizeof(err), "Failed to send 220 greeting: %s", strerror(errno));
        ftp_addlog(err);
        close(fd);
        ftp_cli_fd = -1;
        return;
    }
    ftp_addlog("220 greeting sent successfully");
    ftp_data = -1;
    ftp_rest_offset = 0;
    // Always start at SD root on new client connection
    snprintf(ftp_cwd, sizeof(ftp_cwd), "/");
    ftp_addlog("Client connected at /");
    add_alert("FTP: Client connected");

    while (ftp_on) {
        memset(buf, 0, sizeof(buf));
        int n = ftp_recv_line(fd, buf, sizeof(buf));
        if (n == -2) { fsend(fd, "500 Command line too long\r\n"); continue; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                ftp_addlog("Control idle timeout; released client slot");
            break;
        }
        if (n == 0) continue;

        char cmd[32]={0}, arg[256]={0};
        sscanf(buf, "%31s %255[^\r\n]", cmd, arg);
        // FTP command names are case-insensitive (RFC 959). Some clients send
        // lower/mixed-case verbs, so normalize before dispatching them.
        for (char *c = cmd; *c; ++c) {
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - ('a' - 'A'));
        }
        if (strcmp(cmd, "PASS") != 0) {
            char activity[220];
            snprintf(activity, sizeof(activity), "CMD %.24s %.180s", cmd, arg);
            ftp_addlog(activity);
        } else {
            ftp_addlog("CMD PASS (credentials hidden)");
        }

        if (strcmp(cmd,"USER")==0) fsend(fd,"230 Login OK (anonymous)\r\n");
        else if (strcmp(cmd,"PASS")==0) fsend(fd,"230 Login OK\r\n");
        else if (strcmp(cmd,"SYST")==0) {
            // Keep the RFC 959 215 reply short and terminate it with CRLF;
            // clients such as FileZilla wait for this reply before continuing.
            if (fsend(fd,"215 UNIX Type: L8\r\n") < 0) break;
            ftp_addlog("SYST reply sent: 215 UNIX Type: L8");
        }
        else if (strcmp(cmd,"AUTH")==0) {
            // This server currently supports plain FTP only. Answer AUTH TLS
            // explicitly so clients configured to use TLS when available can
            // fall back to the advertised plain FTP session instead of waiting.
            if (strcasecmp(arg, "TLS") == 0 || strcasecmp(arg, "SSL") == 0) {
                if (fsend(fd,"502 TLS is not supported; plain FTP is available\r\n") < 0) break;
                ftp_addlog("AUTH TLS declined; plain FTP remains available");
            } else {
                if (fsend(fd,"504 AUTH mechanism not supported\r\n") < 0) break;
            }
        }
        else if (strcmp(cmd,"CLNT")==0) fsend(fd,"200 Client identification noted\r\n");
        else if (strcmp(cmd,"ALLO")==0) {
            char *end = NULL;
            unsigned long long announced = strtoull(arg, &end, 10);
            ftp_expected_xfer_size = (end != arg && announced > 0) ? (u64)announced : 0;
            fsend(fd,"202 ALLO command accepted\r\n");
        }
        else if (strcmp(cmd,"MODE")==0 || strcmp(cmd,"STRU")==0) fsend(fd,"200 Command accepted\r\n");
        else if (strcmp(cmd,"FEAT")==0) {
            fsend(fd,"211-Features:\r\n SIZE\r\n PASV\r\n EPSV\r\n UTF8\r\n MDTM\r\n REST STREAM\r\n MLST type*;size*;modify*;\r\n MLSD\r\n211 End\r\n");
        }
        else if (strcmp(cmd,"OPTS")==0) fsend(fd,"200 OK\r\n");
        else if (strcmp(cmd,"PWD")==0||strcmp(cmd,"XPWD")==0) {
            char p[512]; snprintf(p,sizeof(p),"257 \"%s\"\r\n",ftp_cwd);
            fsend(fd,p);
        }
        else if (strcmp(cmd,"QUIT")==0) {
            fsend(fd,"221 Goodbye\r\n");
            ftp_addlog("Client disconnected");
            add_alert("FTP: Client disconnected");
            break;
        }
        else if (strcmp(cmd,"TYPE")==0) fsend(fd,"200 Type set\r\n");
        else if (strcmp(cmd,"NOOP")==0) fsend(fd,"200 OK\r\n");
        else if (strcmp(cmd,"CDUP")==0||strcmp(cmd,"XCUP")==0) {
            fclean(ftp_cwd, ftp_cwd, "..");
            fsend(fd,"250 OK\r\n");
        }
        else if (strcmp(cmd,"REST")==0) {
            ftp_rest_offset = (u64)strtoull(arg, NULL, 10);
            char resp[64];
            snprintf(resp, sizeof(resp), "350 Restarting at %llu\r\n", (unsigned long long)ftp_rest_offset);
            fsend(fd, resp);
        }
        else if (strcmp(cmd,"SIZE")==0) {
            char p[512]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            struct stat st;
            if (stat(rp,&st)==0) {
                char z[64]; snprintf(z,sizeof(z),"213 %lu\r\n",(unsigned long)st.st_size);
                fsend(fd,z);
            } else fsend(fd,"550 File not found\r\n");
        }
        else if (strcmp(cmd,"MDTM")==0) {
            char p[512]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            struct stat st;
            if (stat(rp,&st)==0) {
                struct tm *mt = gmtime(&st.st_mtime);
                if (mt) {
                    char z[64]; snprintf(z,sizeof(z),"213 %04d%02d%02d%02d%02d%02d\r\n",
                        mt->tm_year+1900,mt->tm_mon+1,mt->tm_mday,
                        mt->tm_hour,mt->tm_min,mt->tm_sec);
                    fsend(fd,z);
                } else fsend(fd,"550 Error\r\n");
            } else fsend(fd,"550 File not found\r\n");
        }
        else if (strcmp(cmd,"CWD")==0||strcmp(cmd,"XCWD")==0) {
            const char *target = arg;
            while (*target == ' ') target++;
            if (strncmp(target, "sdmc:/", 6) == 0) target += 5;
            else if (strncmp(target, "sdmc:", 5) == 0) target += 5;

            if (!target[0] || strcmp(target, "/") == 0 || strcmp(target, "\\") == 0) {
                snprintf(ftp_cwd, sizeof(ftp_cwd), "/");
                ftp_addlog("CWD /");
                fsend(fd,"250 Directory changed\r\n");
                continue;
            }

            char new_dir[256];
            fclean(new_dir, ftp_cwd, target);
            char rp[540]; get_real_path(rp, sizeof(rp), new_dir);
            struct stat st;
            if (stat(rp, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(ftp_cwd, sizeof(ftp_cwd), "%s", new_dir);
                if (!ftp_cwd[0]) snprintf(ftp_cwd, sizeof(ftp_cwd), "/");
                char log[300]; snprintf(log,sizeof(log),"CWD %s", ftp_cwd);
                ftp_addlog(log);
                fsend(fd,"250 Directory changed\r\n");
            } else if (strcmp(new_dir, "/") == 0) {
                snprintf(ftp_cwd, sizeof(ftp_cwd), "/");
                ftp_addlog("CWD /");
                fsend(fd,"250 Directory changed\r\n");
            } else {
                fsend(fd,"550 Directory not found\r\n");
            }
        }
        else if (strcmp(cmd,"PASV")==0 || strcmp(cmd,"EPSV")==0) {
            if (ftp_data>=0) close(ftp_data);
            ftp_data = socket(AF_INET, SOCK_STREAM, 0);
            if (ftp_data<0) { fsend(fd,"421 Err\r\n"); continue; }
            struct sockaddr_in da;
            socklen_t sz=sizeof(da);
            int opt=1; setsockopt(ftp_data,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
            da.sin_family=AF_INET; da.sin_addr.s_addr=INADDR_ANY; da.sin_port=0;
            if (bind(ftp_data,(struct sockaddr*)&da,sizeof(da))<0||listen(ftp_data,1)<0)
                { close(ftp_data); ftp_data=-1; fsend(fd,"421 Err\r\n"); continue; }
            getsockname(ftp_data,(struct sockaddr*)&da,&sz);
            int dp = ntohs(da.sin_port);
            char r[128];
            if (strcmp(cmd, "EPSV") == 0)
                snprintf(r, sizeof(r), "229 Entering Extended Passive Mode (|||%d|)\r\n", dp);
            else
                snprintf(r,sizeof(r),"227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\r\n",
                    (ip>>0)&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF,(dp>>8)&0xFF,dp&0xFF);
            fsend(fd,r);
        }
        else if (strcmp(cmd,"LIST")==0 || strcmp(cmd,"NLST")==0 || strcmp(cmd,"MLSD")==0) {
            int names_only = strcmp(cmd,"NLST")==0;
            int machine_listing = strcmp(cmd,"MLSD")==0;
            int recursive_listing = 0;
            char dp[256];
            const char *lpath = arg;
            while (*lpath == ' ') lpath++;
            if (lpath[0] == '-') {
                // Accept common FileZilla flags, including recursive LIST -R.
                const char *sp = strchr(lpath, ' ');
                if (sp) {
                    for (const char *flag = lpath + 1; flag < sp; ++flag)
                        if (*flag == 'R' || *flag == 'r') recursive_listing = 1;
                    lpath = sp + 1;
                }
                else lpath = "";
            }
            while (*lpath == ' ') lpath++;
            if (lpath[0]) fclean(dp, ftp_cwd, lpath);
            else snprintf(dp, sizeof(dp), "%s", ftp_cwd);

            char rp[540]; get_real_path(rp, sizeof(rp), dp);
            DIR *d = opendir(rp);
            if (!d) { fsend(fd,"450 Cannot open dir\r\n"); continue; }
            if (ftp_data<0) { closedir(d); fsend(fd,"425 No data connection\r\n"); continue; }
            struct sockaddr_in cl;
            socklen_t cln=sizeof(cl);
            int df = accept(ftp_data,(struct sockaddr*)&cl,&cln);
            if (df<0) { closedir(d); fsend(fd,"425 Accept failed\r\n"); continue; }
            close(ftp_data); ftp_data=-1;
            ftp_xfer_fd = df;
            fsend(fd,"150 Opening ASCII mode data connection\r\n");
            struct dirent *e;
            int listing_failed = 0;
            if (recursive_listing && !machine_listing && !names_only) {
                closedir(d); d = NULL;
                listing_failed = ftp_list_recursive(df, dp, 0) < 0;
            }
            while (d && !listing_failed && (e=readdir(d))!=NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                char l[512];
                struct stat st;
                memset(&st, 0, sizeof(st));
                char f[1024];
                if (strcmp(dp, "/") == 0)
                    snprintf(f, sizeof(f), "sdmc:/%s", e->d_name);
                else
                    snprintf(f, sizeof(f), "sdmc:%s/%s", dp, e->d_name);
                int res = stat(f, &st);
                int id = (res == 0 && S_ISDIR(st.st_mode));
                char szb[32] = "-";
                if (!id) {
                    if (res == 0)
                        snprintf(szb, sizeof(szb), "%lu", (unsigned long)st.st_size);
                    else
                        snprintf(szb, sizeof(szb), "0");
                }
                char datestr[32] = "Jan  1 00:00";
                char machine_date[20] = "00000000000000";
                if (res == 0) {
                    struct tm *mt = gmtime(&st.st_mtime);
                    if (mt) {
                        static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
                        snprintf(datestr, sizeof(datestr), "%s %2d %02d:%02d",
                            months[mt->tm_mon], mt->tm_mday, mt->tm_hour, mt->tm_min);
                        strftime(machine_date, sizeof(machine_date), "%Y%m%d%H%M%S", mt);
                    }
                }
                if (names_only) {
                    snprintf(l, sizeof(l), "%s\r\n", e->d_name);
                } else if (machine_listing) {
                    if (id)
                        snprintf(l, sizeof(l), "type=dir;modify=%s; %s\r\n", machine_date, e->d_name);
                    else
                        snprintf(l, sizeof(l), "type=file;size=%s;modify=%s; %s\r\n", szb, machine_date, e->d_name);
                } else {
                    snprintf(l,sizeof(l),"%s 1 switch switch %12s %s %s\r\n",
                        id?"drwxr-xr-x":"-rw-r--r--",szb,datestr,e->d_name);
                }
                if (ftp_send_all(df, l, strlen(l)) < 0) { listing_failed = 1; break; }
            }
            if (d) closedir(d);
            close(df);
            ftp_xfer_fd = -1;
            fsend(fd, listing_failed ? "426 Directory listing interrupted\r\n" : "226 Transfer complete\r\n");
        }
        else if (strcmp(cmd,"MLST")==0) {
            char p[256]; fclean(p, ftp_cwd, arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            struct stat st;
            if (stat(rp, &st) != 0) { fsend(fd, "550 File not found\r\n"); continue; }
            struct tm *mt = gmtime(&st.st_mtime);
            char modify[20] = "00000000000000";
            if (mt) strftime(modify, sizeof(modify), "%Y%m%d%H%M%S", mt);
            char reply[640];
            if (S_ISDIR(st.st_mode))
                snprintf(reply, sizeof(reply), "250-Listing %s\r\n type=dir;modify=%s; %s\r\n250 End\r\n", p, modify, strrchr(p, '/') ? strrchr(p, '/') + 1 : p);
            else
                snprintf(reply, sizeof(reply), "250-Listing %s\r\n type=file;size=%lu;modify=%s; %s\r\n250 End\r\n", p, (unsigned long)st.st_size, modify, strrchr(p, '/') ? strrchr(p, '/') + 1 : p);
            fsend(fd, reply);
        }
        else if (strcmp(cmd,"RETR")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            FILE *f = fopen(rp,"rb");
            if (!f) { fsend(fd,"550 File not found\r\n"); continue; }
            struct stat transfer_st;
            u64 transfer_total = (stat(rp, &transfer_st) == 0 && transfer_st.st_size > 0)
                ? (u64)transfer_st.st_size : 0;
            if (ftp_data<0) { fclose(f); fsend(fd,"425 No data connection\r\n"); continue; }
            struct sockaddr_in cl;
            socklen_t cln=sizeof(cl);
            int df = accept(ftp_data,(struct sockaddr*)&cl,&cln);
            if (df<0) { fclose(f); fsend(fd,"425 Accept failed\r\n"); continue; }
            close(ftp_data); ftp_data=-1;
            ftp_xfer_fd = df;
            // Handle REST offset
            if (ftp_rest_offset > 0) {
                fseek(f, (long)ftp_rest_offset, SEEK_SET);
                transfer_total = transfer_total > ftp_rest_offset ? transfer_total - ftp_rest_offset : 0;
                ftp_rest_offset = 0;
            }
            ftp_xfer_done = 0;
            ftp_xfer_total = transfer_total;
            ftp_xfer_start_tick = armGetSystemTick();
            ftp_xfer_is_upload = 0;
            snprintf(ftp_xfer_name, sizeof(ftp_xfer_name), "%s", strrchr(arg, '/') ? strrchr(arg, '/') + 1 : arg);
            ftp_xfer_active = 1;
            fsend(fd,"150 Sending\r\n");
            char db[FTP_XFER_BUF]; int r = 0;
            u64 sent = 0;
            bool transfer_failed = false;
            while (ftp_on && (r=fread(db,1,sizeof(db),f))>0) {
                if (ftp_send_all(df, db, (size_t)r) < 0) { transfer_failed = true; break; }
                sent += (u64)r;
                ftp_xfer_done = sent;
            }
            if (ferror(f)) transfer_failed = true;
            fclose(f); close(df); ftp_xfer_fd = -1;
            ftp_xfer_active = 0;
            fsend(fd, transfer_failed ? "426 Download interrupted\r\n" : "226 Transfer complete\r\n");
            if (!transfer_failed) { ftp_xfer_count++; ftp_download_count++; ftp_bytes_total += sent; }
            char log[160]; snprintf(log,sizeof(log),"RETR %s (%llu B)%s", arg, (unsigned long long)sent, transfer_failed ? " FAILED" : " OK");
            ftp_addlog(log);
        }
        else if (strcmp(cmd,"STOR")==0 || strcmp(cmd,"APPE")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            const char *mode = (strcmp(cmd,"APPE")==0) ? "ab" : "wb";
            FILE *f = fopen(rp, mode);
            if (!f) { fsend(fd,"550 Cannot create file\r\n"); continue; }
            u64 transfer_total = ftp_expected_xfer_size;
            ftp_expected_xfer_size = 0;
            if (ftp_data<0) { fclose(f); fsend(fd,"425 No data connection\r\n"); continue; }
            struct sockaddr_in cl;
            socklen_t cln=sizeof(cl);
            int df = accept(ftp_data,(struct sockaddr*)&cl,&cln);
            if (df<0) { fclose(f); fsend(fd,"425 Accept failed\r\n"); continue; }
            close(ftp_data); ftp_data=-1;
            ftp_xfer_fd = df;
            // Handle REST offset for STOR
            if (ftp_rest_offset > 0 && strcmp(cmd,"STOR")==0) {
                fseek(f, (long)ftp_rest_offset, SEEK_SET);
                ftp_rest_offset = 0;
            }
            ftp_xfer_done = 0;
            ftp_xfer_total = transfer_total;
            ftp_xfer_start_tick = armGetSystemTick();
            ftp_xfer_is_upload = 1;
            snprintf(ftp_xfer_name, sizeof(ftp_xfer_name), "%s", strrchr(arg, '/') ? strrchr(arg, '/') + 1 : arg);
            ftp_xfer_active = 1;
            fsend(fd,"150 Receiving\r\n");
            char db[FTP_XFER_BUF]; int r = 0;
            u64 rcvd = 0;
            bool write_failed = false;
            while (ftp_on) {
                r = (int)recv(df, db, sizeof(db), 0);
                if (r < 0 && errno == EINTR) continue;
                if (r < 0) { write_failed = true; break; }
                if (r == 0) break;
                if (fwrite(db,1,(size_t)r,f) != (size_t)r) { write_failed = true; break; }
                rcvd += r;
                ftp_xfer_done = rcvd;
            }
            if (fclose(f) != 0) write_failed = true;
            close(df); ftp_xfer_fd = -1;
            ftp_xfer_active = 0;
            fsend(fd, write_failed ? "451 Write failed\r\n" : "226 Transfer complete\r\n");
            if (!write_failed) { ftp_xfer_count++; ftp_upload_count++; ftp_bytes_total += rcvd; }
            char log[160]; snprintf(log,sizeof(log),"%s %s (%llu B)%s", cmd, arg, (unsigned long long)rcvd, write_failed ? " FAILED" : " OK");
            ftp_addlog(log);
        }
        else if (strcmp(cmd,"MKD")==0 || strcmp(cmd,"XMKD")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            if (mkdir(rp, 0755)==0) {
                char r[512]; snprintf(r,sizeof(r),"257 \"%s\" created\r\n",p);
                fsend(fd,r);
                char log[160]; snprintf(log,sizeof(log),"MKD %s OK", arg);
                ftp_addlog(log);
            } else {
                int saved_errno = errno;
                struct stat st;
                if (saved_errno == EEXIST && stat(rp, &st) == 0 && S_ISDIR(st.st_mode)) {
                    // FileZilla treats this exact reply as a successful, idempotent MKD.
                    char reply[320]; snprintf(reply, sizeof(reply), "550 Directory already exists\r\n"); fsend(fd, reply);
                    char log[160]; snprintf(log, sizeof(log), "MKD %s already exists", arg); ftp_addlog(log);
                } else {
                    char reply[256]; snprintf(reply, sizeof(reply), "550 Cannot create directory: %s\r\n", strerror(saved_errno)); fsend(fd, reply);
                    char log[160]; snprintf(log, sizeof(log), "MKD %s FAILED: %s", arg, strerror(saved_errno)); ftp_addlog(log);
                }
            }
        }
        else if (strcmp(cmd,"RMD")==0 || strcmp(cmd,"XRMD")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            if (rmdir(rp)==0) {
                fsend(fd,"250 Directory removed\r\n");
                char log[80]; snprintf(log,sizeof(log),"RMD %s", arg);
                ftp_addlog(log);
            } else fsend(fd,"550 Cannot remove directory\r\n");
        }
        else if (strcmp(cmd,"DELE")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            if (remove(rp)==0) {
                fsend(fd,"250 File deleted\r\n");
                char log[80]; snprintf(log,sizeof(log),"DELE %s", arg);
                ftp_addlog(log);
            } else fsend(fd,"550 Cannot delete\r\n");
        }
        else if (strcmp(cmd,"RNFR")==0) {
            fclean(ftp_rnfr, ftp_cwd, arg);
            fsend(fd,"350 Ready for RNTO\r\n");
        }
        else if (strcmp(cmd,"RNTO")==0) {
            if (!ftp_rnfr[0]) { fsend(fd,"503 RNFR required first\r\n"); continue; }
            char dst[260]; fclean(dst, ftp_cwd, arg);
            char rsrc[540]; get_real_path(rsrc, sizeof(rsrc), ftp_rnfr);
            char rdst[540]; get_real_path(rdst, sizeof(rdst), dst);
            if (rename(rsrc, rdst)==0) {
                fsend(fd,"250 Rename successful\r\n");
                char log[80]; snprintf(log,sizeof(log),"RENAME -> %s", arg);
                ftp_addlog(log);
            } else fsend(fd,"550 Rename failed\r\n");
            ftp_rnfr[0] = 0;
        }
        else if (strcmp(cmd,"STAT")==0) {
            if (arg[0]) {
                // STAT on a file/dir
                char p[260]; fclean(p,ftp_cwd,arg);
                char rp[540]; get_real_path(rp, sizeof(rp), p);
                struct stat st;
                if (stat(rp,&st)==0) {
                    char resp[256];
                    snprintf(resp, sizeof(resp), "213-Status of %s:\r\n Size: %lu\r\n213 End\r\n",
                        arg, (unsigned long)st.st_size);
                    fsend(fd, resp);
                } else fsend(fd,"550 File not found\r\n");
            } else {
                char resp[512];
                snprintf(resp, sizeof(resp), "211-FTP Server Status\r\n CWD: %s\r\n Transfers: %u\r\n211 End\r\n",
                    ftp_cwd, ftp_xfer_count);
                fsend(fd, resp);
            }
        }
        else if (strcmp(cmd,"PORT")==0) fsend(fd,"200 Use PASV instead\r\n");
        else if (strcmp(cmd,"ABOR")==0) fsend(fd,"226 Abort OK\r\n");
        else { char er[128]; snprintf(er,sizeof(er),"500 Unknown: %s\r\n",cmd); fsend(fd,er); }
    }
    if (ftp_data>=0) { close(ftp_data); ftp_data=-1; }
    close(fd);
    ftp_cli_fd = -1;
}

static void fserve(void *arg) {
    (void)arg;
    struct sockaddr_in a;
    ftp_srv = socket(AF_INET, SOCK_STREAM, 0);
    if (ftp_srv<0) {
        char err[128];
        snprintf(err, sizeof(err), "FTP socket failed: %s", strerror(errno));
        ftp_addlog(err);
        ftp_on=0;
        return;
    }
    int o=1; setsockopt(ftp_srv,SOL_SOCKET,SO_REUSEADDR,&o,sizeof(o));
    a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(ftp_get_port());
    if (bind(ftp_srv,(struct sockaddr*)&a,sizeof(a))<0) {
        int saved_errno = errno;
        char err[160];
        snprintf(err, sizeof(err), "FTP bind port %d failed: %s", ftp_get_port(), strerror(saved_errno));
        ftp_addlog(err);
        close(ftp_srv); ftp_srv=-1; ftp_on=0; return;
    }
    if (listen(ftp_srv,FTP_MAXCL)<0) {
        int saved_errno = errno;
        char err[160];
        snprintf(err, sizeof(err), "FTP listen port %d failed: %s", ftp_get_port(), strerror(saved_errno));
        ftp_addlog(err);
        close(ftp_srv); ftp_srv=-1; ftp_on=0; return;
    }
    char logmsg[64];
    snprintf(logmsg, sizeof(logmsg), "%s listening on port %d", ftp_mode ? "FTPD" : "FTP", ftp_get_port());
    ftp_addlog(logmsg);
    while (ftp_on) {
        struct sockaddr_in c;
        socklen_t cl=sizeof(c);
        int fd = accept(ftp_srv,(struct sockaddr*)&c,&cl);
        if (fd<0) { if (!ftp_on) break; continue; }
        char client_msg[96];
        snprintf(client_msg, sizeof(client_msg), "Accepted client %u.%u.%u.%u:%u",
            (ntohl(c.sin_addr.s_addr) >> 24) & 0xFF,
            (ntohl(c.sin_addr.s_addr) >> 16) & 0xFF,
            (ntohl(c.sin_addr.s_addr) >> 8) & 0xFF,
            ntohl(c.sin_addr.s_addr) & 0xFF,
            ntohs(c.sin_port));
        ftp_addlog(client_msg);
        fcli(fd, ftp_ip);
    }
    if (ftp_srv >= 0) {
        close(ftp_srv);
        ftp_srv = -1;
    }
    ftp_on=0;
}

static void ftp_start(void) {
    if (ftp_on) return;
    snprintf(ftp_cwd, sizeof(ftp_cwd), "/");
    nifmGetCurrentIpAddress(&ftp_ip);
    ftp_xfer_count = 0;
    ftp_bytes_total = 0;
    ftp_upload_count = 0;
    ftp_download_count = 0;
    ftp_log_idx = 0;
    ftp_rnfr[0] = 0;
    ftp_rest_offset = 0;
    ftp_cli_fd = -1;
    ftp_xfer_fd = -1;
    memset(ftp_log, 0, sizeof(ftp_log));
    ftp_on = 1;
    Result rc = threadCreate(&ftp_thr, fserve, NULL, NULL, 131072, 0x2B, -2);
    if (R_FAILED(rc)) {
        char err[96];
        snprintf(err, sizeof(err), "FTP thread creation failed: %08X", (unsigned)rc);
        ftp_addlog(err);
        ftp_on = 0;
        return;
    }
    rc = threadStart(&ftp_thr);
    if (R_FAILED(rc)) {
        char err[96];
        snprintf(err, sizeof(err), "FTP thread start failed: %08X", (unsigned)rc);
        ftp_addlog(err);
        ftp_on = 0;
        threadClose(&ftp_thr);
        return;
    }
    svcSleepThread(500000000);
}

static void ftp_stop(void) {
    if (!ftp_on) return;
    ftp_on = 0;
    
    if (ftp_srv >= 0) {
        shutdown(ftp_srv, SHUT_RDWR);
        close(ftp_srv);
        ftp_srv = -1;
    }
    int cli_fd = ftp_cli_fd;
    if (cli_fd >= 0) {
        shutdown(cli_fd, SHUT_RDWR);
        close(cli_fd);
        ftp_cli_fd = -1;
    }
    int data_fd = ftp_data;
    if (data_fd >= 0) {
        shutdown(data_fd, SHUT_RDWR);
        close(data_fd);
        ftp_data = -1;
    }
    int xfer_fd = ftp_xfer_fd;
    if (xfer_fd >= 0) {
        shutdown(xfer_fd, SHUT_RDWR);
        close(xfer_fd);
        ftp_xfer_fd = -1;
    }
    
    threadWaitForExit(&ftp_thr);
    threadClose(&ftp_thr);
    ftp_addlog("FTP server stopped");
}

// ─── MTP File Transfer ─────────────────────────────────────
static volatile int mtp_on = 0;
static u64 mtp_xfer_count = 0;
static u64 mtp_bytes_total = 0;
static char mtp_log[MTP_LOG_MAX][120];
static unsigned int mtp_log_idx = 0;
static Mutex mtp_log_mtx;
static u64 mtp_xfer_start_tick = 0;

static void mtp_addlog(const char *msg) {
    mutexLock(&mtp_log_mtx);
    snprintf(mtp_log[mtp_log_idx % MTP_LOG_MAX], sizeof(mtp_log[0]), "%s", msg);
    mtp_log_idx++;
    mutexUnlock(&mtp_log_mtx);
}

void mtp_report_activity(const char *action, const char *path, u64 bytes) {
    char msg[120];
    const char *name = path ? strrchr(path, '/') : NULL;
    name = name && name[1] ? name + 1 : (path ? path : "file");
    snprintf(msg, sizeof(msg), "%s %s (%llu bytes)", action ? action : "TRANSFER", name, (unsigned long long)bytes);
    mtp_xfer_count++;
    mtp_bytes_total += bytes;
    mtp_addlog(msg);
}

void mtp_report_event(const char *message) { mtp_addlog(message ? message : "MTP activity"); }

static void mtp_start_local(void) {
    if (mtp_on || mtp_server_is_running()) return;
    mtp_xfer_count = 0; mtp_bytes_total = 0;
    mutexLock(&mtp_log_mtx);
    mtp_log_idx = 0;
    memset(mtp_log, 0, sizeof(mtp_log));
    mutexUnlock(&mtp_log_mtx);
    Result rc = mtp_server_start();
    if (R_FAILED(rc)) { char err[96]; snprintf(err, sizeof(err), "MTP init failed (%08X)", (unsigned)rc); mtp_addlog(err); return; }
    mtp_on = 1;
    mtp_addlog("MTP started - Switch appears as MTP device on PC");
}

static void mtp_stop_local(void) {
    if (!mtp_on) return;
    mtp_on = 0;
    mtp_server_stop();
    mtp_addlog("MTP stopped");
}

// ─── System info export ───────────────────────────────────

static void export_system_info(void) {
    s32 skin = 0;
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    FILE *f = fopen("sdmc:/switch/SwitchInfoNX/system_report.txt", "w");
    if (!f) {
        snprintf(export_msg, sizeof(export_msg), "%s", s_export_error[cur_lang]);
        export_msg_tick = armGetSystemTick();
        add_alert(s_export_error[cur_lang]);
        return;
    }

    fprintf(f, "=== SwitchInfoNX System Report ===\n");
    fprintf(f, "Generated by SwitchInfoNX v" APP_VERSION " by dodosi\n\n");
    fprintf(f, "Environment: %s\n", detect_emummc_str());

    // Time
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) fprintf(f, "Date: %04d-%02d-%02d %02d:%02d:%02d\n\n",
        lt->tm_year+1900,lt->tm_mon+1,lt->tm_mday,lt->tm_hour,lt->tm_min,lt->tm_sec);

    // Firmware
    fprintf(f, "--- Firmware & Hardware ---\n");
    SetSysFirmwareVersion fw = {0};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
        fprintf(f, "Firmware: %d.%d.%d\n", fw.major, fw.minor, fw.micro);

    SetSysSerialNumber sn = {0};
    if (R_SUCCEEDED(setsysGetSerialNumber(&sn))) {
        fprintf(f, "Serial: %s\n", sn.number);
        fprintf(f, "Hardware: %s\n", detect_hardware_type(sn.number));
    }

    SetSysDeviceNickName nick = {0};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)))
        fprintf(f, "Device Name: %s\n", nick.nickname);

    SetRegion region;
    if (R_SUCCEEDED(setGetRegionCode(&region))) {
        const char *rnames[] = {"Japan","Americas","Europe","Australia/NZ","Hong Kong/Taiwan/Korea","China"};
        if ((int)region >= 0 && (int)region <= 5)
            fprintf(f, "Region: %s\n", rnames[(int)region]);
    }

    fprintf(f, "Mode: %s\n", appletGetOperationMode() ? "Docked" : "Handheld");
    fprintf(f, "\n");

    // Battery
    fprintf(f, "--- Battery ---\n");
    u32 batt = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    psmGetBatteryChargePercentage(&batt);
    psmGetChargerType(&ch);
    fprintf(f, "Battery: %u%%\n", batt);
    fprintf(f, "Charging: %s\n", ch != PsmChargerType_Unconnected ? "Yes" : "No");
    fprintf(f, "\n");

    // Storage
    fprintf(f, "--- Storage ---\n");
    FsFileSystem sd;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
        s64 fr=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&sd, "/", &fr)) && R_SUCCEEDED(fsFsGetTotalSpace(&sd, "/", &tot)))
            fprintf(f, "SD Card: %.2f GB free / %.2f GB total\n", fr/1.0e9, tot/1.0e9);
        fsFsClose(&sd);
    }
    fprintf(f, "\n");

    // Network
    fprintf(f, "--- Network ---\n");
    u32 ip=0;
    nifmGetCurrentIpAddress(&ip);
    if (ip) fprintf(f, "IP: %u.%u.%u.%u\n", ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);
    else fprintf(f, "IP: Not connected\n");
    fprintf(f, "\n");

    // Clocks
    fprintf(f, "--- Performance ---\n");
    ClkrstSession cc, cg, cm;
    u32 cpu=0, gpu=0, mem=0;
    if (R_SUCCEEDED(clkrstOpenSession(&cc, PcvModuleId_CpuBus, 3))) {
        clkrstGetClockRate(&cc,&cpu);
        fprintf(f, "CPU: %u MHz\n", cpu/1000000);
        clkrstCloseSession(&cc);
    }
    if (R_SUCCEEDED(clkrstOpenSession(&cg, PcvModuleId_GPU, 3))) {
        clkrstGetClockRate(&cg,&gpu);
        fprintf(f, "GPU: %u MHz\n", gpu/1000000);
        clkrstCloseSession(&cg);
    }
    if (R_SUCCEEDED(clkrstOpenSession(&cm, PcvModuleId_EMC, 3))) {
        clkrstGetClockRate(&cm,&mem);
        fprintf(f, "Memory: %u MHz\n", mem/1000000);
        clkrstCloseSession(&cm);
    }

    // Temps
    if (R_SUCCEEDED(tcInitialize())) {
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin)))
            fprintf(f, "Temperature: %d.%d C\n", skin/1000, (skin%1000)/100);
        tcExit();
    }

    soc_temp_update();
    if (soc_temp_valid)
        fprintf(f, "SoC Temperature: %d C\n", soc_temp_c);
    fprintf(f, "\n");

    fprintf(f, "=== End of Report ===\n");
    fclose(f);

    // Also export clean JSON for diagnostic tooling (v0.4 feature)
    FILE *fj = fopen("sdmc:/switch/SwitchInfoNX/sysinfo.json", "w");
    if (fj) {
        fprintf(fj, "{\n");
        fprintf(fj, "  \"application\": \"%s\",\n", APP_NAME);
        fprintf(fj, "  \"version\": \"%s\",\n", APP_VERSION);
        if (lt) {
            fprintf(fj, "  \"timestamp\": \"%04d-%02d-%02dT%02d:%02d:%02d\",\n",
                lt->tm_year+1900, lt->tm_mon+1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
        }
        fprintf(fj, "  \"firmware\": \"%d.%d.%d\",\n", fw.major, fw.minor, fw.micro);
        fprintf(fj, "  \"serial\": \"%s\",\n", sn.number);
        fprintf(fj, "  \"hardware\": \"%s\",\n", detect_hardware_type(sn.number));
        fprintf(fj, "  \"emummc\": \"%s\",\n", detect_emummc_str());
        fprintf(fj, "  \"operation_mode\": \"%s\",\n", appletGetOperationMode() ? "Docked" : "Handheld");
        fprintf(fj, "  \"battery\": {\n");
        fprintf(fj, "    \"charge_percent\": %u,\n", batt);
        fprintf(fj, "    \"charging\": %s,\n", ch != PsmChargerType_Unconnected ? "true" : "false");
        fprintf(fj, "    \"rate_ma\": %.0f\n", batt_rate_ma);
        fprintf(fj, "  },\n");
        fprintf(fj, "  \"clocks_mhz\": {\n");
        fprintf(fj, "    \"cpu\": %u,\n", cpu / 1000000);
        fprintf(fj, "    \"gpu\": %u,\n", gpu / 1000000);
        fprintf(fj, "    \"emc\": %u\n", mem / 1000000);
        fprintf(fj, "  },\n");
        fprintf(fj, "  \"thermals\": {\n");
        fprintf(fj, "    \"skin_c\": %.1f,\n", skin / 1000.0);
        fprintf(fj, "    \"soc_c\": %d\n", soc_temp_valid ? soc_temp_c : 0);
        fprintf(fj, "  }\n");
        fprintf(fj, "}\n");
        fclose(fj);
    }

    snprintf(export_msg, sizeof(export_msg), "%s", s_export_saved[cur_lang]);
    export_msg_tick = armGetSystemTick();
    add_alert(s_export_saved[cur_lang]);
}

static void sd_run_speed_test(void) {
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    const char *test_path = "sdmc:/switch/SwitchInfoNX/speed_test.tmp";
    FILE *f = fopen(test_path, "wb");
    if (!f) { sd_speed_result = -1; return; }
    // Write 16MB of test data
    u8 buf[65536];
    memset(buf, 0xFF, sizeof(buf));
    bool write_ok = true;
    for (int i = 0; i < 256; i++) {
        size_t w = fwrite(buf, 1, sizeof(buf), f);
        if (w != sizeof(buf)) {
            write_ok = false;
            break;
        }
    }
    fclose(f);
    if (!write_ok) {
        remove(test_path);
        sd_speed_result = -1;
        return;
    }
    // Now read it back
    f = fopen(test_path, "rb");
    if (!f) {
        remove(test_path);
        sd_speed_result = -1;
        return;
    }
    u64 total_read = 0;
    u64 read_start = armGetSystemTick();
    while (1) {
        size_t r = fread(buf, 1, sizeof(buf), f);
        total_read += r;
        if (r != sizeof(buf)) break;
    }
    u64 read_end = armGetSystemTick();
    fclose(f);
    remove(test_path);
    // Calculate read speed in MB/s
    u64 read_ticks = read_end - read_start;
    if (read_ticks > 0 && total_read > 0) {
        double sec = (double)read_ticks / armGetSystemTickFreq();
        sd_speed_result = (float)(total_read / 1048576.0 / sec);
    } else {
        sd_speed_result = -1;
    }
}

// ─── Game Cartridge Reader ─────────────────────────────────
static bool get_installed_title_name(u64 application_id, char *out, size_t out_size);

static void gamecart_update_info(void) {
    u64 now = armGetSystemTick();
    if (now - gamecart_last_check < armGetSystemTickFreq()) return;
    gamecart_last_check = now;
    bool inserted = false;
    Result rc = nsIsGameCardInserted(&inserted);
    gamecart_status_available = R_SUCCEEDED(rc);
    gamecart_present = gamecart_status_available && inserted;
}

static void batt_rate_update(void) {
    u64 now = armGetSystemTick();
    if (now - batt_rate_tick < armGetSystemTickFreq()) return;
    batt_rate_tick = now;
    
    u32 cur_pct = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetBatteryChargePercentage(&cur_pct))) return;
    psmGetChargerType(&ch);
    bool charging = ch != PsmChargerType_Unconnected;
    
    if (!batt_rate_initialized) {
        batt_prev_pct = cur_pct;
        batt_prev_tick = now;
        batt_prev_charging = charging;
        batt_rate_initialized = true;
        return;
    }
    if (charging != batt_prev_charging) {
        batt_rate_ma = 0.0f;
        batt_prev_pct = cur_pct;
        batt_prev_tick = now;
        batt_prev_charging = charging;
        return;
    }
    
    if (cur_pct != batt_prev_pct) {
        u64 dt = now - batt_prev_tick;
        double dt_sec = (double)dt / armGetSystemTickFreq();
        if (dt_sec > 0) {
            int pct_diff = (int)cur_pct - (int)batt_prev_pct;
            if (pct_diff < 0) pct_diff = -pct_diff;
            // rate(mA) = (Δ% / 100) * capacity(mAh) / (Δt / 3600 s)
            // typical Switch battery ~4310 mAh
            batt_rate_ma = (float)pct_diff * 4310.0f * 36.0f / (float)dt_sec;
            if (batt_rate_ma < 0.0f) batt_rate_ma = 0.0f;
        }
        batt_prev_pct = cur_pct;
        batt_prev_tick = now;
        batt_prev_charging = charging;
    }
}

static const char *s_batt_rate_label[LANG_MAX] = {
    "Charge/Disch. Rate", "Taux charge/dech.", "Tasso carica/scarica", "Tasa carga/descarga",
    "Lade-/Entladerate", "Taxa carga/descarga", "Laad-/ontlaadsnelheid",
    "Charge/Disch. Rate", "Charge/Disch. Rate", "Charge/Disch. Rate", "Charge/Disch. Rate"
};

// ─── Network Speed Test ────────────────────────────────────
static volatile int net_test_running = 0;
#define NET_PING_HIST_SIZE 20
static float net_ping_history[NET_PING_HIST_SIZE] = {0};
static int net_ping_history_pos = 0;
static int net_ping_history_count = 0;
static bool net_ping_capture_pending = false;

#define NET_TEST_BUF_SIZE 4096
#define NET_TEST_DURATION 5 // seconds per test
#define NET_TEST_SIZE (1024 * 1024)
static struct in_addr net_ping_addr, net_download_addr, net_upload_addr, net_fallback_addr, net_cloudflare_addr;
static int net_dl_http_status = 0, net_ul_http_status = 0;
static bool net_fallback_ready = false;
static bool net_cloudflare_ready = false;

static bool net_resolve(const char *hostname, struct in_addr *out) {
    struct hostent *host = gethostbyname(hostname);
    if (!host || !host->h_addr_list || !host->h_addr_list[0]) return false;
    memcpy(out, host->h_addr_list[0], sizeof(*out));
    return true;
}

static int net_speed_connect(struct in_addr address) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    // Let low-bandwidth links make progress; connect itself still has its own
    // bounded three-second select timeout below.
    struct timeval recv_tv = {10, 0};
    struct timeval send_tv = {10, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_tv, sizeof(recv_tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_tv, sizeof(send_tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr = address;
    int old_flags = fcntl(s, F_GETFL, 0);
    if (old_flags < 0 || fcntl(s, F_SETFL, old_flags | O_NONBLOCK) < 0) { close(s); return -1; }
    int rc = connect(s, (struct sockaddr*)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) { close(s); return -1; }
    if (rc < 0) {
        fd_set writefds;
        FD_ZERO(&writefds); FD_SET(s, &writefds);
        struct timeval connect_timeout = {3, 0};
        rc = select(s + 1, NULL, &writefds, NULL, &connect_timeout);
        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);
        if (rc <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) < 0 || socket_error) {
            close(s); return -1;
        }
    }
    if (fcntl(s, F_SETFL, old_flags) < 0) { close(s); return -1; }
    return s;
}

static int net_send_all(int s, const char *data, size_t size) {
    size_t sent = 0;
    while (sent < size && net_test_running) {
        int n = (int)send(s, data + sent, size - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return sent == size ? 0 : -1;
}

static int net_read_http_headers(int s, int *status_out) {
    char c, prev[4] = {0}, status[64] = {0};
    size_t status_len = 0;
    bool status_done = false;
    for (int i = 0; i < 8192 && net_test_running; ++i) {
        int n = (int)recv(s, &c, 1, 0);
        if (n <= 0) return -1;
        if (!status_done && status_len < sizeof(status) - 1) {
            status[status_len++] = c;
            if (status_len >= 2 && status[status_len - 2] == '\r' && status[status_len - 1] == '\n') {
                status_done = true;
                int code = 0;
                if (sscanf(status, "HTTP/%*s %d", &code) != 1) return -1;
                if (status_out) *status_out = code;
                if (code < 200 || code >= 300) return -1;
            }
        }
        prev[0] = prev[1]; prev[1] = prev[2]; prev[2] = prev[3]; prev[3] = c;
        if (prev[0] == '\r' && prev[1] == '\n' && prev[2] == '\r' && prev[3] == '\n') return 0;
    }
    return -1;
}

static float net_download_from(struct in_addr address, const char *host, const char *path, const char *agent, const char *connection, int *http_status) {
    int s = net_speed_connect(address);
    if (s < 0) return -1.0f;
    char req[320];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: %s\r\n\r\n", path, host, agent, connection);
    if (net_send_all(s, req, strlen(req)) < 0 || net_read_http_headers(s, http_status) < 0) {
        close(s); return -1.0f;
    }
    u64 total = 0;
    u64 start = armGetSystemTick();
    u64 deadline = start + armGetSystemTickFreq() * NET_TEST_DURATION;
    char buf[NET_TEST_BUF_SIZE];
    while (net_test_running && armGetSystemTick() < deadline) {
        u64 now = armGetSystemTick();
        u64 remaining = deadline - now;
        u64 remaining_us = remaining * 1000000ULL / armGetSystemTickFreq();
        struct timeval wait = {(long)(remaining_us / 1000000ULL), (long)(remaining_us % 1000000ULL)};
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        int ready = select(s + 1, &readfds, NULL, NULL, &wait);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        int n = recv(s, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        total += (u64)n;
    }
    double sec = (double)(armGetSystemTick() - start) / armGetSystemTickFreq();
    close(s);
    return (total && sec > 0.05) ? (float)(total * 8.0 / sec / 1000000.0) : -1.0f;
}

static void net_test_download(void) {
    net_dl_http_status = 0;
    net_dl_mbps = -1.0f;
    char path[96];
    snprintf(path, sizeof(path), "/__down?bytes=10000000&measId=%llu", (unsigned long long)armGetSystemTick());
    // Cloudflare's own speed-test endpoints are designed for streamed GET
    // measurements and run on its global edge network.
    if (net_cloudflare_ready)
        net_dl_mbps = net_download_from(net_cloudflare_addr, "speed.cloudflare.com", path, "SwitchInfoNX/0.5", "close", &net_dl_http_status);
    if (net_dl_mbps > 0 || !net_test_running) return;
    if (net_fallback_ready) {
        int fallback_status = 0;
        float fallback = net_download_from(net_fallback_addr, "speedtest.tele2.net", "/10MB.zip", "SwitchInfoNX/0.5", "close", &fallback_status);
        if (fallback > 0) { net_dl_mbps = fallback; net_dl_http_status = fallback_status; }
        else if (fallback_status) net_dl_http_status = fallback_status;
    }
    if (net_dl_mbps > 0 || !net_test_running) return;
    int fallback_status = 0;
    float fallback = net_download_from(net_download_addr, "ctest-dl-lp1.cdn.nintendo.net", "/30m", "Nintendo NX", "keep-alive", &fallback_status);
    if (fallback > 0) { net_dl_mbps = fallback; net_dl_http_status = fallback_status; }
    else if (fallback_status) net_dl_http_status = fallback_status;
}

static float net_upload_to(struct in_addr address, const char *host, const char *method, const char *path,
                           const char *version, const char *agent, const char *content_type, const char *connection, bool close_write, int *http_status) {
    int s = net_speed_connect(address);
    if (s < 0) return -1.0f;
    int send_buffer_size = 32768;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&send_buffer_size, sizeof(send_buffer_size));
    char header[320];
    snprintf(header, sizeof(header), "%s %s HTTP/%s\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nContent-Length: %d\r\nContent-Type: %s\r\nConnection: %s\r\n\r\n",
        method, path, version, host, agent, NET_TEST_SIZE, content_type, connection);
    if (net_send_all(s, header, strlen(header)) < 0) { close(s); return -1.0f; }
    char payload[4096];
    memset(payload, 'A', sizeof(payload));
    u64 total = 0;
    u64 start = armGetSystemTick();
    u64 deadline = start + armGetSystemTickFreq() * 15;
    while (net_test_running && total < NET_TEST_SIZE && armGetSystemTick() < deadline) {
        size_t chunk = (size_t)(NET_TEST_SIZE - total);
        if (chunk > sizeof(payload)) chunk = sizeof(payload);
        if (net_send_all(s, payload, chunk) < 0) break;
        total += chunk;
    }
    double sec = (double)(armGetSystemTick() - start) / armGetSystemTickFreq();
    if (close_write) shutdown(s, SHUT_WR);
    // Require the server to acknowledge the upload, but keep response latency
    // out of the throughput interval.
    int response_ok = net_read_http_headers(s, http_status) == 0;
    close(s);
    return (total == NET_TEST_SIZE && response_ok && sec > 0.05) ? (float)(total * 8.0 / sec / 1000000.0) : -1.0f;
}

static void net_test_upload(void) {
    net_ul_http_status = 0;
    net_ul_mbps = -1.0f;
    char path[64];
    snprintf(path, sizeof(path), "/__up?measId=%llu", (unsigned long long)armGetSystemTick());
    if (net_cloudflare_ready)
        net_ul_mbps = net_upload_to(net_cloudflare_addr, "speed.cloudflare.com", "POST", path, "1.1", "SwitchInfoNX/0.5", "application/octet-stream", "close", true, &net_ul_http_status);
    if (net_ul_mbps > 0 || !net_test_running) return;
    if (net_fallback_ready) {
        int fallback_status = 0;
        float fallback = net_upload_to(net_fallback_addr, "speedtest.tele2.net", "PUT", "/upload.php", "1.1", "SwitchInfoNX/0.5", "application/octet-stream", "close", true, &fallback_status);
        if (fallback > 0) { net_ul_mbps = fallback; net_ul_http_status = fallback_status; }
        else if (fallback_status) net_ul_http_status = fallback_status;
    }
    if (net_ul_mbps > 0 || !net_test_running) return;
    int fallback_status = 0;
    float fallback = net_upload_to(net_upload_addr, "ctest-ul-lp1.cdn.nintendo.net", "POST", "/1m", "1.0", "Nintendo NX", "application/x-www-form-urlencoded", "keep-alive", false, &fallback_status);
    if (fallback > 0) { net_ul_mbps = fallback; net_ul_http_status = fallback_status; }
    else if (fallback_status) net_ul_http_status = fallback_status;
}

static void net_test_ping(void) {
    float pings[2];
    int ping_count = 0;
    for (int pi = 0; pi < 2 && net_test_running; pi++) {
        u64 t0 = armGetSystemTick();
        int s = net_speed_connect(net_ping_addr);
        if (s < 0) continue;
        u64 dt = armGetSystemTick() - t0;
        pings[ping_count++] = (float)((double)dt / armGetSystemTickFreq() * 1000.0);
        close(s);
    }
    if (ping_count > 0) {
        float sum = 0, minv = pings[0], maxv = pings[0];
        for (int i = 0; i < ping_count; i++) {
            sum += pings[i];
            if (pings[i] < minv) minv = pings[i];
            if (pings[i] > maxv) maxv = pings[i];
        }
        net_ping_ms = sum / ping_count;
        net_jitter_ms = maxv - minv;
    } else {
        net_ping_ms = -1;
        net_jitter_ms = -1;
    }
}

static void net_test_thread(void *arg) {
    (void)arg;
    net_dl_mbps = 0;
    net_ul_mbps = 0;
    net_ping_ms = 0;
    net_jitter_ms = 0;
    net_dl_http_status = net_ul_http_status = 0;
    if (!net_resolve("ctest.cdn.nintendo.net", &net_ping_addr) ||
        !net_resolve("ctest-dl-lp1.cdn.nintendo.net", &net_download_addr) ||
        !net_resolve("ctest-ul-lp1.cdn.nintendo.net", &net_upload_addr)) {
        net_ping_ms = net_dl_mbps = net_ul_mbps = net_jitter_ms = -1;
        net_test_running = 0;
        return;
    }
    net_cloudflare_ready = net_resolve("speed.cloudflare.com", &net_cloudflare_addr);
    net_fallback_ready = net_resolve("speedtest.tele2.net", &net_fallback_addr);
    
    net_test_ping();
    if (!net_test_running) { net_test_running = 0; return; }
    net_test_download();
    if (!net_test_running) { net_test_running = 0; return; }
    net_test_upload();
    
    net_test_running = 0;
}

static bool net_test_started = false;

static void net_test_cleanup(void) {
    if (!net_test_started) return;
    if (net_test_running) {
        net_test_running = 0;
        threadWaitForExit(&net_test_thr);
    }
    threadClose(&net_test_thr);
}

static void net_test_start(void) {
    if (net_test_running) return;
    // Reap the previous thread handle if a prior test already finished,
    // so repeated runs don't leak thread handles.
    if (net_test_started) {
        threadWaitForExit(&net_test_thr);
        threadClose(&net_test_thr);
    }
    net_test_running = 1;
    Result rc = threadCreate(&net_test_thr, net_test_thread, NULL, NULL, 16384, 0x2B, -2);
    if (R_FAILED(rc)) { net_test_running = 0; net_dl_mbps = net_ul_mbps = -1; return; }
    rc = threadStart(&net_test_thr);
    if (R_FAILED(rc)) { net_test_running = 0; threadClose(&net_test_thr); net_dl_mbps = net_ul_mbps = -1; return; }
    net_test_started = true;
    net_ping_capture_pending = true;
}

// --- CFW Anti-Ban & Telemetry Shield ---
typedef enum {
    ANTIBAN_NOT_TESTED = 0,
    ANTIBAN_TESTING,
    ANTIBAN_SAFE_BLOCKED,
    ANTIBAN_UNBLOCKED_ALERT
} AntiBanState;

typedef struct {
    const char *domain;
    char resolved_ip[32];
    AntiBanState state;
} AntiBanTarget;

static AntiBanTarget antiban_targets[3] = {
    {"ctest.cdn.nintendo.net", "-", ANTIBAN_NOT_TESTED},
    {"dauth-lp1.ndas.srv.nintendo.net", "-", ANTIBAN_NOT_TESTED},
    {"receive-lp1.dg.srv.nintendo.net", "-", ANTIBAN_NOT_TESTED}
};
static AntiBanState antiban_overall = ANTIBAN_NOT_TESTED;
static volatile int antiban_running = 0;
static Thread antiban_thr;
static bool antiban_thr_started = false;

static void antiban_thread_func(void *arg) {
    (void)arg;
    antiban_overall = ANTIBAN_TESTING;
    bool any_unblocked = false;

    for (int i = 0; i < 3 && antiban_running; i++) {
        antiban_targets[i].state = ANTIBAN_TESTING;
        struct hostent *he = gethostbyname(antiban_targets[i].domain);
        if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
            snprintf(antiban_targets[i].resolved_ip, sizeof(antiban_targets[i].resolved_ip), "Blocked (NXDOMAIN)");
            antiban_targets[i].state = ANTIBAN_SAFE_BLOCKED;
        } else {
            struct in_addr in;
            memcpy(&in, he->h_addr_list[0], sizeof(in));
            const char *res = inet_ntop(AF_INET, &in, antiban_targets[i].resolved_ip, sizeof(antiban_targets[i].resolved_ip));
            if (!res) snprintf(antiban_targets[i].resolved_ip, sizeof(antiban_targets[i].resolved_ip), "0.0.0.0");

            if (strcmp(antiban_targets[i].resolved_ip, "0.0.0.0") == 0 ||
                strncmp(antiban_targets[i].resolved_ip, "127.", 4) == 0) {
                antiban_targets[i].state = ANTIBAN_SAFE_BLOCKED;
            } else {
                int s = socket(AF_INET, SOCK_STREAM, 0);
                if (s >= 0) {
                    struct sockaddr_in saddr;
                    memset(&saddr, 0, sizeof(saddr));
                    saddr.sin_family = AF_INET;
                    saddr.sin_port = htons(80);
                    saddr.sin_addr = in;

                    int old_flags = fcntl(s, F_GETFL, 0);
                    if (old_flags >= 0) fcntl(s, F_SETFL, old_flags | O_NONBLOCK);
                    int rc = connect(s, (struct sockaddr*)&saddr, sizeof(saddr));
                    if (rc < 0 && errno == EINPROGRESS) {
                        fd_set wfds;
                        FD_ZERO(&wfds);
                        FD_SET(s, &wfds);
                        struct timeval tv = {1, 500000};
                        if (select(s + 1, NULL, &wfds, NULL, &tv) > 0) {
                            int err = 0;
                            socklen_t len = sizeof(err);
                            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                                antiban_targets[i].state = ANTIBAN_UNBLOCKED_ALERT;
                                any_unblocked = true;
                            } else {
                                antiban_targets[i].state = ANTIBAN_SAFE_BLOCKED;
                            }
                        } else {
                            antiban_targets[i].state = ANTIBAN_SAFE_BLOCKED;
                        }
                    } else if (rc == 0) {
                        antiban_targets[i].state = ANTIBAN_UNBLOCKED_ALERT;
                        any_unblocked = true;
                    } else {
                        antiban_targets[i].state = ANTIBAN_SAFE_BLOCKED;
                    }
                    close(s);
                } else {
                    antiban_targets[i].state = ANTIBAN_SAFE_BLOCKED;
                }
            }
        }
    }

    if (any_unblocked) {
        antiban_overall = ANTIBAN_UNBLOCKED_ALERT;
        add_alert(cur_lang == 1 ? "Attention: Telemetrie NON bloquee!" : "Warning: Telemetry UNBLOCKED!");
    } else {
        antiban_overall = ANTIBAN_SAFE_BLOCKED;
        add_alert(cur_lang == 1 ? "Bouclier Anti-Ban Actif: Protege!" : "Anti-Ban Shield: Protected!");
    }
    antiban_running = 0;
}

static void antiban_start_test(void) {
    if (antiban_running) return;
    if (antiban_thr_started) {
        threadWaitForExit(&antiban_thr);
        threadClose(&antiban_thr);
        antiban_thr_started = false;
    }
    antiban_running = 1;
    antiban_overall = ANTIBAN_TESTING;
    for (int i = 0; i < 3; i++) antiban_targets[i].state = ANTIBAN_TESTING;
    Result rc = threadCreate(&antiban_thr, antiban_thread_func, NULL, NULL, 0x8000, 0x2B, -2);
    if (R_SUCCEEDED(rc)) {
        antiban_thr_started = true;
        threadStart(&antiban_thr);
    } else {
        antiban_running = 0;
        antiban_overall = ANTIBAN_NOT_TESTED;
    }
}

static void antiban_cleanup(void) {
    if (antiban_running) {
        antiban_running = 0;
        threadWaitForExit(&antiban_thr);
    }
    if (antiban_thr_started) {
        threadClose(&antiban_thr);
        antiban_thr_started = false;
    }
}

static const char *s_net_test_label[LANG_MAX] = {
    "Network Speed Test", "Test vitesse reseau", "Test velocita rete", "Prueba velocidad red",
    "Netzwerk-Geschw.-Test", "Teste de velocidade de rede", "Netwerksnelheidstest",
    "ネットワーク速度テスト", "Тест скорости сети", "网络测速", "네트워크 속도 테스트"
};
static const char *s_net_ping_history[LANG_MAX] = {
    "TCP connect history (last 20 tests)", "Historique connexions TCP (20 derniers tests)",
    "Storico connessioni TCP (ultimi 20 test)", "Historial de conexion TCP (ultimas 20 pruebas)",
    "TCP-Verbindungsverlauf (letzte 20 Tests)", "Historico de conexao TCP (ultimos 20 testes)",
    "TCP-verbindingsgeschiedenis (laatste 20 tests)", "TCP connect history (last 20 tests)",
    "TCP connect history (last 20 tests)", "TCP connect history (last 20 tests)", "TCP connect history (last 20 tests)"};
static const char *s_tcp_connect_fmt[LANG_MAX] = {
    "TCP connect: %.0f ms (range %.0f)", "Connexion TCP : %.0f ms (ecart %.0f)",
    "Connessione TCP: %.0f ms (intervallo %.0f)", "Conexion TCP: %.0f ms (rango %.0f)",
    "TCP-Verbindung: %.0f ms (Spanne %.0f)", "Conexao TCP: %.0f ms (intervalo %.0f)",
    "TCP-verbinding: %.0f ms (bereik %.0f)", "TCP connect: %.0f ms (range %.0f)",
    "TCP connect: %.0f ms (range %.0f)", "TCP connect: %.0f ms (range %.0f)",
    "TCP connect: %.0f ms (range %.0f)"};
static const char *s_net_dl[LANG_MAX] = {
    "Download: %.1f Mbps", "Download: %.1f Mbps", "Download: %.1f Mbps", "Descarga: %.1f Mbps",
    "Download: %.1f Mbps", "Download: %.1f Mbps", "Download: %.1f Mbps",
    "ダウンロード: %.1f Mbps", "Загрузка: %.1f Мбит/с", "下载：%.1f Mbps", "다운로드: %.1f Mbps"
};
static const char *s_net_ul[LANG_MAX] = {
    "Upload: %.1f Mbps", "Upload: %.1f Mbps", "Upload: %.1f Mbps", "Subida: %.1f Mbps",
    "Upload: %.1f Mbps", "Upload: %.1f Mbps", "Upload: %.1f Mbps",
    "アップロード: %.1f Mbps", "Отдача: %.1f Мбит/с", "上传：%.1f Mbps", "업로드: %.1f Mbps"
};
static const char *s_net_test_btn[LANG_MAX] = {
    "[Touch] Run Speed Test", "[Touch] Lancer test", "[Touch] Avvia test", "[Touch] Iniciar prueba",
    "[Touch] Geschw.-Test", "[Touch] Iniciar teste", "[Touch] Snelheidstest",
    "[タッチ] 速度テスト開始", "[Касание] Начать тест", "[触摸] 开始测速", "[터치] 속도 테스트 실행"
};
static const char *s_net_testing[LANG_MAX] = {
    "Testing...", "Test...", "Test...", "Probando...",
    "Test...", "Testando...", "Testen...",
    "測定中…", "Проверка…", "测速中…", "측정 중…"
};

// ─── CPU/GPU Frequency Scaling Control ─────────────────────
static const char *s_scale_title[LANG_MAX] = {
    "CPU/GPU Frequency Scaling", "Scaling frequences CPU/GPU", "Scaling frequenze CPU/GPU", "Escalado frec. CPU/GPU",
    "CPU/GPU-Takt-Skalierung", "Escalonamento freq. CPU/GPU", "CPU/GPU-frequentieschaling",
    "CPU/GPU Frequency Scaling", "CPU/GPU Frequency Scaling", "CPU/GPU Frequency Scaling", "CPU/GPU Frequency Scaling"
};
static const char *s_scale_auto[LANG_MAX] = {
    "Auto (Default)", "Auto (Defaut)", "Auto (Predefinito)", "Auto (Predet.)",
    "Auto (Standard)", "Auto (Padrao)", "Auto (Standaard)",
    "Auto (Default)", "Auto (Default)", "Auto (Default)", "Auto (Default)"
};
static const char *s_scale_set[LANG_MAX] = {
    "Touch sliders | [ZR] CPU [ZL] GPU | [L+R] Reset",
    "Toucher curseurs | [ZR] CPU [ZL] GPU | [L+R] Reinit",
    "Tocca cursori | [ZR] CPU [ZL] GPU | [L+R] Reset",
    "Toca controles | [ZR] CPU [ZL] GPU | [L+R] Restab.",
    "Regler antippen | [ZR] CPU [ZL] GPU | [L+R] Reset",
    "Toque controles | [ZR] CPU [ZL] GPU | [L+R] Reinic.",
    "Tik op schuiven | [ZR] CPU [ZL] GPU | [L+R] Reset",
    "スライダーをタッチ | [ZR] CPU [ZL] GPU | [L+R] リセット",
    "Коснитесь ползунков | [ZR] CPU [ZL] GPU | [L+R] сброс",
    "触摸滑块 | [ZR] CPU [ZL] GPU | [L+R] 重置",
    "슬라이더 터치 | [ZR] CPU [ZL] GPU | [L+R] 초기화"
};
static const char *s_scale_cpu_fmt[LANG_MAX] = {
    "CPU Target: %d MHz (%s)", "CPU Cible: %d MHz (%s)", "CPU Target: %d MHz (%s)", "CPU Objetivo: %d MHz (%s)",
    "CPU Ziel: %d MHz (%s)", "CPU Alvo: %d MHz (%s)", "CPU Doel: %d MHz (%s)",
    "CPU Target: %d MHz (%s)", "CPU Target: %d MHz (%s)", "CPU Target: %d MHz (%s)", "CPU Target: %d MHz (%s)"
};
static const char *s_scale_gpu_fmt[LANG_MAX] = {
    "GPU Target: %d MHz (%s)", "GPU Cible: %d MHz (%s)", "GPU Target: %d MHz (%s)", "GPU Objetivo: %d MHz (%s)",
    "GPU Ziel: %d MHz (%s)", "GPU Alvo: %d MHz (%s)", "GPU Doel: %d MHz (%s)",
    "GPU Target: %d MHz (%s)", "GPU Target: %d MHz (%s)", "GPU Target: %d MHz (%s)", "GPU Target: %d MHz (%s)"
};

static Result scale_set_cpu(int mhz, int *applied_mhz) {
    return scale_set_clock(PcvModuleId_CpuBus, mhz, applied_mhz);
}
static Result scale_set_gpu(int mhz, int *applied_mhz) {
    return scale_set_clock(PcvModuleId_GPU, mhz, applied_mhz);
}
static Result scale_set_mem(int mhz, int *applied_mhz) {
    return scale_set_clock(PcvModuleId_EMC, mhz, applied_mhz);
}
static Result scale_reset_all(void) {
    int applied = 0;
    // CpuBus default ~1020MHz handheld, 1785MHz docked
    Result rc = scale_set_cpu(appletGetOperationMode() ? 1785 : 1020, &applied);
    if (R_FAILED(rc)) return rc;
    rc = scale_set_gpu(appletGetOperationMode() ? 768 : 307, &applied);
    if (R_FAILED(rc)) return rc;
    rc = scale_set_mem(1600, &applied);
    if (R_FAILED(rc)) return rc;
    cpu_scale_target_mhz = 0;
    gpu_scale_target_mhz = 0;
    mem_scale_target_mhz = 0;
    return 0;
}

// ─── Game Usage Stats ──────────────────────────────────────
#define STATS_PATH "sdmc:/switch/SwitchInfoNX/game_stats.csv"
static int game_stats_count = 0;
static u64 game_stats_ids[256];
static int game_stats_launches[256];
static u64 game_stats_seconds[256];
static char game_stats_names[256][64];

static void load_game_stats(void) {
    game_stats_count = 0;
    memset(game_stats_ids, 0, sizeof(game_stats_ids));
    memset(game_stats_launches, 0, sizeof(game_stats_launches));
    memset(game_stats_seconds, 0, sizeof(game_stats_seconds));
    memset(game_stats_names, 0, sizeof(game_stats_names));
    FILE *f = fopen(STATS_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && game_stats_count < 256) {
        u64 id; int launches; u64 secs; char name[64];
        if (sscanf(line, "%lx,%d,%lu,%63s", &id, &launches, &secs, name) >= 3) {
            game_stats_ids[game_stats_count] = id;
            game_stats_launches[game_stats_count] = launches;
            game_stats_seconds[game_stats_count] = secs;
            // Extract name from CSV (might have spaces)
            char *p = strchr(line, ',');
            if (p) p = strchr(p+1, ',');
            if (p) p = strchr(p+1, ',');
            if (p) {
                p++;
                int nl = (int)strlen(p);
                if (nl > 0 && p[nl-1] == '\n') p[nl-1] = 0;
                strncpy(game_stats_names[game_stats_count], p, sizeof(game_stats_names[0])-1);
            }
            game_stats_count++;
        }
    }
    fclose(f);
}

static void save_game_stats(void) {
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    FILE *f = fopen(STATS_PATH, "w");
    if (!f) return;
    for (int i = 0; i < game_stats_count; i++) {
        fprintf(f, "%016lX,%d,%lu,%s\n",
            (unsigned long)game_stats_ids[i],
            game_stats_launches[i],
            (unsigned long)game_stats_seconds[i],
            game_stats_names[i]);
    }
    fclose(f);
}

// Seed stats from installed games on first scan
static void seed_game_stats(void) {
    bool renamed = false;
    for (int si = 0; si < game_stats_count; ++si) {
        for (int gi = 0; gi < installed_game_count; ++gi) {
            if (game_stats_ids[si] == installed_game_ids[gi] && installed_game_names[gi][0] &&
                strcmp(game_stats_names[si], installed_game_names[gi]) != 0) {
                size_t name_len = strnlen(installed_game_names[gi], sizeof(installed_game_names[gi]));
                if (name_len >= sizeof(game_stats_names[si])) name_len = sizeof(game_stats_names[si]) - 1;
                memcpy(game_stats_names[si], installed_game_names[gi], name_len);
                game_stats_names[si][name_len] = 0;
                renamed = true;
                break;
            }
        }
    }
    if (renamed) save_game_stats();
    if (game_stats_count == 0 && installed_game_count > 0) {
        for (int i = 0; i < installed_game_count; i++) {
            if (game_stats_count >= 256) break;
            game_stats_ids[game_stats_count] = installed_game_ids[i];
            game_stats_launches[game_stats_count] = 0;
            game_stats_seconds[game_stats_count] = 0;
            strncpy(game_stats_names[game_stats_count], installed_game_names[i], sizeof(game_stats_names[0])-1);
            game_stats_count++;
        }
        save_game_stats();
    }
}

// ─── Installed Games Scanner ───────────────────────────────
static bool get_installed_title_name(u64 application_id, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    NsApplicationControlData *ctrl = (NsApplicationControlData *)malloc(sizeof(*ctrl));
    if (!ctrl) return false;
    u64 actual_size = 0;
    bool found = false;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
            application_id, ctrl, sizeof(*ctrl), &actual_size)) && actual_size >= sizeof(NacpStruct)) {
        NacpLanguageEntry *entry = NULL;
        if (R_SUCCEEDED(nsGetApplicationDesiredLanguage(&ctrl->nacp, &entry)) && entry && entry->name[0]) {
            size_t name_len = strnlen(entry->name, sizeof(entry->name));
            if (name_len >= out_size) name_len = out_size - 1;
            memcpy(out, entry->name, name_len);
            out[name_len] = 0;
            found = true;
        }
        // Some homebrew titles have no desired-language match; choose their first
        // populated NACP name instead of showing the title ID as the game name.
        if (!found) {
            for (int i = 0; i < 16; i++) {
                if (ctrl->nacp.lang[i].name[0]) {
                    size_t name_len = strnlen(ctrl->nacp.lang[i].name, sizeof(ctrl->nacp.lang[i].name));
                    if (name_len >= out_size) name_len = out_size - 1;
                    memcpy(out, ctrl->nacp.lang[i].name, name_len);
                    out[name_len] = 0;
                    found = true;
                    break;
                }
            }
        }
    }
    free(ctrl);
    return found;
}

static const char *s_unknown_game[LANG_MAX];

static void scan_installed_games(void) {
    installed_game_count = 0;
    memset(installed_game_ids, 0, sizeof(installed_game_ids));
    memset(installed_game_names, 0, sizeof(installed_game_names));
    
    NsApplicationRecord rec;
    s32 total = 0;
    u64 offset = 0;
    
    while (R_SUCCEEDED(nsListApplicationRecord(&rec, 1, offset, &total)) && total > 0 && installed_game_count < MAX_GAMES) {
        installed_game_ids[installed_game_count] = rec.application_id;
        
        if (!get_installed_title_name(rec.application_id, installed_game_names[installed_game_count], sizeof(installed_game_names[0])))
            snprintf(installed_game_names[installed_game_count], sizeof(installed_game_names[0]),
                "Title ID: %016llX", (unsigned long long)rec.application_id);
        
        installed_game_count++;
        offset++;  // v0.3 fix: use list index, not application_id
    }
    installed_games_scanned = true;
}

static const char *s_installed_games[LANG_MAX] = {
    "Installed titles (%d)", "Titres installes (%d)", "Titoli installati (%d)", "Titulos instalados (%d)",
    "Installierte Titel (%d)", "Titulos instalados (%d)", "Geinstalleerde titels (%d)",
    "インストール済みタイトル (%d)", "Установленные приложения (%d)", "已安装标题 (%d)", "설치된 타이틀 (%d)"
};
static const char *s_running_games[LANG_MAX] = {
    "Running Games", "Jeux en cours", "Giochi in esecuzione", "Juegos en ejecucion", "Laufende Spiele", "Jogos em execucao", "Actieve games",
    "実行中のゲーム", "Запущенные игры", "正在运行的游戏", "실행 중인 게임"};
static const char *s_no_running_games[LANG_MAX] = {
    "No running games detected", "Aucun jeu en cours detecte", "Nessun gioco in esecuzione", "No se detectan juegos en ejecucion", "Keine laufenden Spiele erkannt", "Nenhum jogo em execucao", "Geen actieve games gevonden",
    "実行中のゲームはありません", "Запущенных игр нет", "未检测到运行中的游戏", "실행 중인 게임이 없습니다"};
static const char *s_save_manager_title[LANG_MAX] = {
    "Game Save Manager", "Sauvegardes de jeux", "Gestione salvataggi", "Copias de juegos", "Spielstand-Manager", "Gestor de saves", "Spelopslagbeheer",
    "ゲームセーブ管理", "Менеджер сохранений", "游戏存档管理", "게임 저장 관리"};
static const char *s_unknown_game[LANG_MAX] = {
    "Unknown game", "Jeu inconnu", "Gioco sconosciuto", "Juego desconocido", "Unbekanntes Spiel", "Jogo desconhecido", "Onbekende game",
    "不明なゲーム", "Неизвестная игра", "未知游戏", "알 수 없는 게임"};
static const char *s_shared_save[LANG_MAX] = {
    "Shared save", "Sauvegarde partagee", "Salvataggio condiviso", "Guardado compartido", "Gemeinsamer Spielstand", "Save compartilhado", "Gedeelde opslag",
    "共有セーブ", "Общее сохранение", "共享存档", "공용 저장 데이터"};
static const char *s_profile_unavailable[LANG_MAX] = {
    "Profile unavailable", "Profil indisponible", "Profilo non disponibile", "Perfil no disponible", "Profil nicht verfuegbar", "Perfil indisponivel", "Profiel niet beschikbaar",
    "プロフィールを取得できません", "Профиль недоступен", "无法读取用户资料", "프로필을 사용할 수 없음"};
static const char *s_save_manager_hint[LANG_MAX] = {
    "Backups are stored on SD. Tap the backup selector to cycle; restore makes a safety copy. Missing profile? Relaunch in title mode.",
    "Copies sur SD. Touchez le selecteur pour changer; la restauration cree une copie de securite.",
    "Backup su SD. Tocca il selettore per cambiare; il ripristino crea prima una copia.",
    "Copias en SD. Toca el selector para cambiar; restaurar crea antes una copia de seguridad.",
    "Sicherungen auf SD. Sicherungsauswahl antippen; vor dem Wiederherstellen wird gesichert.",
    "Copias no SD. Toque no seletor para trocar; o restauro cria primeiro uma copia de seguranca.",
    "Back-ups op SD. Tik op de selector om te wisselen; herstellen maakt eerst een veiligheidskopie.",
    "SDに保存。セレクターをタップして切り替えます。復元前に現在のデータを保存します。",
    "Копии на SD. Коснитесь переключателя; перед восстановлением создаётся копия.",
    "备份保存在 SD。点按选择器切换；恢复前会先备份当前存档。",
    "SD에 백업합니다. 선택기를 눌러 변경하세요. 복원 전에 현재 데이터를 백업합니다."};
static const char *s_save_backup_btn[LANG_MAX] = {
    "BACK UP", "SAUVEGARDER", "BACKUP", "COPIAR", "SICHERN", "BACKUP", "BACK-UP",
    "バックアップ", "СОЗДАТЬ КОПИЮ", "备份", "백업"};
static const char *s_save_backup_history[LANG_MAX] = {
    "Backup", "Sauvegarde", "Backup", "Copia", "Sicherung", "Backup", "Back-up",
    "バックアップ", "Копия", "备份", "백업"};
static const char *s_save_backup_success[LANG_MAX] = {
    "Backup completed", "Sauvegarde effectuee", "Backup completato", "Copia completada",
    "Sicherung abgeschlossen", "Backup concluido", "Back-up voltooid",
    "バックアップ完了", "Копия создана", "备份已完成", "백업 완료"};
static const char *s_save_backup_failure[LANG_MAX] = {
    "Backup failed", "Sauvegarde non effectuee", "Backup non riuscito", "Copia no realizada",
    "Sicherung fehlgeschlagen", "Backup nao concluido", "Back-up mislukt",
    "バックアップ失敗", "Не удалось создать копию", "备份失败", "백업 실패"};
static const char *s_save_backup_selector[LANG_MAX] = {
    "Backup %d/%d", "Sauvegarde %d/%d", "Backup %d/%d", "Copia %d/%d",
    "Sicherung %d/%d", "Backup %d/%d", "Back-up %d/%d",
    "バックアップ %d/%d", "Копия %d/%d", "备份 %d/%d", "백업 %d/%d"};
static const char *s_save_no_backups[LANG_MAX] = {
    "No backups yet", "Aucune sauvegarde", "Nessun backup", "Sin copias", "Keine Sicherungen",
    "Sem backups", "Nog geen back-ups", "バックアップなし", "Нет копий", "暂无备份", "백업 없음"};
static const char *s_save_restore_btn[LANG_MAX] = {
    "RESTORE SELECTED", "RESTAURER", "RIPRISTINA", "RESTAURAR", "AUSWAHL WIEDERHERSTELLEN", "RESTAURAR", "HERSTEL",
    "選択を復元", "ВОССТАНОВИТЬ ВЫБРАННОЕ", "恢复所选", "선택 항목 복원"};
static const char *s_save_restore_confirm_btn[LANG_MAX] = {
    "CONFIRM RESTORE", "CONFIRMER", "CONFERMA", "CONFIRMAR", "WIRKLICH?", "CONFIRMAR", "BEVESTIG",
    "復元を確認", "ПОДТВЕРДИТЬ", "确认恢复", "복원 확인"};

// ─── Header bar ───────────────────────────────────────────

static void draw_header(SDL_Renderer *r) {
    // Gradient background
    SDL_Rect bg = {0, 0, W, 50};
    SDL_SetRenderDrawColor(r, color_bg2.r, color_bg2.g, color_bg2.b, 255);
    SDL_RenderFillRect(r, &bg);
    thickLineRGBA(r, 0, 50, W, 50, 2, color_card_border.r, color_card_border.g, color_card_border.b, 255);

    // Cyan accent dot + title
    filledCircleRGBA(r, 16, 25, 5, color_cyan.r, color_cyan.g, color_cyan.b, 255);
    draw_text(r, font_md, "Switch Info NX", 30, 11, color_white, 0);

    // Version badge pill
    draw_rounded_box(r, 222, 13, 56, 24, 6, color_bg3);
    draw_rounded_rect(r, 222, 13, 56, 24, 6, color_card_border);
    draw_text(r, font_xs, "v" APP_VERSION, 250, 17, color_cyan, 1);

    // Refresh count badge + auto-refresh countdown
    char ref_str[32];
    int ref_w = 48;
    if (auto_refresh_interval > 0) {
        u64 elapsed = armGetSystemTick() - last_refresh;
        int remain = auto_refresh_interval - (int)(elapsed / armGetSystemTickFreq());
        if (remain < 0) remain = 0;
        snprintf(ref_str, sizeof(ref_str), "[%u] %ds", refresh_count, remain);
        ref_w = 78;
        draw_rounded_box(r, 286, 13, ref_w, 24, 6, color_bg3);
        draw_rounded_rect(r, 286, 13, ref_w, 24, 6, color_card_border);
        draw_text(r, font_xs, ref_str, 286 + ref_w / 2, 17, remain <= 3 ? color_yellow : color_grey, 1);
    } else {
        snprintf(ref_str, sizeof(ref_str), "[%u]", refresh_count);
        draw_rounded_box(r, 286, 13, ref_w, 24, 6, color_bg3);
        draw_rounded_rect(r, 286, 13, ref_w, 24, 6, color_card_border);
        draw_text(r, font_xs, ref_str, 286 + ref_w / 2, 17, color_grey, 1);
    }

    // Time
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) {
        char ts[16];
        snprintf(ts, sizeof(ts), "%02d:%02d", lt->tm_hour, lt->tm_min);
        draw_text(r, font_md, ts, W - 78, 11, color_white, 0);
    }

    // Uptime pill centered in header (No IP displayed in header to preserve screenshot privacy)
    u64 uptime_ticks = armGetSystemTick() - start_tick;
    u64 uptime_sec = uptime_ticks / armGetSystemTickFreq();
    int days = (int)(uptime_sec / 86400);
    int hours = (int)((uptime_sec % 86400) / 3600);
    int mins = (int)((uptime_sec % 3600) / 60);
    char upt_str[32];
    if (days > 0)
        snprintf(upt_str, sizeof(upt_str), "Up: %dd %02dh", days, hours);
    else if (hours > 0)
        snprintf(upt_str, sizeof(upt_str), "Up: %02dh %02dm", hours, mins);
    else
        snprintf(upt_str, sizeof(upt_str), "Up: %02dm", mins);
    draw_rounded_box(r, 565, 13, 150, 24, 6, color_bg3);
    draw_rounded_rect(r, 565, 13, 150, 24, 6, color_card_border);
    draw_text(r, font_xs, upt_str, 640, 17, color_grey, 1);

    // Battery (cached every 5 seconds)
    u64 header_now = armGetSystemTick();
    if (header_now - header_batt_tick >= armGetSystemTickFreq() * 5) {
        psmGetBatteryChargePercentage(&header_cached_batt);
        psmGetChargerType(&header_cached_charger);
        header_batt_tick = header_now;
    }
    u32 batt = header_cached_batt;
    PsmChargerType ch = header_cached_charger;
    bool is_chg = (ch != PsmChargerType_Unconnected);
    draw_battery_icon(r, W - 150, 14, 44, 22, batt, is_chg);
    char batt_str[16];
    snprintf(batt_str, sizeof(batt_str), "%u%%", batt);
    SDL_Color batt_clr = is_chg ? color_cyan : (batt <= 15 ? color_red : (batt <= 30 ? color_yellow : color_white));
    draw_text(r, font_sm, batt_str, W - 200, 16, batt_clr, 2);
}

// ─── Tab bar ──────────────────────────────────────────────

static void draw_tabs(SDL_Renderer *r, int cur) {
    SDL_Rect bg = {0, 51, W, 62};
    SDL_SetRenderDrawColor(r, color_bg2.r, color_bg2.g, color_bg2.b, 255);
    SDL_RenderFillRect(r, &bg);
    thickLineRGBA(r, 0, 113, W, 113, 1, color_card_border.r, color_card_border.g, color_card_border.b, 255);

    // Left shoulder button indicator [L]
    bool hover_l = (ui_cursor_x >= 6 && ui_cursor_x <= 38 && ui_cursor_y >= 57 && ui_cursor_y <= 105);
    draw_rounded_box(r, 6, 60, 32, 42, 6, hover_l ? color_bg3 : color_card);
    draw_rounded_rect(r, 6, 60, 32, 42, 6, hover_l ? color_cyan : color_card_border);
    draw_text(r, font_xs, "L", 22, 73, hover_l ? color_white : color_cyan, 1);

    // Right shoulder button indicator [R]
    bool hover_r = (ui_cursor_x >= W - 38 && ui_cursor_x <= W - 6 && ui_cursor_y >= 57 && ui_cursor_y <= 105);
    draw_rounded_box(r, W - 38, 60, 32, 42, 6, hover_r ? color_bg3 : color_card);
    draw_rounded_rect(r, W - 38, 60, 32, 42, 6, hover_r ? color_cyan : color_card_border);
    draw_text(r, font_xs, "R", W - 22, 73, hover_r ? color_white : color_cyan, 1);

    int start_x = 46;
    int item_w = 132;

    for (int i = 0; i < PGS; i++) {
        int tx = start_x + i * item_w + 3;
        int tw = item_w - 6;
        int ty = 57;
        int th = 48;
        bool is_hover = (ui_cursor_x >= tx && ui_cursor_x <= tx + tw && ui_cursor_y >= ty && ui_cursor_y <= ty + th);

        if (i == cur) {
            // Selected Pill: rounded card + accent outline + glow line + accent dot
            draw_rounded_box(r, tx, ty, tw, th, 8, color_bg3);
            draw_rounded_rect(r, tx, ty, tw, th, 8, color_cyan);
            thickLineRGBA(r, tx + 12, ty + th - 4, tx + tw - 12, ty + th - 4, 3, color_cyan.r, color_cyan.g, color_cyan.b, 255);
            filledCircleRGBA(r, tx + 12, ty + 24, 3, color_cyan.r, color_cyan.g, color_cyan.b, 255);
            draw_text_w(r, font_sm, tab_names[cur_lang][i], tx + tw / 2 + 4, ty + 12, color_white, 1, tw - 16);
        } else {
            // Inactive Pill (with hover effect when stick pointer is over)
            draw_rounded_box(r, tx, ty, tw, th, 6, is_hover ? color_bg3 : color_card);
            draw_rounded_rect(r, tx, ty, tw, th, 6, is_hover ? color_cyan : color_card_border);
            draw_text_w(r, font_sm, tab_names[cur_lang][i], tx + tw / 2, ty + 12, is_hover ? color_white : color_grey, 1, tw - 8);
        }
    }
}

// ─── Footer ───────────────────────────────────────────────

static void draw_footer(SDL_Renderer *r, int cur_page) {
    SDL_Rect bg = {0, 660, W, 60};
    SDL_SetRenderDrawColor(r, color_bg2.r, color_bg2.g, color_bg2.b, 255);
    SDL_RenderFillRect(r, &bg);
    thickLineRGBA(r, 0, 660, W, 660, 1, color_card_border.r, color_card_border.g, color_card_border.b, 255);

    int fx = 20;
    int fy = 673;

    #define DRAW_KEY_BADGE(btn, label, col_btn) do { \
        int bw = 16 + (int)strlen(btn) * 8; \
        if (bw < 34) bw = 34; \
        draw_rounded_box(r, fx, fy, bw, 28, 6, color_bg3); \
        draw_rounded_rect(r, fx, fy, bw, 28, 6, color_card_border); \
        draw_text(r, font_xs, btn, fx + bw / 2, fy + 7, col_btn, 1); \
        fx += bw + 8; \
        draw_text(r, font_sm, label, fx, fy + 5, color_white, 0); \
        fx += (int)strlen(label) * 8 + 18; \
    } while (0)

    DRAW_KEY_BADGE("L / R", cur_lang == 1 ? "Onglets" : "Tabs", color_cyan);
    DRAW_KEY_BADGE("D-Pad", cur_lang == 1 ? "Naviguer" : "Scroll", color_cyan);
    DRAW_KEY_BADGE("Y", cur_lang == 1 ? "Actualiser" : "Refresh", color_yellow);

    if (cur_page == 0) {
        DRAW_KEY_BADGE("X", cur_lang == 1 ? "Reglages" : "Settings", color_green);
    } else if (cur_page == 1) {
        DRAW_KEY_BADGE("A", cur_lang == 1 ? "Explorateur" : "Browser", color_green);
    } else if (cur_page == 2) {
        DRAW_KEY_BADGE("Touch", cur_lang == 1 ? "Test Debit" : "Speed Test", color_green);
    } else if (cur_page == 3) {
        DRAW_KEY_BADGE("Touch", "FTP / MTP", color_green);
    } else if (cur_page == 4) {
        DRAW_KEY_BADGE("Touch", cur_lang == 1 ? "Exporter" : "Export", color_green);
    } else if (cur_page == 5) {
        DRAW_KEY_BADGE("-", cur_lang == 1 ? "Reset Drift" : "Reset Drift", color_green);
        DRAW_KEY_BADGE("X", cur_lang == 1 ? "Vibration" : "Rumble", color_orange);
    } else if (cur_page == 6) {
        DRAW_KEY_BADGE("A", cur_lang == 1 ? "Sauvegardes" : "Saves", color_green);
    } else if (cur_page == 8) {
        DRAW_KEY_BADGE("A", cur_lang == 1 ? "Theme Custom" : "Custom Theme", color_green);
    }

    #undef DRAW_KEY_BADGE

    // Exit badge on far right
    int exit_bw = 92;
    int exit_x = W - exit_bw - 24;
    draw_rounded_box(r, exit_x, fy, exit_bw, 28, 6, (SDL_Color){70, 20, 25, 255});
    draw_rounded_rect(r, exit_x, fy, exit_bw, 28, 6, color_red);
    draw_text(r, font_xs, s_exit_hint[cur_lang], exit_x + exit_bw / 2, fy + 7, color_white, 1);
}

// ─── Pages ────────────────────────────────────────────────

// Scroll state for pages
static int about_scroll = 0;
static int about_scroll_max = 0;
static int tools_scroll = 0;
static int tools_scroll_max = 0;
static int save_manager_loaded = 0;
static int save_manager_selected = 0;
static int save_backup_list_save = -1;
static int save_backup_selected = 0;
static AccountUid save_profile_uid = {{0}};
static char save_profile_name[64] = {0};
static bool save_profile_cached = false;
static u64 save_profile_retry_tick = 0;
static bool account_init_ok = false;
static u64 save_title_label_id = 0;
static char save_title_label[256] = {0};
static bool save_title_label_cached = false;

static bool read_account_nickname(AccountUid uid, char *out, size_t out_size, Result *error) {
    AccountProfile profile;
    Result rc = accountGetProfile(&profile, uid);
    if (R_FAILED(rc)) {
        if (error) *error = rc;
        return false;
    }
    AccountProfileBase base;
    memset(&base, 0, sizeof(base));
    rc = accountProfileGet(&profile, NULL, &base);
    bool ok = R_SUCCEEDED(rc);
    base.nickname[sizeof(base.nickname) - 1] = 0;
    ok = ok && base.nickname[0] && !memcmp(&base.uid, &uid, sizeof(uid));
    if (ok) snprintf(out, out_size, "%s", base.nickname);
    accountProfileClose(&profile);
    if (!ok && error)
        *error = R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    return ok;
}

static const char *get_save_title_label(u64 title_id) {
    if (save_title_label_cached && save_title_label_id == title_id) return save_title_label;

    save_title_label_id = title_id;
    save_title_label_cached = true;
    save_title_label[0] = 0;
    for (int i = 0; i < installed_game_count; ++i) {
        bool name_known = installed_game_names[i][0] != 0;
        for (int lang = 0; name_known && lang < LANG_MAX; ++lang)
            if (strcmp(installed_game_names[i], s_unknown_game[lang]) == 0) name_known = false;
        if (installed_game_ids[i] == title_id && name_known) {
            size_t name_len = strnlen(installed_game_names[i], sizeof(installed_game_names[i]));
            if (name_len >= sizeof(save_title_label)) name_len = sizeof(save_title_label) - 1;
            memcpy(save_title_label, installed_game_names[i], name_len);
            save_title_label[name_len] = 0;
            return save_title_label;
        }
    }

    if (!get_installed_title_name(title_id, save_title_label, sizeof(save_title_label))) {
        snprintf(save_title_label, sizeof(save_title_label), "Title ID: %016llX",
            (unsigned long long)title_id);
    }
    return save_title_label;
}

static const char *get_save_profile_name(AccountUid uid) {
    if (save_profile_cached && !memcmp(&save_profile_uid, &uid, sizeof(uid))) return save_profile_name;
    u64 now = armGetSystemTick();
    if (!memcmp(&save_profile_uid, &uid, sizeof(uid)) && save_profile_retry_tick &&
        now - save_profile_retry_tick < armGetSystemTickFreq() * 2)
        return save_profile_name;
    save_profile_uid = uid;
    save_profile_retry_tick = now;
    snprintf(save_profile_name, sizeof(save_profile_name), "%s", accountUidIsValid(&uid) ? s_profile_unavailable[cur_lang] : s_shared_save[cur_lang]);
    if (!accountUidIsValid(&uid)) {
        save_profile_cached = true;
        return save_profile_name;
    }
    Result profile_error = 0;
    if (!account_init_ok) {
        profile_error = accountInitialize(AccountServiceType_Application);
        account_init_ok = R_SUCCEEDED(profile_error);
    }
    if (!account_init_ok) {
        snprintf(save_profile_name, sizeof(save_profile_name), "%s %04llX (%08X)",
            s_profile_unavailable[cur_lang], (unsigned long long)(uid.uid[1] & 0xFFFF),
            (unsigned)profile_error);
        return save_profile_name;
    }
    bool loaded = read_account_nickname(uid, save_profile_name, sizeof(save_profile_name), &profile_error);
    if (!loaded) {
        AccountUid candidates[ACC_USER_LIST_SIZE];
        s32 candidate_count = 0;
        Result list_rc = accountListAllUsers(candidates, ACC_USER_LIST_SIZE, &candidate_count);
        if (R_SUCCEEDED(list_rc)) {
            if (candidate_count > ACC_USER_LIST_SIZE) candidate_count = ACC_USER_LIST_SIZE;
            for (s32 i = 0; i < candidate_count; ++i) {
                if (!memcmp(&candidates[i], &uid, sizeof(uid))) {
                    loaded = read_account_nickname(candidates[i], save_profile_name,
                        sizeof(save_profile_name), &profile_error);
                    break;
                }
            }
        } else if (R_SUCCEEDED(profile_error)) profile_error = list_rc;
    }
    if (loaded) save_profile_cached = true;
    else if (R_FAILED(profile_error))
        snprintf(save_profile_name, sizeof(save_profile_name), "%s %04llX (%08X)",
            s_profile_unavailable[cur_lang], (unsigned long long)(uid.uid[1] & 0xFFFF),
            (unsigned)profile_error);
    return save_profile_name;
}
static bool save_restore_confirm = false;
static u64 save_restore_confirm_tick = 0;
static int perf_scroll = 0;
static int perf_scroll_max = 0;
static int storage_scroll = 0;
static int storage_scroll_max = 0;
static int net_scroll = 0;
static int net_scroll_max = 0;
static int transfer_scroll = 0;
static int transfer_scroll_max = 0;
static int ctrl_scroll = 0;
static int ctrl_scroll_max = 0;
static int system_scroll = 0;
static int system_scroll_max = 0;
static int settings_scroll = 0;
static int settings_scroll_max = 0;

// System Info
static void draw_pg0(SDL_Renderer *r) {
    char t[256];

    int content_bottom = 990;
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_h = content_bottom - view_top;
    system_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (system_scroll > system_scroll_max) system_scroll = system_scroll_max;
    if (system_scroll < 0) system_scroll = 0;

    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = system_scroll;

    // ── Left Card: Firmware & Hardware (Height 475) ──
    draw_card(r, 40, 140 - sc, 580, 475, s_fw_hw[cur_lang]);
    int ly = 195 - sc;

    SetSysFirmwareVersion fw = {0};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        snprintf(t, sizeof(t), "%d.%d.%d", fw.major, fw.minor, fw.micro);
        draw_key_value(r, s_firmware[cur_lang], t, 70, ly, color_white);
    }
    ly += 27;

    SetSysSerialNumber sn = {0};
    if (R_SUCCEEDED(setsysGetSerialNumber(&sn))) {
        draw_key_value(r, s_serial[cur_lang], sn.number, 70, ly, color_white);
        ly += 27;
        draw_key_value(r, s_hardware[cur_lang], detect_hardware_type(sn.number), 70, ly, color_purple);
        ly += 27;
        draw_key_value(r, s_env_label[cur_lang], detect_emummc_str(), 70, ly, color_green);
        ly += 27;
    }

    int docked = appletGetOperationMode();
    draw_key_value(r, s_mode[cur_lang], docked ? s_docked[cur_lang] : s_handheld[cur_lang], 70, ly, docked ? color_green : color_cyan);
    ly += 27;

    draw_key_value(r, s_arch[cur_lang], s_arch_val[cur_lang], 70, ly, color_white);
    ly += 27;

    SetSysDeviceNickName nick = {0};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0]) {
        draw_key_value(r, s_dev_name[cur_lang], nick.nickname, 70, ly, color_yellow);
        ly += 27;
    }

    SetRegion region;
    if (R_SUCCEEDED(setGetRegionCode(&region))) {
        if ((int)region >= 0 && (int)region <= 5) {
            draw_key_value(r, s_region[cur_lang], s_country_names[cur_lang][(int)region], 70, ly, color_white);
            ly += 27;
        }
    }

    u64 lang = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&lang))) {
        SetLanguage langCode;
        if (R_SUCCEEDED(setMakeLanguage(lang, &langCode))) {
            if ((int)langCode >= 0 && (int)langCode < 18) {
                draw_key_value(r, s_sys_lang[cur_lang], s_lang_names_18[cur_lang][(int)langCode], 70, ly, color_white);
                ly += 27;
            }
        }
    }

    // Game cart info inside left card
    gamecart_update_info();
    const char *gamecard_status = !gamecart_status_available ? s_gamecard_status_na[cur_lang] :
        (gamecart_present ? s_gamecard_inserted[cur_lang] : s_no_gamecart[cur_lang]);
    draw_text_w(r, font_sm, gamecard_status, 70, ly, color_orange, 0, 520);

    // ── Right Card: Battery diagnostics and estimated battery-side power ──
    draw_card(r, 660, 140 - sc, 580, 475, s_batt_power[cur_lang]);
    int ry = 195 - sc;

    u32 batt = 0;
    bool batt_charging = false;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batt))) {
        PsmChargerType ch = PsmChargerType_Unconnected;
        psmGetChargerType(&ch);
        batt_charging = (ch != PsmChargerType_Unconnected);
        snprintf(t, sizeof(t), "%u%%", batt);
        draw_key_value(r, s_batt_level[cur_lang], t, 690, ry, get_usage_color(100 - batt));
        ry += 27;

        draw_key_value(r, s_charging_label[cur_lang], batt_charging ? s_charging[cur_lang] : s_discharging[cur_lang], 690, ry, batt_charging ? color_green : color_yellow);
        ry += 27;

        const char *chType = s_none[cur_lang];
        if (ch == PsmChargerType_EnoughPower) chType = s_ac_adapter[cur_lang];
        else if (ch == PsmChargerType_LowPower) chType = s_usb_slow[cur_lang];
        draw_key_value(r, s_charger[cur_lang], chType, 690, ry, color_white);
        ry += 27;

        draw_text(r, font_sm, s_capacity[cur_lang], 690, ry, color_grey, 0);
        draw_progress_bar(r, 875, ry + 2, 325, 16, batt / 100.f, get_usage_color(100 - batt), color_dark_grey);
        ry += 29;
    }

    // Battery rate, health and raw voltage
    batt_rate_update();
    update_battery_diagnostics();

    if (batt_age_pct > 0.0) {
        snprintf(t, sizeof(t), "%.1f%%", batt_age_pct);
        SDL_Color health_color = batt_age_pct >= 80.0 ? color_green :
            (batt_age_pct >= 65.0 ? color_yellow : color_red);
        draw_key_value(r, s_battery_condition[cur_lang], t, 690, ry, health_color);
        draw_progress_bar(r, 690, ry + 20, 510, 8, batt_age_pct / 100.0f,
            health_color, color_dark_grey);
        ry += 38;
    }

    if (batt_diag_valid) {
        snprintf(t, sizeof(t), "%u mV", batt_milli_volt);
        draw_key_value(r, "Voltage", t, 690, ry, color_white);
        ry += 27;
    } else {
        draw_key_value(r, "Voltage", s_battery_diag_na[cur_lang], 690, ry, color_grey);
        ry += 27;
    }

    if (batt_rate_initialized && batt_rate_ma > 0) {
        snprintf(t, sizeof(t), "%.0f mA", batt_rate_ma);
        draw_key_value(r, s_batt_rate_label[cur_lang], t, 690, ry, batt_charging ? color_green : color_yellow);
        ry += 27;
    }

    // Estimated battery-side power, derived from coarse charge percentage changes.
    float power_w = 0.0f;
    if (batt_diag_valid && batt_milli_volt > 0 && batt_rate_ma > 0) {
        power_w = (float)(batt_milli_volt * batt_rate_ma) / 1000000.0f;
        if (batt_charging) {
            snprintf(t, sizeof(t), "+%.2f W (Charge)", power_w);
            draw_key_value(r, s_battery_power_est[cur_lang], t, 690, ry, color_cyan);
        } else {
            snprintf(t, sizeof(t), "%.2f W (Discharge)", power_w);
            SDL_Color tdp_col = power_w < 6.0f ? color_green : (power_w < 9.5f ? color_yellow : color_red);
            draw_key_value(r, s_battery_power_est[cur_lang], t, 690, ry, tdp_col);
        }
        ry += 27;
    } else {
        draw_key_value(r, s_battery_power_est[cur_lang], s_battery_diag_na[cur_lang], 690, ry, color_grey);
        ry += 27;
    }

    // Joy-Con battery levels
    HidPowerInfo jc_left = {0}, jc_right = {0};
    hidGetNpadPowerInfoSplit(appletGetOperationMode() == 0 ? HidNpadIdType_Handheld : HidNpadIdType_No1, &jc_left, &jc_right);

    snprintf(t, sizeof(t), "%s", get_joycon_battery_str(jc_left.battery_level));
    draw_key_value(r, s_jc_l[cur_lang], t, 690, ry, get_joycon_battery_color(jc_left.battery_level));
    ry += 27;

    snprintf(t, sizeof(t), "%s", get_joycon_battery_str(jc_right.battery_level));
    draw_key_value(r, s_jc_r[cur_lang], t, 690, ry, get_joycon_battery_color(jc_right.battery_level));

    // ── Row 2: Thermals & Display (Height 175) ──
    soc_temp_update();
    draw_card(r, 40, 635 - sc, 1200, 175, s_thermals[cur_lang]);
    
    s32 skin = 0;
    if (R_SUCCEEDED(tcInitialize())) {
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin))) {
            draw_text_w(r, font_sm, s_skin_temp[cur_lang], 70, 696 - sc, color_grey, 0, 245);
            
            u32 tpct = 0;
            if (skin < 25000) tpct = 0;
            else if (skin > 70000) tpct = 100;
            else tpct = (u32)((skin - 25000) * 100 / 45000);
            
            draw_progress_bar(r, 340, 700 - sc, 230, 16, tpct / 100.f, get_temp_color(skin), color_dark_grey);
            draw_temperature(r, font_sm, skin, 340, 722 - sc, get_temp_color(skin), 0);

            if (soc_temp_valid) {
                snprintf(t, sizeof(t), "%d \xC2\xB0" "C", soc_temp_c);
                draw_key_value(r, s_soc_temp[cur_lang], t, 70, 752 - sc, get_temp_color(soc_temp_c * 1000));
            }

            const char *th_state = s_normal[cur_lang];
            SDL_Color th_col = color_green;
            if (skin >= 55000) { th_state = s_hot[cur_lang]; th_col = color_red; }
            else if (skin >= 40000) { th_state = s_warm[cur_lang]; th_col = color_yellow; }
            draw_key_value(r, s_thermal_state[cur_lang], th_state, 70, 782 - sc, th_col);
        }
        tcExit();
    }

    float br = 0;
    if (R_SUCCEEDED(brightness_read(&br))) {
        snprintf(t, sizeof(t), "%s: %.0f%%   ", s_brightness[cur_lang], br*100);
        draw_text(r, font_sm, t, 690, 690 - sc, color_yellow, 0);
        draw_progress_bar(r, 920, 692 - sc, 280, 16, br, color_yellow, color_dark_grey);
    } else if (lbl_emulator) {
        draw_key_value(r, s_brightness[cur_lang], s_na_emu[cur_lang], 690, 690 - sc, color_grey);
    }

    draw_key_value(r, s_resolution[cur_lang], s_res_val[cur_lang], 690, 725 - sc, color_cyan);
    snprintf(t, sizeof(t), "%.0f Hz", 60.0f);
    draw_key_value_wide(r, s_refresh_rate[cur_lang], t, 690, 755 - sc, 450, color_white);

    // ── Row 3: System Uptime (Height 115) ──
    draw_card(r, 40, 830 - sc, 1200, 115, s_sys_uptime[cur_lang]);
    u64 now_tick = armGetSystemTick();
    u64 elapsed = (now_tick - start_tick) / armGetSystemTickFreq();
    u32 days = (u32)(elapsed / 86400);
    u32 hrs = (u32)((elapsed % 86400) / 3600);
    u32 mins = (u32)((elapsed % 3600) / 60);
    u32 secs = (u32)(elapsed % 60);
    if (days > 0)
        snprintf(t, sizeof(t), "%u days, %02u:%02u:%02u", days, hrs, mins, secs);
    else
        snprintf(t, sizeof(t), "%02u:%02u:%02u", hrs, mins, secs);
    draw_key_value(r, s_app_uptime[cur_lang], t, 70, 885 - sc, color_cyan);

    // Modern vertical scrollbar
    draw_modern_scrollbar(r, view_top, view_bottom, system_scroll, system_scroll_max);
    SDL_RenderSetClipRect(r, NULL);
}

// File browser state (Storage page)
#define FB_MAX_ENTRIES 512
static int fb_active = 0;
static int fb_scroll = 0;
static int fb_count = 0;
static char fb_path[1024];
static char fb_entries[FB_MAX_ENTRIES][260];
static u64 fb_sizes[FB_MAX_ENTRIES];
static int fb_is_dir[FB_MAX_ENTRIES];
static int fb_selected = 0;
static int fb_sort_mode = 0;

static bool fb_sort_entries(void) {
    if (fb_count < 2) return true;
    int *idx = malloc(fb_count * sizeof(int));
    if (!idx) return false;
    for (int i = 0; i < fb_count; i++) idx[i] = i;
    for (int i = 0; i < fb_count - 1; i++) {
        for (int j = 0; j < fb_count - 1 - i; j++) {
            int a = idx[j], b = idx[j + 1];
            int swap = 0;
            if (fb_is_dir[a] != fb_is_dir[b])
                swap = !fb_is_dir[a];
            else if (fb_is_dir[a] || fb_sort_mode == 0)
                swap = strcasecmp(fb_entries[a], fb_entries[b]) > 0;
            else if (fb_sizes[a] != fb_sizes[b])
                swap = fb_sort_mode == 1 ? fb_sizes[a] < fb_sizes[b] : fb_sizes[a] > fb_sizes[b];
            else
                swap = strcasecmp(fb_entries[a], fb_entries[b]) > 0;
            if (swap) {
                int tmp = idx[j];
                idx[j] = idx[j + 1];
                idx[j + 1] = tmp;
            }
        }
    }

    char (*tmp_entries)[260] = malloc(fb_count * sizeof(*tmp_entries));
    u64 *tmp_sizes = malloc(fb_count * sizeof(*tmp_sizes));
    int *tmp_is_dir = malloc(fb_count * sizeof(*tmp_is_dir));
    bool sorted = tmp_entries && tmp_sizes && tmp_is_dir;
    if (sorted) {
        for (int i = 0; i < fb_count; i++) {
            memcpy(tmp_entries[i], fb_entries[idx[i]], sizeof(tmp_entries[i]));
            tmp_sizes[i] = fb_sizes[idx[i]];
            tmp_is_dir[i] = fb_is_dir[idx[i]];
        }
        memcpy(fb_entries, tmp_entries, sizeof(fb_entries[0]) * fb_count);
        memcpy(fb_sizes, tmp_sizes, sizeof(fb_sizes[0]) * fb_count);
        memcpy(fb_is_dir, tmp_is_dir, sizeof(fb_is_dir[0]) * fb_count);
    }
    free(tmp_entries);
    free(tmp_sizes);
    free(tmp_is_dir);
    free(idx);
    return sorted;
}

static void fb_open(const char *path) {
    strncpy(fb_path, path, sizeof(fb_path)-1);
    fb_count = 0;
    fb_scroll = 0;
    fb_selected = 0;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && fb_count < FB_MAX_ENTRIES) {
        if (strcmp(e->d_name, ".") == 0) continue;
        char fp[2048];
        snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
        struct stat st;
        int is_dir = 0;
        u64 sz = 0;
        if (stat(fp, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            if (!is_dir && st.st_size > 0) sz = (u64)st.st_size;
        }
        snprintf(fb_entries[fb_count], sizeof(fb_entries[0]), "%s%s", e->d_name, is_dir ? "/" : "");
        fb_sizes[fb_count] = sz;
        fb_is_dir[fb_count] = is_dir;
        fb_count++;
    }
    closedir(d);

    fb_sort_entries();

    fb_active = 1;
}

// Six-axis sensor handles for gyro/accel
static HidSixAxisSensorHandle sixaxis_handles[2];
static int sixaxis_handles_count = 0;
static bool sixaxis_init_ok = false;

// MAC address
static u8 mac_addr[6] = {0};
static bool mac_addr_valid = false;

// Manual fan control uses libnx's fan service. Keep a safe floor and restore
// Horizon's automatic controller before the app releases the service.
static FanController fan_controller;
static bool fan_service_open = false;
static bool fan_manual_active = false;
static bool fan_level_reported = false;
static int fan_user_pct = 45;
static int fan_requested_pct = 45;
static int fan_reported_pct = 0;
static u64 fan_safety_tick = 0;

static void fan_refresh_reported_level(void) {
    if (!fan_service_open) {
        fan_level_reported = false;
        return;
    }
    float level = 0.0f;
    if (R_SUCCEEDED(fanControllerGetRotationSpeedLevel(&fan_controller, &level))) {
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;
        fan_reported_pct = (int)(level * 100.0f + 0.5f);
        fan_level_reported = true;
    } else {
        fan_level_reported = false;
    }
}

static Result fan_service_start(void) {
    if (fan_service_open) return 0;
    Result rc = fanInitialize();
    if (R_FAILED(rc)) return rc;
    rc = fanOpenController(&fan_controller, 0x3D000001);
    if (R_FAILED(rc)) {
        fanExit();
        return rc;
    }
    fan_service_open = true;
    float level = 0.45f;
    if (R_SUCCEEDED(fanControllerGetRotationSpeedLevel(&fan_controller, &level))) {
        fan_requested_pct = (int)(level * 100.0f + 0.5f);
        if (fan_requested_pct < 30) fan_requested_pct = 30;
        if (fan_requested_pct > 100) fan_requested_pct = 100;
        fan_user_pct = fan_requested_pct;
        fan_reported_pct = fan_requested_pct;
        fan_level_reported = true;
    }
    return 0;
}

static Result fan_apply_safe_level(int requested_pct) {
    if (!fan_service_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (requested_pct < 30) requested_pct = 30;
    if (requested_pct > 100) requested_pct = 100;
    fan_user_pct = requested_pct;
    int safe_pct = requested_pct;
    s32 skin = 0;
    bool temp_ok = false;
    if (R_SUCCEEDED(tcInitialize())) {
        temp_ok = R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin));
        tcExit();
    }
    // Never reduce cooling as the console heats up; use a conservative level
    // when temperature telemetry is unavailable.
    if (!temp_ok || skin >= 65000) safe_pct = 100;
    else if (skin >= 55000 && safe_pct < 70) safe_pct = 70;
    Result rc = fanControllerSetRotationSpeedLevel(&fan_controller, safe_pct / 100.0f);
    if (R_SUCCEEDED(rc)) {
        fan_requested_pct = safe_pct;
        fan_manual_active = true;
        fan_refresh_reported_level();
    }
    return rc;
}

static void fan_service_restore(void) {
    if (!fan_service_open && !fan_manual_active) return;
    if (fan_service_open) {
        // Leave the fan at full cooling while handing control back.
        fanControllerSetRotationSpeedLevel(&fan_controller, 1.0f);
        fanControllerClose(&fan_controller);
        fanExit();
        fan_service_open = false;
    }
    if (R_SUCCEEDED(tcInitialize())) {
        tcEnableFanControl();
        tcExit();
    }
    fan_manual_active = false;
    fan_safety_tick = 0;
}

static void fan_safety_update(void) {
    if (!fan_manual_active || !fan_service_open) return;
    u64 now = armGetSystemTick();
    if (fan_safety_tick && now - fan_safety_tick < armGetSystemTickFreq() * 2) return;
    fan_safety_tick = now;
    if (R_FAILED(fan_apply_safe_level(fan_user_pct))) {
        add_alert("Fan control stopped; restoring automatic control.");
        fan_service_restore();
    }
}

// Overlay/sysmodule mode
static int app_mode = 0; // 0=normal, 1=overlay, 2=sysmodule

// Storage button Y positions (dynamic, updated on draw)
static int storage_test_btn_y = 0;
static int storage_fb_btn_y = 0;
static int storage_scan_button_y = 0;
static int storage_scan_category_y = 0;
static int storage_scan_details_y = 0;

#define STORAGE_SCAN_MAX_ENTRIES 512
enum {
    STORAGE_CATEGORY_GAMES,
    STORAGE_CATEGORY_HOMEBREW,
    STORAGE_CATEGORY_CFW,
    STORAGE_CATEGORY_MEDIA,
    STORAGE_CATEGORY_OTHER,
    STORAGE_CATEGORY_COUNT
};
enum {
    STORAGE_SCAN_IDLE,
    STORAGE_SCAN_RUNNING,
    STORAGE_SCAN_COMPLETE,
    STORAGE_SCAN_FAILED
};

typedef struct {
    char name[256];
    u64 size;
    int category;
    bool is_dir;
} StorageScanEntry;

typedef struct {
    u64 files;
    u64 folders;
    u64 errors;
} StorageScanProgress;

static Mutex storage_scan_mutex;
static bool storage_scan_mutex_ready = false;
static Thread storage_scan_thread;
static bool storage_scan_thread_started = false;
static int storage_scan_state = STORAGE_SCAN_IDLE;
static bool storage_scan_cancel = false;
static bool storage_scan_cache_ready = false;
static int storage_scan_detail_category = -1;
static bool storage_scan_detail_ascending = false;
static u64 storage_scan_category_bytes[STORAGE_CATEGORY_COUNT];
static u64 storage_scan_files = 0;
static u64 storage_scan_folders = 0;
static u64 storage_scan_errors = 0;
static StorageScanEntry storage_scan_entries[STORAGE_SCAN_MAX_ENTRIES];
static int storage_scan_entry_count = 0;
static int storage_scan_sorted_indices[STORAGE_CATEGORY_COUNT][STORAGE_SCAN_MAX_ENTRIES];
static int storage_scan_category_entries[STORAGE_CATEGORY_COUNT];
static bool storage_scan_truncated = false;

static int storage_scan_category_for_entry(const char *name, bool is_dir) {
    if (is_dir) {
        if (strcasecmp(name, "Nintendo") == 0) return STORAGE_CATEGORY_GAMES;
        if (strcasecmp(name, "switch") == 0) return STORAGE_CATEGORY_HOMEBREW;
        if (strcasecmp(name, "atmosphere") == 0 || strcasecmp(name, "bootloader") == 0 ||
            strcasecmp(name, "config") == 0 || strcasecmp(name, "emuMMC") == 0 ||
            strcasecmp(name, "sept") == 0 || strcasecmp(name, "warmboot_mariko") == 0)
            return STORAGE_CATEGORY_CFW;
        if (strcasecmp(name, "Album") == 0 || strcasecmp(name, "DCIM") == 0 ||
            strcasecmp(name, "Pictures") == 0 || strcasecmp(name, "Music") == 0 ||
            strcasecmp(name, "Movies") == 0 || strcasecmp(name, "Downloads") == 0 ||
            strcasecmp(name, "Documents") == 0)
            return STORAGE_CATEGORY_MEDIA;
        return STORAGE_CATEGORY_OTHER;
    }

    const char *ext = strrchr(name, '.');
    if (!ext) return STORAGE_CATEGORY_OTHER;
    ext++;
    if (strcasecmp(ext, "xci") == 0 || strcasecmp(ext, "nsp") == 0 ||
        strcasecmp(ext, "nsz") == 0 || strcasecmp(ext, "nca") == 0)
        return STORAGE_CATEGORY_GAMES;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0 || strcasecmp(ext, "bmp") == 0 ||
        strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
        strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
        strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "flac") == 0 ||
        strcasecmp(ext, "wav") == 0 || strcasecmp(ext, "txt") == 0 ||
        strcasecmp(ext, "pdf") == 0 || strcasecmp(ext, "md") == 0)
        return STORAGE_CATEGORY_MEDIA;
    return STORAGE_CATEGORY_OTHER;
}

static bool storage_scan_cancel_requested(void) {
    mutexLock(&storage_scan_mutex);
    bool cancel = storage_scan_cancel;
    mutexUnlock(&storage_scan_mutex);
    return cancel;
}

static u64 storage_scan_directory(const char *path, int depth, StorageScanProgress *progress) {
    if (depth > 32) {
        progress->errors++;
        return 0;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        progress->errors++;
        return 0;
    }

    progress->folders++;
    u64 total = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if ((progress->files & 31u) == 0 && storage_scan_cancel_requested()) break;

        char child[1024];
        int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(child)) {
            progress->errors++;
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            progress->errors++;
            continue;
        }
        u64 size = 0;
        if (S_ISDIR(st.st_mode)) {
            size = storage_scan_directory(child, depth + 1, progress);
        } else if (S_ISREG(st.st_mode)) {
            progress->files++;
            if (st.st_size > 0) size = (u64)st.st_size;
            if ((progress->files & 31u) == 0) {
                mutexLock(&storage_scan_mutex);
                storage_scan_files = progress->files;
                storage_scan_folders = progress->folders;
                mutexUnlock(&storage_scan_mutex);
            }
        }
        if (UINT64_MAX - total < size) {
            total = UINT64_MAX;
            progress->errors++;
        } else {
            total += size;
        }
    }
    closedir(dir);
    return total;
}

static void storage_scan_worker(void *arg) {
    (void)arg;
    StorageScanProgress progress = {0, 0, 0};
    DIR *root = opendir("sdmc:/");
    if (!root) {
        mutexLock(&storage_scan_mutex);
        storage_scan_state = STORAGE_SCAN_FAILED;
        storage_scan_errors = 1;
        mutexUnlock(&storage_scan_mutex);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(root)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (storage_scan_cancel_requested()) break;

        char path[1024];
        int length = snprintf(path, sizeof(path), "sdmc:/%s", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            progress.errors++;
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            progress.errors++;
            continue;
        }
        bool is_dir = S_ISDIR(st.st_mode);
        int category = storage_scan_category_for_entry(entry->d_name, is_dir);
        u64 size = 0;
        if (is_dir) {
            size = storage_scan_directory(path, 0, &progress);
        } else if (S_ISREG(st.st_mode)) {
            progress.files++;
            if (st.st_size > 0) size = (u64)st.st_size;
        }

        mutexLock(&storage_scan_mutex);
        if (UINT64_MAX - storage_scan_category_bytes[category] < size) {
            storage_scan_category_bytes[category] = UINT64_MAX;
            progress.errors++;
        } else {
            storage_scan_category_bytes[category] += size;
        }
        if (storage_scan_entry_count < STORAGE_SCAN_MAX_ENTRIES) {
            StorageScanEntry *item = &storage_scan_entries[storage_scan_entry_count++];
            snprintf(item->name, sizeof(item->name), "%s", entry->d_name);
            item->size = size;
            item->category = category;
            item->is_dir = is_dir;
        } else {
            storage_scan_truncated = true;
        }
        storage_scan_files = progress.files;
        storage_scan_folders = progress.folders;
        mutexUnlock(&storage_scan_mutex);
    }
    closedir(root);

    mutexLock(&storage_scan_mutex);
    storage_scan_files = progress.files;
    storage_scan_folders = progress.folders;
    storage_scan_errors = progress.errors;
    storage_scan_state = storage_scan_cancel ? STORAGE_SCAN_IDLE : STORAGE_SCAN_COMPLETE;
    mutexUnlock(&storage_scan_mutex);
}

static void storage_scan_prepare_cache(void) {
    mutexLock(&storage_scan_mutex);
    if (storage_scan_cache_ready || storage_scan_state != STORAGE_SCAN_COMPLETE) {
        mutexUnlock(&storage_scan_mutex);
        return;
    }
    memset(storage_scan_category_entries, 0, sizeof(storage_scan_category_entries));
    for (int i = 0; i < storage_scan_entry_count; i++) {
        int category = storage_scan_entries[i].category;
        int count = storage_scan_category_entries[category]++;
        storage_scan_sorted_indices[category][count] = i;
    }
    for (int category = 0; category < STORAGE_CATEGORY_COUNT; category++) {
        int count = storage_scan_category_entries[category];
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - 1 - i; j++) {
                int a = storage_scan_sorted_indices[category][j];
                int b = storage_scan_sorted_indices[category][j + 1];
                if (storage_scan_entries[a].size < storage_scan_entries[b].size) {
                    storage_scan_sorted_indices[category][j] = b;
                    storage_scan_sorted_indices[category][j + 1] = a;
                }
            }
        }
    }
    storage_scan_cache_ready = true;
    mutexUnlock(&storage_scan_mutex);
}

static void storage_scan_start(void) {
    mutexLock(&storage_scan_mutex);
    if (storage_scan_state == STORAGE_SCAN_RUNNING) {
        mutexUnlock(&storage_scan_mutex);
        return;
    }
    mutexUnlock(&storage_scan_mutex);

    if (storage_scan_thread_started) {
        threadWaitForExit(&storage_scan_thread);
        threadClose(&storage_scan_thread);
        storage_scan_thread_started = false;
    }
    mutexLock(&storage_scan_mutex);
    memset(storage_scan_category_bytes, 0, sizeof(storage_scan_category_bytes));
    storage_scan_files = storage_scan_folders = storage_scan_errors = 0;
    storage_scan_entry_count = 0;
    storage_scan_truncated = false;
    storage_scan_cache_ready = false;
    storage_scan_detail_category = -1;
    storage_scan_detail_ascending = false;
    storage_scan_cancel = false;
    storage_scan_state = STORAGE_SCAN_RUNNING;
    mutexUnlock(&storage_scan_mutex);

    Result rc = threadCreate(&storage_scan_thread, storage_scan_worker, NULL, NULL, 131072, 0x2B, -2);
    if (R_FAILED(rc)) {
        mutexLock(&storage_scan_mutex);
        storage_scan_state = STORAGE_SCAN_FAILED;
        storage_scan_errors = 1;
        mutexUnlock(&storage_scan_mutex);
        return;
    }
    rc = threadStart(&storage_scan_thread);
    if (R_FAILED(rc)) {
        threadClose(&storage_scan_thread);
        mutexLock(&storage_scan_mutex);
        storage_scan_state = STORAGE_SCAN_FAILED;
        storage_scan_errors = 1;
        mutexUnlock(&storage_scan_mutex);
        return;
    }
    storage_scan_thread_started = true;
}

static void storage_scan_cleanup(void) {
    if (!storage_scan_mutex_ready || !storage_scan_thread_started) return;
    mutexLock(&storage_scan_mutex);
    storage_scan_cancel = true;
    mutexUnlock(&storage_scan_mutex);
    threadWaitForExit(&storage_scan_thread);
    threadClose(&storage_scan_thread);
    storage_scan_thread_started = false;
}

// Popup for "En developpement" messages
static bool popup_active = false;
static char popup_text[128] = {0};
static u64 popup_start_tick = 0;
enum { POPUP_INFO = 0, POPUP_FAN = 1 };
static int popup_type = POPUP_INFO;
static int popup_fan_val = 30;

// Storage (cleaner reorganized layout)
// --- CFW & Atmosphere Crash Dumps Cleaner ---
static int cfw_cleaner_file_count = 0;
static u64 cfw_cleaner_total_bytes = 0;
static bool cfw_cleaner_scanned = false;
static int cfw_cleaner_last_deleted_count = 0;
static u64 cfw_cleaner_last_deleted_bytes = 0;
static bool cfw_cleaner_just_cleaned = false;

static void cleaner_scan_recursive(const char *dir_path, int *file_count, u64 *total_bytes) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                cleaner_scan_recursive(path, file_count, total_bytes);
            } else if (S_ISREG(st.st_mode)) {
                (*file_count)++;
                *total_bytes += (u64)st.st_size;
            }
        }
    }
    closedir(d);
}

static void cleaner_delete_recursive(const char *dir_path, int *deleted_count, u64 *deleted_bytes) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                cleaner_delete_recursive(path, deleted_count, deleted_bytes);
                rmdir(path);
            } else if (S_ISREG(st.st_mode)) {
                u64 sz = (u64)st.st_size;
                if (unlink(path) == 0) {
                    (*deleted_count)++;
                    *deleted_bytes += sz;
                }
            }
        }
    }
    closedir(d);
}

static void cfw_cleaner_scan(void) {
    cfw_cleaner_file_count = 0;
    cfw_cleaner_total_bytes = 0;
    cleaner_scan_recursive("sdmc:/atmosphere/crash_reports", &cfw_cleaner_file_count, &cfw_cleaner_total_bytes);
    cleaner_scan_recursive("sdmc:/atmosphere/fatal_reports", &cfw_cleaner_file_count, &cfw_cleaner_total_bytes);
    cleaner_scan_recursive("sdmc:/atmosphere/erpt_reports", &cfw_cleaner_file_count, &cfw_cleaner_total_bytes);
    cfw_cleaner_scanned = true;
    cfw_cleaner_just_cleaned = false;

    char msg[128];
    if (cfw_cleaner_file_count > 0) {
        char sz_str[32];
        format_bytes(cfw_cleaner_total_bytes, sz_str, sizeof(sz_str));
        snprintf(msg, sizeof(msg), cur_lang == 1 ? "Scan: %d rapports trouves (%s)" : "Scan: %d crash dumps found (%s)", cfw_cleaner_file_count, sz_str);
    } else {
        snprintf(msg, sizeof(msg), cur_lang == 1 ? "Scan termine: Aucun crash dump" : "Scan done: No crash reports found");
    }
    add_alert(msg);
}

static void cfw_cleaner_clean(void) {
    int del_count = 0;
    u64 del_bytes = 0;
    cleaner_delete_recursive("sdmc:/atmosphere/crash_reports", &del_count, &del_bytes);
    cleaner_delete_recursive("sdmc:/atmosphere/fatal_reports", &del_count, &del_bytes);
    cleaner_delete_recursive("sdmc:/atmosphere/erpt_reports", &del_count, &del_bytes);

    cfw_cleaner_last_deleted_count = del_count;
    cfw_cleaner_last_deleted_bytes = del_bytes;
    cfw_cleaner_file_count = 0;
    cfw_cleaner_total_bytes = 0;
    cfw_cleaner_scanned = true;
    cfw_cleaner_just_cleaned = true;

    char msg[128];
    if (del_count > 0) {
        char sz_str[32];
        format_bytes(del_bytes, sz_str, sizeof(sz_str));
        snprintf(msg, sizeof(msg), cur_lang == 1 ? "Nettoye: %d fichiers supprimes (%s liberes)!" : "Cleaned: %d dumps deleted (%s freed)!", del_count, sz_str);
    } else {
        snprintf(msg, sizeof(msg), cur_lang == 1 ? "Aucun rapport a supprimer" : "Nothing to clean");
    }
    add_alert(msg);
}

static void draw_pg1(SDL_Renderer *r) {
    char t[256];

    int view_top = 130, view_bottom = 655;
    int view_h = view_bottom - view_top;
    int content_bottom = 1140;
    int content_h = content_bottom - view_top;
    storage_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (storage_scroll > storage_scroll_max) storage_scroll = storage_scroll_max;
    if (storage_scroll < 0) storage_scroll = 0;
    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = storage_scroll;

    // ── Left Card: SD Card Info (Height 340) ──
    draw_card(r, 40, 135 - sc, 580, 340, s_sd_card[cur_lang]);
    int sy = 195 - sc;
    FsFileSystem sd;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&sd, "/", &f)) && R_SUCCEEDED(fsFsGetTotalSpace(&sd, "/", &tot)) && tot > 0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            SDL_Color clr = get_usage_color(pct);

            char s_tot[32], s_use[32], s_fre[32];
            format_bytes((u64)tot, s_tot, sizeof(s_tot));
            format_bytes((u64)used, s_use, sizeof(s_use));
            format_bytes((u64)f, s_fre, sizeof(s_fre));

            draw_key_value(r, s_total[cur_lang], s_tot, 70, sy, color_white);
            sy += 28;
            snprintf(t, sizeof(t), "%s (%u%%)", s_use, pct);
            draw_key_value(r, s_used[cur_lang], t, 70, sy, clr);
            sy += 28;
            draw_key_value(r, s_free[cur_lang], s_fre, 70, sy, color_green);
            sy += 28;

            draw_text(r, font_sm, s_storage_usage[cur_lang], 70, sy, color_grey, 0);
            draw_progress_bar(r, 300, sy + 2, 290, 16, (float)used / tot, clr, color_dark_grey);
            sy += 32;

            if (sd_speed_result > 0) {
                snprintf(t, sizeof(t), s_read_speed_fmt[cur_lang], sd_speed_result);
                draw_key_value(r, "Benchmark", t, 70, sy, color_green);
            } else if (sd_speed_result == -2) {
                draw_key_value(r, "Benchmark", s_read_testing[cur_lang], 70, sy, color_yellow);
            } else {
                draw_key_value(r, "Benchmark", s_read_untested[cur_lang], 70, sy, color_grey);
            }
            sy += 28;

            DIR *dir = opendir("sdmc:/");
            if (dir) {
                int file_count = 0, dir_count = 0;
                struct dirent *e;
                while ((e = readdir(dir)) != NULL) {
                    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                    char fp[300];
                    snprintf(fp, sizeof(fp), "sdmc:/%s", e->d_name);
                    struct stat st;
                    if (stat(fp, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) dir_count++;
                        else file_count++;
                    }
                }
                closedir(dir);
                snprintf(t, sizeof(t), s_root_fmt[cur_lang], dir_count, file_count);
                draw_text(r, font_sm, t, 70, sy, color_grey, 0);
            }
        } else {
            draw_text(r, font_sm, s_sd_fail[cur_lang], 70, sy, color_red, 0);
        }
        fsFsClose(&sd);
    } else if (sd_speed_result < 0) {
        draw_key_value(r, "Benchmark", s_read_error[cur_lang], 70, sy, color_red);
    } else {
        draw_text(r, font_sm, s_sd_no_mount[cur_lang], 70, sy, color_red, 0);
    }

    // ── Right Card: NAND Partitions (Height 340) ──
    draw_card(r, 660, 135 - sc, 580, 340, s_nand_parts[cur_lang]);
    int ny = 195 - sc;
    FsFileSystem ns;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&ns, FsBisPartitionId_System, ""))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&ns, "/", &f)) && R_SUCCEEDED(fsFsGetTotalSpace(&ns, "/", &tot)) && tot > 0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            SDL_Color clr = get_usage_color(pct);
            char stot[32], suse[32];
            format_bytes((u64)tot, stot, sizeof(stot));
            format_bytes((u64)used, suse, sizeof(suse));
            draw_text(r, font_sm, s_sys_part[cur_lang], 690, ny, color_cyan, 0);
            snprintf(t, sizeof(t), "%s / %s (%u%%)", suse, stot, pct);
            draw_text(r, font_xs, t, 950, ny + 3, color_white, 0);
            ny += 24;
            draw_progress_bar(r, 715, ny, 495, 12, (float)used / tot, clr, color_dark_grey);
            ny += 30;
        }
        fsFsClose(&ns);
    }
    FsFileSystem nu;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&nu, FsBisPartitionId_User, ""))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&nu, "/", &f)) && R_SUCCEEDED(fsFsGetTotalSpace(&nu, "/", &tot)) && tot > 0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            SDL_Color clr = get_usage_color(pct);
            char utot[32], uuse[32];
            format_bytes((u64)tot, utot, sizeof(utot));
            format_bytes((u64)used, uuse, sizeof(uuse));
            draw_text(r, font_sm, s_user_part[cur_lang], 690, ny, color_cyan, 0);
            snprintf(t, sizeof(t), "%s / %s (%u%%)", uuse, utot, pct);
            draw_text(r, font_xs, t, 950, ny + 3, color_white, 0);
            ny += 24;
            draw_progress_bar(r, 715, ny, 495, 12, (float)used / tot, clr, color_dark_grey);
            ny += 36;
        }
        fsFsClose(&nu);
    }

    // Two buttons side by side
    storage_fb_btn_y = ny;
    draw_rounded_box(r, 690, ny, 250, 42, 8, color_card_border);
    draw_rounded_rect(r, 690, ny, 250, 42, 8, color_cyan);
    draw_text_w(r, font_sm, s_open_fb[cur_lang], 815, ny + 10, color_cyan, 1, 235);

    storage_test_btn_y = ny;
    draw_rounded_box(r, 960, ny, 250, 42, 8, color_card_border);
    draw_rounded_rect(r, 960, ny, 250, 42, 8, color_grey);
    draw_text_w(r, font_sm, s_touch_test[cur_lang], 1085, ny + 10, color_white, 1, 235);

    // ── SD space analyzer ──
    draw_card(r, 40, 500 - sc, 1200, 380, s_sd_breakdown[cur_lang]);
    storage_scan_button_y = 510;
    storage_scan_category_y = 586;
    storage_scan_details_y = 722;
    draw_rounded_box(r, 1000, storage_scan_button_y - sc, 210, 34, 6, color_card_border);
    draw_rounded_rect(r, 1000, storage_scan_button_y - sc, 210, 34, 6, color_cyan);
    draw_text_w(r, font_xs, s_sd_scan_button[cur_lang], 1105, storage_scan_button_y + 8 - sc, color_white, 1, 196);

    storage_scan_prepare_cache();
    int scan_state, selected_category;
    u64 scan_files, scan_folders, scan_errors;
    u64 category_bytes[STORAGE_CATEGORY_COUNT];
    bool scan_truncated;
    StorageScanEntry detail_entries[5];
    int detail_count = 0, category_entry_count = 0;
    bool detail_ascending;
    mutexLock(&storage_scan_mutex);
    scan_state = storage_scan_state;
    scan_files = storage_scan_files;
    scan_folders = storage_scan_folders;
    scan_errors = storage_scan_errors;
    scan_truncated = storage_scan_truncated;
    memcpy(category_bytes, storage_scan_category_bytes, sizeof(category_bytes));
    selected_category = storage_scan_detail_category;
    detail_ascending = storage_scan_detail_ascending;
    if (scan_state == STORAGE_SCAN_COMPLETE && selected_category >= 0 &&
        selected_category < STORAGE_CATEGORY_COUNT && storage_scan_cache_ready) {
        category_entry_count = storage_scan_category_entries[selected_category];
        detail_count = category_entry_count < 5 ? category_entry_count : 5;
        for (int i = 0; i < detail_count; i++) {
            int order = detail_ascending ? category_entry_count - 1 - i : i;
            int idx = storage_scan_sorted_indices[selected_category][order];
            detail_entries[i] = storage_scan_entries[idx];
        }
    }
    mutexUnlock(&storage_scan_mutex);

    if (scan_state == STORAGE_SCAN_IDLE) {
        draw_text_w(r, font_xs, s_sd_scan_prompt[cur_lang], 70, 555 - sc, color_grey, 0, 900);
    } else if (scan_state == STORAGE_SCAN_RUNNING) {
        snprintf(t, sizeof(t), s_sd_scan_running[cur_lang],
            (unsigned long long)scan_files, (unsigned long long)scan_folders);
        draw_text_w(r, font_xs, t, 70, 555 - sc, color_yellow, 0, 900);
        int phase = (int)((armGetSystemTick() / (armGetSystemTickFreq() / 30 + 1)) % 8);
        draw_progress_bar(r, 70, 571 - sc, 1140, 6, (phase + 1) / 8.0f, color_cyan, color_dark_grey);
    } else if (scan_state == STORAGE_SCAN_FAILED) {
        draw_text_w(r, font_xs, s_sd_scan_failed[cur_lang], 70, 555 - sc, color_red, 0, 900);
    } else {
        char extra[64] = "";
        if (scan_errors)
            snprintf(extra, sizeof(extra), " (%llu skipped)", (unsigned long long)scan_errors);
        else if (scan_truncated)
            snprintf(extra, sizeof(extra), " (root list limited)");
        snprintf(t, sizeof(t), s_sd_scan_done[cur_lang],
            (unsigned long long)scan_files, (unsigned long long)scan_folders, extra);
        draw_text_w(r, font_xs, t, 70, 555 - sc, scan_errors ? color_yellow : color_green, 0, 900);

        u64 analyzed_bytes = 0;
        for (int i = 0; i < STORAGE_CATEGORY_COUNT; i++)
            analyzed_bytes += category_bytes[i];
        char bytes_text[32];
        format_bytes(analyzed_bytes, bytes_text, sizeof(bytes_text));
        snprintf(t, sizeof(t), "%s: %s", s_sd_scan_total[cur_lang], bytes_text);
        draw_text(r, font_xs, t, 70, 572 - sc, color_grey, 0);

        static const SDL_Color category_colors[STORAGE_CATEGORY_COUNT] = {
            {170, 120, 255, 255}, {0, 210, 255, 255}, {255, 190, 80, 255},
            {80, 220, 150, 255}, {150, 160, 175, 255}
        };
        for (int i = 0; i < STORAGE_CATEGORY_COUNT; i++) {
            int row_y = storage_scan_category_y + i * 27 - sc;
            u32 share = analyzed_bytes ? (u32)((double)category_bytes[i] * 100.0 / (double)analyzed_bytes) : 0;
            if (i == selected_category)
                draw_rounded_box(r, 60, row_y - 2, 1160, 24, 4, color_bg3);
            draw_text_w(r, font_xs, s_sd_scan_categories[cur_lang][i], 70, row_y, category_colors[i], 0, 250);
            draw_progress_bar(r, 335, row_y + 3, 630, 12,
                analyzed_bytes ? (float)((double)category_bytes[i] / (double)analyzed_bytes) : 0.0f,
                category_colors[i], color_dark_grey);
            format_bytes(category_bytes[i], bytes_text, sizeof(bytes_text));
            snprintf(t, sizeof(t), "%s  %u%%", bytes_text, share);
            draw_text(r, font_xs, t, 1200, row_y, color_white, 2);
        }

        draw_text_w(r, font_xs, s_sd_scan_details[cur_lang], 70, storage_scan_details_y - sc,
            color_grey, 0, 850);
        if (selected_category >= 0 && selected_category < STORAGE_CATEGORY_COUNT)
            draw_text_w(r, font_xs,
                s_size_sort_names[cur_lang][detail_ascending ? 2 : 1],
                1200, storage_scan_details_y - sc, color_cyan, 2, 300);
        for (int i = 0; i < detail_count; i++) {
            int row_y = storage_scan_details_y + 27 + i * 20 - sc;
            char item_label[280];
            snprintf(item_label, sizeof(item_label), "%.255s%s",
                detail_entries[i].name, detail_entries[i].is_dir ? "/" : "");
            draw_text_w(r, font_xs, item_label, 80, row_y,
                detail_entries[i].is_dir ? color_yellow : color_white, 0, 950);
            format_bytes(detail_entries[i].size, bytes_text, sizeof(bytes_text));
            draw_text(r, font_xs, bytes_text, 1200, row_y, color_cyan, 2);
        }
        if (category_entry_count > detail_count) {
            snprintf(t, sizeof(t), "+ %d more; open the file browser to browse all entries.",
                category_entry_count - detail_count);
            draw_text(r, font_xs, t, 80, storage_scan_details_y + 27 + detail_count * 20 - sc, color_grey, 0);
        }
    }

    // ── CFW & Atmosphere Crash Dumps Cleaner ──
    int cleaner_y = 900;
    draw_card(r, 40, cleaner_y - sc, 1200, 210,
        cur_lang == 1 ? "Nettoyeur de Rapports de Crash Atmosphere CFW" :
        "Atmosphere CFW Crash Dumps & Junk Cleaner");
    draw_text_w(r, font_sm,
        cur_lang == 1 ? "Atmosphere stocke les rapports d'erreurs et crash dumps dans /atmosphere/ sans jamais les effacer." :
        "Atmosphere creates error reports and fatal dumps on the SD card that continuously consume free space.",
        70, cleaner_y + 55 - sc, color_grey, 0, 1140);
    draw_text(r, font_xs,
        "Dossiers: /atmosphere/crash_reports | /atmosphere/fatal_reports | /atmosphere/erpt_reports",
        70, cleaner_y + 85 - sc, color_cyan, 0);

    if (cfw_cleaner_just_cleaned) {
        char clean_str[128], sz_str[32];
        format_bytes(cfw_cleaner_last_deleted_bytes, sz_str, sizeof(sz_str));
        snprintf(clean_str, sizeof(clean_str),
            cur_lang == 1 ? "Succes: %d fichiers supprimes, %s d'espace liberes sur la carte SD !" :
            "Success: %d crash dump files deleted, %s of SD space recovered!",
            cfw_cleaner_last_deleted_count, sz_str);
        draw_text_w(r, font_sm, clean_str, 70, cleaner_y + 115 - sc, color_green, 0, 1140);
    } else if (cfw_cleaner_scanned) {
        if (cfw_cleaner_file_count > 0) {
            char dump_str[128], sz_str[32];
            format_bytes(cfw_cleaner_total_bytes, sz_str, sizeof(sz_str));
            snprintf(dump_str, sizeof(dump_str),
                cur_lang == 1 ? "Analyse : %d rapports de crash trouves (%s a recuperer sur la SD)." :
                "Scan: Found %d crash dump files taking %s on your SD card.",
                cfw_cleaner_file_count, sz_str);
            draw_text_w(r, font_sm, dump_str, 70, cleaner_y + 115 - sc, color_yellow, 0, 1140);
        } else {
            draw_text_w(r, font_sm,
                cur_lang == 1 ? "Analyse : Carte SD propre ! Aucun rapport de crash trouve." :
                "Scan: Clean! No Atmosphere crash reports or dump files found.",
                70, cleaner_y + 115 - sc, color_green, 0, 1140);
        }
    } else {
        draw_text_w(r, font_sm,
            cur_lang == 1 ? "Statut : Pret. Touchez 'Analyser les Crashs' pour verifier l'espace occupe." :
            "Status: Ready. Touch 'Scan Crash Dumps' to check accumulated junk files.",
            70, cleaner_y + 115 - sc, color_white, 0, 1140);
    }

    // Action buttons
    int cleaner_btn_y = cleaner_y + 150 - sc;
    // Scan button
    draw_rounded_box(r, 680, cleaner_btn_y, 240, 42, 8, color_card_border);
    draw_rounded_rect(r, 680, cleaner_btn_y, 240, 42, 8, color_cyan);
    draw_text_w(r, font_sm,
        cur_lang == 1 ? "[Touch] Analyser" : "[Touch] Scan Dumps",
        800, cleaner_btn_y + 10, color_white, 1, 220);

    // Clean button
    bool can_clean = cfw_cleaner_file_count > 0;
    draw_rounded_box(r, 940, cleaner_btn_y, 260, 42, 8, can_clean ? color_red : color_dark_grey);
    draw_rounded_rect(r, 940, cleaner_btn_y, 260, 42, 8, can_clean ? color_white : color_card_border);
    draw_text_w(r, font_sm,
        cur_lang == 1 ? "[Touch] Tout Nettoyer" : "[Touch] Clean All Dumps",
        1070, cleaner_btn_y + 10, can_clean ? color_white : color_grey, 1, 240);

    SDL_RenderSetClipRect(r, NULL);
    draw_modern_scrollbar(r, view_top, view_bottom, storage_scroll, storage_scroll_max);
}

// Connection quality history
#define WIFI_HIST_SIZE 60
static int wifi_hist[WIFI_HIST_SIZE] = {0};
static int wifi_hist_pos = 0;
static int wifi_hist_count = 0;
static u64 wifi_hist_last_tick = 0;

typedef struct {
    u64 tick;
    u64 received;
    u64 sent;
} NetworkTrafficSample;

#define NETWORK_TRAFFIC_HISTORY 60
static NetworkTrafficSample network_traffic_history[NETWORK_TRAFFIC_HISTORY];
static int network_traffic_pos = 0;
static int network_traffic_count = 0;
static u64 network_traffic_rx_rate = 0;
static u64 network_traffic_tx_rate = 0;
static u64 network_traffic_rx_total = 0;
static u64 network_traffic_tx_total = 0;
static u64 network_traffic_rx_session = 0;
static u64 network_traffic_tx_session = 0;
static u64 network_traffic_last_tick = 0;
static u64 network_traffic_last_attempt_tick = 0;
static NetworkCounterErrors network_traffic_errors = {0, 0, 0};
static bool network_traffic_has_baseline = false;
static bool network_traffic_available = false;

static void network_sample_traffic(void) {
    u64 now = armGetSystemTick();
    u64 frequency = armGetSystemTickFreq();
    if (network_traffic_last_attempt_tick &&
        now - network_traffic_last_attempt_tick < frequency)
        return;
    network_traffic_last_attempt_tick = now;

    NetworkCounters counters = {0, 0};
    if (!network_counters_read(&counters, &network_traffic_errors)) {
        network_traffic_available = false;
        network_traffic_rx_rate = 0;
        network_traffic_tx_rate = 0;
        network_traffic_has_baseline = false;
        return;
    }

    if (network_traffic_has_baseline && now > network_traffic_last_tick) {
        double elapsed = (double)(now - network_traffic_last_tick) /
            (double)armGetSystemTickFreq();
        u64 received_delta = counters.received >= network_traffic_rx_total ?
            counters.received - network_traffic_rx_total : counters.received;
        u64 sent_delta = counters.sent >= network_traffic_tx_total ?
            counters.sent - network_traffic_tx_total : counters.sent;
        network_traffic_rx_rate = (u64)((double)received_delta / elapsed);
        network_traffic_tx_rate = (u64)((double)sent_delta / elapsed);
        network_traffic_rx_session += received_delta;
        network_traffic_tx_session += sent_delta;
    } else {
        network_traffic_rx_rate = 0;
        network_traffic_tx_rate = 0;
    }

    network_traffic_rx_total = counters.received;
    network_traffic_tx_total = counters.sent;
    network_traffic_last_tick = now;
    network_traffic_has_baseline = true;
    network_traffic_available = true;
    network_traffic_errors = (NetworkCounterErrors){0, 0, 0};
    network_traffic_history[network_traffic_pos].tick = now;
    network_traffic_history[network_traffic_pos].received = network_traffic_rx_rate;
    network_traffic_history[network_traffic_pos].sent = network_traffic_tx_rate;
    network_traffic_pos = (network_traffic_pos + 1) % NETWORK_TRAFFIC_HISTORY;
    if (network_traffic_count < NETWORK_TRAFFIC_HISTORY) network_traffic_count++;
}

// Network
static void draw_pg2(SDL_Renderer *r) {
    char t[128];

    int content_bottom = 1390;
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_h = content_bottom - view_top;
    net_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (net_scroll > net_scroll_max) net_scroll = net_scroll_max;
    if (net_scroll < 0) net_scroll = 0;
    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = net_scroll;

    draw_card(r, 40, 140 - sc, 580, 380, s_ip_config[cur_lang]);
    u32 ip=0, msk=0, gw=0, d1=0, d2=0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip,&msk,&gw,&d1,&d2)) && ip) {
        snprintf(t,sizeof(t),"%u.%u.%u.%u",ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
        draw_key_value(r, s_ip_addr[cur_lang], t, 70, 195 - sc, color_green);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",msk&0xFF,(msk>>8)&0xFF,(msk>>16)&0xFF,(msk>>24)&0xFF);
        draw_key_value(r, s_subnet[cur_lang], t, 70, 223 - sc, color_white);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",gw&0xFF,(gw>>8)&0xFF,(gw>>16)&0xFF,(gw>>24)&0xFF);
        draw_key_value(r, s_gateway[cur_lang], t, 70, 251 - sc, color_white);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",d1&0xFF,(d1>>8)&0xFF,(d1>>16)&0xFF,(d1>>24)&0xFF);
        draw_key_value(r, s_primary_dns[cur_lang], t, 70, 279 - sc, color_white);
        if (d2) {
            snprintf(t,sizeof(t),"%u.%u.%u.%u",d2&0xFF,(d2>>8)&0xFF,(d2>>16)&0xFF,(d2>>24)&0xFF);
            draw_key_value(r, s_secondary_dns[cur_lang], t, 70, 307 - sc, color_white);
        } else {
            draw_key_value(r, s_secondary_dns[cur_lang], s_none[cur_lang], 70, 307 - sc, color_grey);
        }

        char hostname[64] = {0};
        if (gethostname(hostname, sizeof(hostname)) == 0 && hostname[0])
            draw_key_value(r, s_hostname[cur_lang], hostname, 70, 335 - sc, color_white);
        else
            draw_key_value(r, s_hostname[cur_lang], "NintendoSwitch", 70, 335 - sc, color_grey);

        if (mac_addr_valid) {
            snprintf(t, sizeof(t), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
            draw_key_value(r, s_mac_addr[cur_lang], t, 70, 363 - sc, color_yellow);
        }

        draw_key_value(r, s_adapter[cur_lang], "Broadcom BCM4356", 70, 391 - sc, color_white);
        draw_text(r, font_sm, "Dual-band 802.11ac Wi-Fi + BT 4.1", 70, 419 - sc, color_grey, 0);

        if (ftp_on) {
            snprintf(t, sizeof(t), "%s://%u.%u.%u.%u:%d", ftp_mode ? "ftpd" : "ftp", ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF, ftp_get_port());
            draw_key_value(r, "FTP Access", t, 70, 447 - sc, color_yellow);
        }
    } else {
        draw_text(r, font_sm, s_not_connected[cur_lang], 70, 210 - sc, color_red, 0);
        draw_text(r, font_sm, s_go_to_settings[cur_lang], 70, 240 - sc, color_grey, 0);
        draw_text(r, font_sm, s_supports_both[cur_lang], 70, 270 - sc, color_grey, 0);
    }

    draw_card(r, 660, 140 - sc, 580, 260, s_conn_details[cur_lang]);
    NifmInternetConnectionType ct; u32 ws=0; NifmInternetConnectionStatus cs;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&ct,&ws,&cs))) {
        const char *netType = "Unknown";
        if (ct == 1) netType = s_wifi_wireless[cur_lang];
        else if (ct == 2) netType = s_ethernet_wired[cur_lang];
        draw_key_value(r, s_interface[cur_lang], netType, 690, 210 - sc, ct == 2 ? color_green : color_cyan);
        
        draw_key_value(r, s_internet[cur_lang], cs == 4 ? s_connected[cur_lang] : s_limited[cur_lang], 690, 240 - sc, cs == 4 ? color_green : color_yellow);

        if (ct == 1 && ws > 0) {
            draw_text(r, font_sm, s_signal_quality[cur_lang], 690, 290 - sc, color_grey, 0);
            draw_wifi_bars(r, 870, 290 - sc, 50, 20, ws);
            snprintf(t, sizeof(t), "%d/3", ws > 3 ? 3 : ws);
            draw_text(r, font_sm, t, 940, 290 - sc, color_cyan, 0);
        } else if (ct == 2) {
            draw_key_value(r, s_link[cur_lang], s_wired_stable[cur_lang], 690, 290 - sc, color_green);
            if (gw) {
                char gw_str[32];
                snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", gw&0xFF, (gw>>8)&0xFF, (gw>>16)&0xFF, (gw>>24)&0xFF);
                draw_key_value(r, s_gateway[cur_lang], gw_str, 690, 350 - sc, color_white);
            }
        }
        if (net_connected_tick) {
            u64 elapsed = (armGetSystemTick() - net_connected_tick) / armGetSystemTickFreq();
            u32 hrs = (u32)(elapsed / 3600);
            u32 mins = (u32)((elapsed % 3600) / 60);
            u32 secs = (u32)(elapsed % 60);
            snprintf(t, sizeof(t), "%02uh %02um %02us", hrs, mins, secs);
            draw_key_value(r, s_connected_for[cur_lang], t, 690, 320 - sc, color_white);
        }
        if (ct == 1) {
            draw_key_value(r, s_band[cur_lang], ws >= 2 ? "5 GHz" : "2.4 GHz", 690, 340 - sc, ws >= 2 ? color_green : color_yellow);
        }
        if (cs == 4)
            draw_key_value(r, s_status[cur_lang], s_full_internet[cur_lang], 690, 365 - sc, color_green);
        else if (cs == 3)
            draw_key_value(r, s_status[cur_lang], s_local_only[cur_lang], 690, 365 - sc, color_yellow);
        else
            draw_key_value(r, s_status[cur_lang], s_no_conn[cur_lang], 690, 365 - sc, color_red);
    }

    // Network Speed Test card (below IP config, left column)
    draw_card(r, 40, 540 - sc, 580, 180, s_net_test_label[cur_lang]);
    draw_rounded_box(r, 365, 600 - sc, 220, 36, 6, net_test_running ? color_dark_grey : color_card_border);
    draw_rounded_rect(r, 365, 600 - sc, 220, 36, 6, color_cyan);
    draw_text_w(r, font_sm, net_test_running ? s_net_testing[cur_lang] : s_net_test_btn[cur_lang], 475, 608 - sc, color_white, 1, 204);
    if (net_test_running)
        draw_text_w(r, font_sm, s_net_testing[cur_lang], 70, 602 - sc, color_yellow, 0, 270);
    else {
        int ny = 594 - sc;
        if (net_ping_ms > 0)
            snprintf(t, sizeof(t), s_tcp_connect_fmt[cur_lang], net_ping_ms, net_jitter_ms);
        else
            snprintf(t, sizeof(t), "%s: %s",
                cur_lang == 1 ? "Connexion TCP" : "TCP connect", s_error[cur_lang]);
        draw_text(r, font_xs, t, 70, ny, net_ping_ms > 0 ? color_green : color_red, 0);
        ny += 19;
        if (net_dl_mbps > 0) snprintf(t, sizeof(t), s_net_dl[cur_lang], net_dl_mbps);
        else if (net_dl_http_status >= 400) snprintf(t, sizeof(t), "Download: HTTP %d", net_dl_http_status);
        else if (net_dl_http_status) snprintf(t, sizeof(t), "Download: no data (HTTP %d)", net_dl_http_status);
        else snprintf(t, sizeof(t), "Download: %s", s_error[cur_lang]);
        draw_text_w(r, font_xs, t, 70, ny, net_dl_mbps > 0 ? color_green : color_red, 0, 270);
        ny += 19;
        if (net_ul_mbps > 0) snprintf(t, sizeof(t), s_net_ul[cur_lang], net_ul_mbps);
        else if (net_ul_http_status >= 400) snprintf(t, sizeof(t), "Upload: HTTP %d", net_ul_http_status);
        else if (net_ul_http_status) snprintf(t, sizeof(t), "Upload: failed (HTTP %d)", net_ul_http_status);
        else snprintf(t, sizeof(t), "Upload: %s", s_error[cur_lang]);
        draw_text_w(r, font_xs, t, 70, ny, net_ul_mbps > 0 ? color_green : color_red, 0, 270);
    }
    draw_text_w(r, font_xs, s_net_ping_history[cur_lang], 70, 652 - sc, color_grey, 0, 500);
    int ping_chart_x = 110, ping_chart_y = 670 - sc, ping_chart_w = 470, ping_chart_h = 24;
    draw_rounded_rect(r, ping_chart_x, ping_chart_y, ping_chart_w, ping_chart_h, 3, color_dark_grey);
    lineRGBA(r, ping_chart_x, ping_chart_y + ping_chart_h / 2,
        ping_chart_x + ping_chart_w - 1, ping_chart_y + ping_chart_h / 2, 100, 100, 100, 80);
    if (net_ping_history_count > 0) {
        int first = (net_ping_history_pos - net_ping_history_count + NET_PING_HIST_SIZE) % NET_PING_HIST_SIZE;
        int previous_x = 0, previous_y = 0;
        for (int i = 0; i < net_ping_history_count; i++) {
            int idx = (first + i) % NET_PING_HIST_SIZE;
            float ping = net_ping_history[idx];
            if (ping > 200.0f) ping = 200.0f;
            int px = ping_chart_x + ping_chart_w - 1 -
                (net_ping_history_count - 1 - i) * (ping_chart_w - 1) / (NET_PING_HIST_SIZE - 1);
            int py = ping_chart_y + ping_chart_h - 1 -
                (int)(ping * (ping_chart_h - 1) / 200.0f);
            SDL_Color ping_color = ping < 50.0f ? color_green :
                (ping < 120.0f ? color_yellow : color_red);
            if (i > 0)
                thickLineRGBA(r, previous_x, previous_y, px, py, 2,
                    ping_color.r, ping_color.g, ping_color.b, 220);
            filledCircleRGBA(r, px, py, i == net_ping_history_count - 1 ? 3 : 2,
                ping_color.r, ping_color.g, ping_color.b, 240);
            previous_x = px;
            previous_y = py;
        }
    } else {
        draw_text(r, font_xs, s_net_testing[cur_lang], ping_chart_x + ping_chart_w / 2,
            ping_chart_y + 4, color_grey, 1);
    }
    draw_text(r, font_xs, "0 ms", 70, ping_chart_y + 4, color_grey, 0);
    draw_text(r, font_xs, "200", 595, ping_chart_y + 4, color_grey, 2);
    draw_text_w(r, font_xs, "Speed test uses public test servers",
        70, 700 - sc, color_grey, 0, 500);

    draw_card(r, 660, 420 - sc, 580, 280, s_wifi_diag[cur_lang]);
    if (R_SUCCEEDED(wlaninfInitialize())) {
        WlanInfState wst;
        if (R_SUCCEEDED(wlaninfGetState(&wst)) && wst == WlanInfState_Connected) {
            draw_key_value(r, s_ssid[cur_lang], s_connected_label[cur_lang], 690, 500 - sc, color_yellow);
            s32 rssi = 0;
            if (R_SUCCEEDED(wlaninfGetRSSI(&rssi))) {
                int qual = (rssi + 90) * 100 / 60;
                if (qual > 100) qual = 100;
                if (qual < 0) qual = 0;

                snprintf(t, sizeof(t), "%d dBm", rssi);
                draw_key_value(r, s_rssi[cur_lang], t, 690, 530 - sc, color_white);
                
                snprintf(t, sizeof(t), "%d%%", qual);
                draw_key_value(r, s_link_quality[cur_lang], t, 690, 560 - sc, get_usage_color(100 - qual));
                
                draw_text(r, font_sm, s_signal_power[cur_lang], 690, 595 - sc, color_grey, 0);
                draw_progress_bar(r, 870, 597 - sc, 330, 16, qual / 100.f, get_usage_color(100 - qual), color_dark_grey);

                u64 sample_tick = armGetSystemTick();
                if (!wifi_hist_last_tick ||
                    sample_tick - wifi_hist_last_tick >= armGetSystemTickFreq()) {
                    wifi_hist_last_tick = sample_tick;
                    wifi_hist[wifi_hist_pos] = qual;
                    wifi_hist_pos = (wifi_hist_pos + 1) % WIFI_HIST_SIZE;
                    if (wifi_hist_count < WIFI_HIST_SIZE) wifi_hist_count++;
                }
                draw_text(r, font_xs, s_signal_history[cur_lang], 690, 625 - sc, color_grey, 0);
                int chart_x = 870, chart_y = 622 - sc, chart_w = 330, chart_h = 38;
                draw_rounded_rect(r, chart_x, chart_y, chart_w, chart_h, 3, color_dark_grey);
                lineRGBA(r, chart_x, chart_y + chart_h / 2, chart_x + chart_w - 1,
                    chart_y + chart_h / 2, 110, 110, 110, 85);
                int hc = wifi_hist_count;
                if (hc > WIFI_HIST_SIZE) hc = WIFI_HIST_SIZE;
                for (int si = 0; si < hc - 1; si++) {
                    int x1 = chart_x + chart_w - (si+1) * chart_w / hc;
                    int x2 = chart_x + chart_w - (si+2) * chart_w / hc;
                    int y1 = chart_y + chart_h - wifi_hist[(wifi_hist_pos - si - 1 + WIFI_HIST_SIZE) % WIFI_HIST_SIZE] * (chart_h - 1) / 100;
                    int y2 = chart_y + chart_h - wifi_hist[(wifi_hist_pos - si - 2 + WIFI_HIST_SIZE) % WIFI_HIST_SIZE] * (chart_h - 1) / 100;
                    lineRGBA(r, x1, y1, x2, y2, color_cyan.r, color_cyan.g, color_cyan.b, 200);
                }
                draw_text(r, font_xs, "60s", chart_x, chart_y + chart_h + 2, color_grey, 0);
                draw_text(r, font_xs, "now", chart_x + chart_w, chart_y + chart_h + 2, color_grey, 2);
            }
        } else {
            draw_text(r, font_sm, s_wifi_not_active[cur_lang], 690, 540 - sc, color_grey, 0);
        }
        wlaninfExit();
    } else {
        draw_text(r, font_sm, s_wlan_na[cur_lang], 690, 540 - sc, color_grey, 0);
    }

    // USB Ethernet diagnostics
    draw_card(r, 40, 750 - sc, 1200, 140, "USB Ethernet Adapter");
    NifmInternetConnectionType eth_ct; u32 eth_ws=0; NifmInternetConnectionStatus eth_cs;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&eth_ct,&eth_ws,&eth_cs)) && eth_ct == 2 && eth_cs == 4) {
        draw_text(r, font_sm, "USB Ethernet adapter connected", 70, 805 - sc, color_green, 0);
        draw_text(r, font_sm, "Stable wired connection - ideal for online gaming & file transfer", 70, 835 - sc, color_grey, 0);
        // MAC address via ifreq
        int eth_s = socket(AF_INET, SOCK_DGRAM, 0);
        if (eth_s >= 0) {
            struct ifreq eth_ifr;
            strncpy(eth_ifr.ifr_name, "eth0", sizeof(eth_ifr.ifr_name)-1);
            if (ioctl(eth_s, SIOCGIFINDEX, &eth_ifr) == 0) {
                // Try to read MAC from sysfs or use placeholder
                draw_text(r, font_sm, "USB Ethernet active", 70, 862 - sc, color_cyan, 0);
            }
            close(eth_s);
        }
    } else {
        draw_text(r, font_sm, "No USB Ethernet adapter detected", 70, 805 - sc, color_grey, 0);
        draw_text(r, font_sm, "Connect a compatible USB Ethernet adapter via the dock", 70, 835 - sc, color_grey, 0);
    }

    draw_card(r, 40, 920 - sc, 1200, 190, s_traffic_title[cur_lang]);
    if (network_traffic_available) {
        char rx_text[48], tx_text[48], rx_total_text[48], tx_total_text[48];
        format_bytes(network_traffic_rx_rate, rx_text, sizeof(rx_text));
        format_bytes(network_traffic_tx_rate, tx_text, sizeof(tx_text));
        format_bytes(network_traffic_rx_session, rx_total_text, sizeof(rx_total_text));
        format_bytes(network_traffic_tx_session, tx_total_text, sizeof(tx_total_text));
        char rate_text[64];
        snprintf(rate_text, sizeof(rate_text), "%s/s", rx_text);
        draw_key_value(r, s_traffic_rx[cur_lang], rate_text, 70, 970 - sc, color_cyan);
        snprintf(rate_text, sizeof(rate_text), "%s/s", tx_text);
        draw_key_value(r, s_traffic_tx[cur_lang], rate_text, 70, 1000 - sc, color_orange);
        draw_key_value(r, s_traffic_received_total[cur_lang], rx_total_text,
            70, 1030 - sc, color_cyan);
        draw_key_value(r, s_traffic_sent_total[cur_lang], tx_total_text,
            70, 1060 - sc, color_orange);

        int graph_x = 360, graph_y = 970 - sc, graph_w = 830, graph_h = 78;
        draw_rounded_rect(r, graph_x, graph_y, graph_w, graph_h, 5, color_dark_grey);
        lineRGBA(r, graph_x, graph_y + graph_h - 1, graph_x + graph_w - 1,
            graph_y + graph_h - 1, 120, 120, 120, 90);
        u64 max_rate = 0;
        for (int i = 0; i < network_traffic_count; i++) {
            int idx = (network_traffic_pos - network_traffic_count + i +
                NETWORK_TRAFFIC_HISTORY) % NETWORK_TRAFFIC_HISTORY;
            if (network_traffic_history[idx].received > max_rate)
                max_rate = network_traffic_history[idx].received;
            if (network_traffic_history[idx].sent > max_rate)
                max_rate = network_traffic_history[idx].sent;
        }
        if (max_rate == 0) max_rate = 1;
        int prev_rx_x = 0, prev_rx_y = 0, prev_tx_x = 0, prev_tx_y = 0;
        for (int i = 0; i < network_traffic_count; i++) {
            int idx = (network_traffic_pos - network_traffic_count + i +
                NETWORK_TRAFFIC_HISTORY) % NETWORK_TRAFFIC_HISTORY;
            int px = graph_x + (network_traffic_count > 1 ?
                i * (graph_w - 1) / (network_traffic_count - 1) : graph_w - 1);
            int rx_y = graph_y + graph_h - 2 -
                (int)(network_traffic_history[idx].received * (graph_h - 4) / max_rate);
            int tx_y = graph_y + graph_h - 2 -
                (int)(network_traffic_history[idx].sent * (graph_h - 4) / max_rate);
            if (i > 0) {
                thickLineRGBA(r, prev_rx_x, prev_rx_y, px, rx_y, 2,
                    color_cyan.r, color_cyan.g, color_cyan.b, 230);
                thickLineRGBA(r, prev_tx_x, prev_tx_y, px, tx_y, 2,
                    color_orange.r, color_orange.g, color_orange.b, 230);
            }
            prev_rx_x = px;
            prev_rx_y = rx_y;
            prev_tx_x = px;
            prev_tx_y = tx_y;
        }
        draw_text(r, font_xs, "60s", graph_x, graph_y + graph_h + 2, color_grey, 0);
        draw_text(r, font_xs, "now", graph_x + graph_w, graph_y + graph_h + 2,
            color_grey, 2);
    } else {
        char error_text[160];
        snprintf(error_text, sizeof(error_text),
            "%s (route %d, routeL %d, ioctl %d)",
            s_traffic_unavailable[cur_lang],
            network_traffic_errors.route_list_error,
            network_traffic_errors.route_list_long_error,
            network_traffic_errors.ioctl_error);
        draw_text_w(r, font_sm, error_text, 70, 980 - sc, color_grey, 0, 1100);
    }

    // ── CFW Anti-Ban & Telemetry Shield (DNS.mitm / 90DNS) ──
    int antiban_y = 1140;
    draw_card(r, 40, antiban_y - sc, 1200, 220,
        cur_lang == 1 ? "Bouclier Anti-Ban & Telemetrie Nintendo (90DNS / DNS.mitm)" :
        "CFW Anti-Ban & Nintendo Telemetry Shield");
    draw_text_w(r, font_sm,
        cur_lang == 1 ? "Verifie si les serveurs de telemetrie de Nintendo sont neutralises par DNS.mitm ou 90DNS." :
        "Verifies that Nintendo telemetry & auth endpoints are properly blocked to prevent console bans.",
        70, antiban_y + 50 - sc, color_grey, 0, 1140);

    // Left Column: 3 Targets
    int target_y = antiban_y + 82 - sc;
    for (int ti = 0; ti < 3; ti++) {
        SDL_Color stat_clr = color_grey;
        const char *stat_lbl = "Not tested";
        if (antiban_targets[ti].state == ANTIBAN_SAFE_BLOCKED) {
            stat_clr = color_green;
            stat_lbl = cur_lang == 1 ? "[BLOQUE / SECURISE]" : "[BLOCKED / SAFE]";
        } else if (antiban_targets[ti].state == ANTIBAN_UNBLOCKED_ALERT) {
            stat_clr = color_red;
            stat_lbl = cur_lang == 1 ? "[ALERTE: ACCESSIBLE]" : "[ALERT: UNBLOCKED]";
        } else if (antiban_targets[ti].state == ANTIBAN_TESTING) {
            stat_clr = color_yellow;
            stat_lbl = cur_lang == 1 ? "[TEST EN COURS...]" : "[TESTING...]";
        }

        draw_text(r, font_xs, antiban_targets[ti].domain, 70, target_y, color_white, 0);
        draw_text(r, font_xs, stat_lbl, 410, target_y, stat_clr, 0);
        draw_text(r, font_xs, antiban_targets[ti].resolved_ip, 590, target_y, color_grey, 0);
        target_y += 24;
    }

    // Right Column: Overall Shield Status Box
    int box_x = 750, box_y = antiban_y + 80 - sc, box_w = 450, box_h = 65;
    SDL_Color box_border = (antiban_overall == ANTIBAN_SAFE_BLOCKED) ? color_green :
                           (antiban_overall == ANTIBAN_UNBLOCKED_ALERT) ? color_red : color_card_border;
    draw_rounded_box(r, box_x, box_y, box_w, box_h, 8, color_card);
    draw_rounded_rect(r, box_x, box_y, box_w, box_h, 8, box_border);

    if (antiban_overall == ANTIBAN_SAFE_BLOCKED) {
        draw_text_w(r, font_sm,
            cur_lang == 1 ? "BOUCLIER ACTIF : Console Protegee !" :
            "SHIELD ACTIVE: Console Protected!",
            box_x + box_w / 2, box_y + 12, color_green, 1, box_w - 20);
        draw_text_w(r, font_xs,
            cur_lang == 1 ? "Toute communication vers Nintendo est neutralisee." :
            "All telemetry traffic is safely blocked by DNS.",
            box_x + box_w / 2, box_y + 36, color_white, 1, box_w - 20);
    } else if (antiban_overall == ANTIBAN_UNBLOCKED_ALERT) {
        draw_text_w(r, font_sm,
            cur_lang == 1 ? "DANGER : Telemetrie Nintendo active !" :
            "WARNING: Nintendo Telemetry Accessible!",
            box_x + box_w / 2, box_y + 12, color_red, 1, box_w - 20);
        draw_text_w(r, font_xs,
            cur_lang == 1 ? "Attention au risque de ban ! Configurez DNS.mitm." :
            "Risk of console ban! Setup DNS.mitm / 90DNS immediately.",
            box_x + box_w / 2, box_y + 36, color_yellow, 1, box_w - 20);
    } else if (antiban_overall == ANTIBAN_TESTING) {
        draw_text_w(r, font_sm,
            cur_lang == 1 ? "Verification du bouclier en cours..." :
            "Testing anti-ban shield...",
            box_x + box_w / 2, box_y + 22, color_yellow, 1, box_w - 20);
    } else {
        draw_text_w(r, font_sm,
            cur_lang == 1 ? "Etat du bouclier non verifie" :
            "Shield status not verified yet",
            box_x + box_w / 2, box_y + 12, color_grey, 1, box_w - 20);
        draw_text_w(r, font_xs,
            cur_lang == 1 ? "Appuyez sur le bouton ci-dessous pour tester" :
            "Tap the test button below to verify protection",
            box_x + box_w / 2, box_y + 36, color_grey, 1, box_w - 20);
    }

    // Action button
    int test_btn_y = antiban_y + 160 - sc;
    draw_rounded_box(r, 880, test_btn_y, 320, 38, 7, antiban_running ? color_dark_grey : color_card_border);
    draw_rounded_rect(r, 880, test_btn_y, 320, 38, 7, color_cyan);
    draw_text_w(r, font_sm,
        antiban_running ? (cur_lang == 1 ? "Test en cours..." : "Testing...") :
        (cur_lang == 1 ? "[Touch] Tester le Bouclier" : "[Touch] Test Anti-Ban Shield"),
        1040, test_btn_y + 8, color_white, 1, 300);

    SDL_RenderSetClipRect(r, NULL);

    draw_modern_scrollbar(r, view_top, view_bottom, net_scroll, net_scroll_max);
}

// FTP & MTP Server Dashboard
static void draw_pg3(SDL_Renderer *r) {
    int content_bottom = 785;
    int view_top = 135, view_bottom = 655;
    int view_h = view_bottom - view_top;
    int content_h = content_bottom - view_top;
    transfer_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (transfer_scroll > transfer_scroll_max) transfer_scroll = transfer_scroll_max;
    if (transfer_scroll < 0) transfer_scroll = 0;

    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = transfer_scroll;

    draw_text_w(r, font_md, s_transfer_dashboard_title[cur_lang],
        40, 140 - sc, color_cyan, 0, 1160);
    draw_text_w(r, font_sm, s_transfer_file_hint[cur_lang],
        40, 168 - sc, color_grey, 0, 1160);
    draw_text_w(r, font_xs, s_game_package_hint[cur_lang],
        40, 192 - sc, color_yellow, 0, 1160);

    char t[1024];
    char ipstr[32] = "0.0.0.0";
    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip)
        snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);

    int cards_y = 220 - sc;
    const char *mode_name = ftp_mode ? "FTPD" : "FTP";
    char card1_title[64];
    snprintf(card1_title, sizeof(card1_title), s_wifi_transfer[cur_lang], mode_name);
    draw_card(r, 40, cards_y, 580, 320, card1_title);
    
    // Status button
    if (ftp_on) {
        draw_rounded_box(r, 70, cards_y + 55, 140, 34, 6, color_green);
        draw_text_w(r, font_sm, s_running[cur_lang], 140, cards_y + 61, color_bg, 1, 130);
    } else {
        draw_rounded_box(r, 70, cards_y + 55, 140, 34, 6, color_red);
        draw_text_w(r, font_sm, s_stopped[cur_lang], 140, cards_y + 61, color_white, 1, 130);
    }

    snprintf(t, sizeof(t), "%s://%s:%d", ftp_mode ? "ftpd" : "ftp", ipstr, ftp_get_port());
    draw_key_value(r, s_address[cur_lang], t, 70, cards_y + 100, color_cyan);
    draw_key_value(r, s_current_dir[cur_lang], ftp_cwd, 70, cards_y + 128, color_yellow);
    snprintf(t, sizeof(t), "%u total (%u up, %u down)", ftp_xfer_count, ftp_upload_count, ftp_download_count);
    draw_key_value(r, s_transfers[cur_lang], t, 70, cards_y + 156, color_white);
    if (ftp_bytes_total > 0) {
        format_bytes(ftp_bytes_total, t, sizeof(t));
        draw_key_value(r, s_data_total[cur_lang], t, 70, cards_y + 184, color_white);
    }
    if (ftp_xfer_active) {
        u64 done = ftp_xfer_done;
        u64 total = ftp_xfer_total;
        u64 tick = armGetSystemTick();
        u64 freq = armGetSystemTickFreq();
        double elapsed = (freq && tick > ftp_xfer_start_tick)
            ? (double)(tick - ftp_xfer_start_tick) / (double)freq : 0.0;
        char speed_text[32];
        format_bytes(elapsed > 0.0 ? (u64)((double)done / elapsed) : 0, speed_text, sizeof(speed_text));
        draw_rounded_box(r, 70, cards_y + 205, 500, 10, 3, color_dark_grey);
        if (total > 0) {
            double ratio = (double)done / (double)total;
            if (ratio > 1.0) ratio = 1.0;
            int fill = (int)(ratio * 500.0);
            if (fill > 0) draw_rounded_box(r, 70, cards_y + 205, fill, 10, 3, color_green);
            char done_text[32], total_text[32];
            format_bytes(done, done_text, sizeof(done_text));
            format_bytes(total, total_text, sizeof(total_text));
            snprintf(t, sizeof(t), "%s: %s  %s / %s (%d%%)  %s/s",
                ftp_xfer_is_upload ? s_ftp_uploading[cur_lang] : s_ftp_downloading[cur_lang],
                ftp_xfer_name, done_text, total_text, (int)(ratio * 100.0), speed_text);
            if (done > 0 && done < total && elapsed > 0.1) {
                char eta[32];
                format_duration((u64)((double)(total - done) * elapsed / (double)done), eta, sizeof(eta));
                snprintf(t, sizeof(t), s_transfer_eta[cur_lang], eta);
                draw_text_w(r, font_xs, t, 70, cards_y + 234, color_cyan, 0, 500);
            }
        } else {
            const int bar_width = 500, segment_width = 90;
            int phase = freq ? (int)((tick / (freq / 60 + 1)) % (bar_width - segment_width + 1)) : 0;
            draw_rounded_box(r, 70 + phase, cards_y + 205, segment_width, 10, 3, color_cyan);
            char done_text[32];
            format_bytes(done, done_text, sizeof(done_text));
            snprintf(t, sizeof(t), "%s: %s  %s  %s/s",
                ftp_xfer_is_upload ? s_ftp_uploading[cur_lang] : s_ftp_downloading[cur_lang],
                ftp_xfer_name, done_text, speed_text);
        }
        draw_text_w(r, font_xs, t, 70, cards_y + 218, color_white, 0, 520);
    }

    int mode_sel_x = 70, mode_sel_y = cards_y + 252;
    SDL_Color col_ftp = ftp_mode ? color_grey : color_white;
    SDL_Color col_ftpd = ftp_mode ? color_white : color_grey;
    SDL_Color col_bg_ftp = ftp_mode ? color_card_border : color_cyan;
    SDL_Color col_bg_ftpd = ftp_mode ? color_cyan : color_card_border;
    draw_rounded_box(r, mode_sel_x, mode_sel_y, 56, 26, 4, col_bg_ftp);
    draw_rounded_rect(r, mode_sel_x, mode_sel_y, 56, 26, 4, color_card_border);
    draw_text(r, font_sm, "FTP", mode_sel_x + 28, mode_sel_y + 3, col_ftp, 1);
    draw_rounded_box(r, mode_sel_x + 64, mode_sel_y, 60, 26, 4, col_bg_ftpd);
    draw_rounded_rect(r, mode_sel_x + 64, mode_sel_y, 60, 26, 4, color_card_border);
    draw_text(r, font_sm, "FTPD", mode_sel_x + 94, mode_sel_y + 3, col_ftpd, 1);
    draw_text(r, font_xs, s_toggle_wifi[cur_lang], 70, cards_y + 284, color_grey, 0);

    draw_card(r, 660, cards_y, 580, 320, s_mtp_title[cur_lang]);
    if (mtp_on) {
        draw_rounded_box(r, 690, cards_y + 55, 140, 34, 6, color_green);
        draw_text_w(r, font_sm, s_active[cur_lang], 760, cards_y + 61, color_bg, 1, 130);
    } else {
        draw_rounded_box(r, 690, cards_y + 55, 140, 34, 6, color_red);
        draw_text_w(r, font_sm, s_stopped[cur_lang], 760, cards_y + 61, color_white, 1, 130);
    }
    draw_key_value(r, s_protocol[cur_lang], s_mtp_protocol[cur_lang], 690, cards_y + 100, color_cyan);
    snprintf(t, sizeof(t), s_mtp_xfers_fmt[cur_lang], (unsigned long long)mtp_xfer_count);
    draw_key_value(r, s_transfers[cur_lang], t, 690, cards_y + 128, color_white);
    if (mtp_bytes_total > 0) {
        format_bytes(mtp_bytes_total, t, sizeof(t));
        draw_key_value(r, s_data_total[cur_lang], t, 690, cards_y + 156, color_white);
    }
    draw_text(r, font_sm, s_mtp_desc[cur_lang], 690, cards_y + 188, color_grey, 0);

    if (mtp_xfer_active) {
        if (!mtp_xfer_start_tick) mtp_xfer_start_tick = armGetSystemTick();

        draw_rounded_box(r, 690, cards_y + 215, 500, 10, 3, color_dark_grey);
        u64 done = mtp_xfer_done;
        u64 total = mtp_xfer_total;
        float pct = (total > 0) ? (float)done / total : 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        if (pct > 0.0f)
            draw_rounded_box(r, 690, cards_y + 215, (int)(pct * 500), 10, 3, color_green);

        const char *unit = "B";
        double ddone = (double)done, dtotal = (double)total;
        if (dtotal > 1073741824.0) { ddone /= 1.073741824e9; dtotal /= 1.073741824e9; unit = "GB"; }
        else if (dtotal > 1048576.0) { ddone /= 1.048576e6; dtotal /= 1.048576e6; unit = "MB"; }
        else if (dtotal > 1024.0) { ddone /= 1024.0; dtotal /= 1024.0; unit = "KB"; }
        snprintf(t, sizeof(t), "%s  %.1f/%.1f %s (%d%%)", (const char*)mtp_xfer_filename,
            ddone, dtotal, unit, (int)(pct * 100));
        draw_text(r, font_xs, t, 690, cards_y + 230, color_white, 0);
        u64 tick = armGetSystemTick();
        u64 freq = armGetSystemTickFreq();
        double elapsed = (freq && tick > mtp_xfer_start_tick)
            ? (double)(tick - mtp_xfer_start_tick) / (double)freq : 0.0;
        if (total > done && done > 0 && elapsed > 0.1) {
            char eta[32];
            format_duration((u64)((double)(total - done) * elapsed / (double)done), eta, sizeof(eta));
            snprintf(t, sizeof(t), s_transfer_eta[cur_lang], eta);
            draw_text_w(r, font_xs, t, 690, cards_y + 250, color_cyan, 0, 480);
        }
    } else {
        mtp_xfer_start_tick = 0;
    }

    draw_text(r, font_sm, s_toggle_mtp[cur_lang], 690, cards_y + 284, color_grey, 0);

    // ── Activity Log Card ──
    int log_card_y = cards_y + 335;
    draw_card(r, 40, log_card_y, 1200, 210, s_activity_log[cur_lang]);
    // Copy log entries under each mutex separately to avoid deadlock risk
    char local_mtp_log[MTP_LOG_MAX][120];
    unsigned int local_mtp_log_idx;
    mutexLock(&mtp_log_mtx);
    local_mtp_log_idx = mtp_log_idx;
    for (int _i = 0; _i < MTP_LOG_MAX; _i++) {
        memcpy(local_mtp_log[_i], mtp_log[_i], sizeof(mtp_log[0]));
        local_mtp_log[_i][sizeof(mtp_log[0])-1] = 0;
    }
    mutexUnlock(&mtp_log_mtx);

    char local_ftp_log[FTP_LOG_MAX][160];
    int local_ftp_log_idx;
    mutexLock(&ftp_log_mtx);
    local_ftp_log_idx = (int)ftp_log_idx;
    for (int _i = 0; _i < FTP_LOG_MAX; _i++) {
        memcpy(local_ftp_log[_i], ftp_log[_i], sizeof(ftp_log[0]));
        local_ftp_log[_i][sizeof(ftp_log[0])-1] = 0;
    }
    mutexUnlock(&ftp_log_mtx);

    int mtp_count = (int)(local_mtp_log_idx < MTP_LOG_MAX ? local_mtp_log_idx : MTP_LOG_MAX);
    int ftp_count = local_ftp_log_idx < FTP_LOG_MAX ? local_ftp_log_idx : FTP_LOG_MAX;
    int total_logs = mtp_count + ftp_count;
    const int visible_logs = 7;
    transfer_log_scroll_max = total_logs > visible_logs ? total_logs - visible_logs : 0;
    if (transfer_log_scroll > transfer_log_scroll_max) transfer_log_scroll = transfer_log_scroll_max;
    if (transfer_log_scroll < 0) transfer_log_scroll = 0;
    int first = total_logs - visible_logs - transfer_log_scroll;
    if (first < 0) first = 0;
    int last = first + visible_logs;
    if (last > total_logs) last = total_logs;
    int py = log_card_y + 50;
    for (int row = first; row < last; ++row) {
        if (row < mtp_count) {
            unsigned i = (local_mtp_log_idx - 1u - (unsigned)row) % MTP_LOG_MAX;
            draw_text(r, font_xs, "[MTP]", 70, py + 1, color_purple, 0);
            draw_text_w(r, font_xs, local_mtp_log[i], 120, py + 2, color_white, 0, 1040);
        } else {
            unsigned i = (unsigned)local_ftp_log_idx - 1u - (unsigned)(row - mtp_count);
            draw_text(r, font_xs, "[FTP]", 70, py + 1, color_cyan, 0);
            draw_text_w(r, font_xs, local_ftp_log[i % FTP_LOG_MAX], 120, py + 2, color_white, 0, 1040);
        }
        py += 20;
    }
    if (!total_logs)
        draw_text(r, font_sm, s_no_activity[cur_lang], 70, log_card_y + 70, color_grey, 0);
    if (transfer_log_scroll_max > 0)
        draw_modern_scrollbar(r, log_card_y + 45, log_card_y + 195, transfer_log_scroll, transfer_log_scroll_max);

    SDL_RenderSetClipRect(r, NULL);
    if (transfer_scroll_max > 0)
        draw_modern_scrollbar(r, view_top, view_bottom, transfer_scroll, transfer_scroll_max);
}

// Performance Clocks - Redesigned layout
static void draw_pg4(SDL_Renderer *r) {
    char t[256];
    batt_rate_update();
    update_battery_diagnostics();

    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_bottom = 875;
    int content_h = content_bottom - view_top;
    perf_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (perf_scroll > perf_scroll_max) perf_scroll = perf_scroll_max;
    if (perf_scroll < 0) perf_scroll = 0;

    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = perf_scroll;

    // Collect hardware data
    u32 cpu=0, gpu=0, mem=0;
    s32 skin = 0;
    bool skin_ok = false;
    bool clk_ok = false;
    if (read_clock_rate(PcvModuleId_CpuBus, &cpu) &&
        read_clock_rate(PcvModuleId_GPU, &gpu) &&
        read_clock_rate(PcvModuleId_EMC, &mem)) {
        clk_ok = true;
    }
    if (R_SUCCEEDED(tcInitialize())) {
        skin_ok = R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin));
        tcExit();
    }
    soc_temp_update();

    u32 cpu_mhz = cpu/1000000, gpu_mhz = gpu/1000000, mem_mhz = mem/1000000;

    // Update history buffers
    u64 now = armGetSystemTick();
    bool sample_history = !perf_hist_last_tick || now - perf_hist_last_tick >= armGetSystemTickFreq();
    if (sample_history) {
        perf_hist_last_tick = now;

        int cpu_pct = clk_ok ? (int)((float)cpu_mhz / 1785.0f * 100.0f) : 0;
        if (cpu_pct < 0) cpu_pct = 0;
        if (cpu_pct > 100) cpu_pct = 100;
        cpu_hist[cpu_hist_pos] = cpu_pct;
        cpu_hist_pos = (cpu_hist_pos + 1) % PERF_HIST_SIZE;
        if (cpu_hist_count < PERF_HIST_SIZE) cpu_hist_count++;

        if (skin_ok) {
            u32 tpct = skin < 25000 ? 0 : (skin > 70000 ? 100 : (skin - 25000) * 100 / 45000);
            temp_hist[temp_hist_pos] = (int)tpct;
            temp_hist_pos = (temp_hist_pos + 1) % PERF_HIST_SIZE;
            if (temp_hist_count < PERF_HIST_SIZE) temp_hist_count++;
        }
        fps_hist[fps_hist_pos] = current_fps;
        fps_hist_pos = (fps_hist_pos + 1) % FPS_HIST_SIZE;
        if (fps_hist_count < FPS_HIST_SIZE) fps_hist_count++;
    }

    u64 mem_total = 0, mem_used = 0;
    bool mem_ok = R_SUCCEEDED(svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
                  R_SUCCEEDED(svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0));
    float mem_pct_f = mem_ok && mem_total > 0 ? (float)mem_used / mem_total : 0;
    if (sample_history) {
        int mem_pct = (int)(mem_pct_f * 100.0f);
        if (mem_pct < 0) mem_pct = 0;
        if (mem_pct > 100) mem_pct = 100;
        mem_hist[mem_hist_pos] = mem_pct;
        mem_hist_pos = (mem_hist_pos + 1) % PERF_HIST_SIZE;
        if (mem_hist_count < PERF_HIST_SIZE) mem_hist_count++;
    }
    // Alerts
    if (skin_ok) {
        if (skin >= temp_alert_millic) { if (!temp_alert_notified) { add_alert("ALERT: High Temp!"); temp_alert_notified = true; } }
        else if (skin < temp_alert_millic - 5000) { temp_alert_notified = false; }
    }
    if (mem_pct_f > mem_alert_frac) { if (!mem_alert_notified) { add_alert("ALERT: High Mem Usage!"); mem_alert_notified = true; } }
    else if (mem_pct_f < mem_alert_frac - 0.05f) { mem_alert_notified = false; }

    // ═══════════════════════════════════════════════════════
    // ROW 1: Live Clock Dashboard (3 gauges side by side)
    // ═══════════════════════════════════════════════════════
    draw_card(r, 40, 140 - sc, 1200, 135, s_live_clocks[cur_lang]);

    if (clk_ok) {
        draw_rounded_box(r, 70, 198 - sc, 370, 65, 6, color_bg3);
        draw_text(r, font_md, s_cpu_clock[cur_lang], 85, 201 - sc, color_cyan, 0);
        snprintf(t, sizeof(t), "%u / 1785 MHz", cpu_mhz);
        draw_text(r, font_sm, t, 85, 229 - sc, color_white, 0);
        draw_progress_bar(r, 85, 253 - sc, 340, 6, (float)cpu_mhz / 1785.0f, color_cyan, color_dark_grey);

        draw_rounded_box(r, 455, 198 - sc, 370, 65, 6, color_bg3);
        draw_text(r, font_md, s_gpu_clock[cur_lang], 470, 201 - sc, color_yellow, 0);
        snprintf(t, sizeof(t), "%u / 921 MHz", gpu_mhz);
        draw_text(r, font_sm, t, 470, 229 - sc, color_white, 0);
        draw_progress_bar(r, 470, 253 - sc, 340, 6, (float)gpu_mhz / 921.0f, color_yellow, color_dark_grey);

        draw_rounded_box(r, 840, 198 - sc, 370, 65, 6, color_bg3);
        draw_text(r, font_md, s_mem_bus[cur_lang], 855, 201 - sc, color_green, 0);
        snprintf(t, sizeof(t), "%u / 1600 MHz", mem_mhz);
        draw_text(r, font_sm, t, 855, 229 - sc, color_white, 0);
        draw_progress_bar(r, 855, 253 - sc, 340, 6, (float)mem_mhz / 1600.0f, color_green, color_dark_grey);
    } else {
        draw_text(r, font_sm, s_failed_read_clocks[cur_lang], 70, 218 - sc, color_red, 0);
    }

    // ═══════════════════════════════════════════════════════
    // ROW 2: Full-width FPS history
    // ═══════════════════════════════════════════════════════
    int chart_y = 290;
    int chart_h = 205;
    SDL_Color fps_color = current_fps >= 55 ? color_green : (current_fps >= 30 ? color_yellow : color_red);

    // ── FPS History (connected line + fill) ──
    draw_card(r, 40, chart_y - sc, 1200, chart_h, s_fps_history[cur_lang]);
    int hc = fps_hist_count > 60 ? 60 : fps_hist_count;
    int fps_cx = 105, fps_cy = chart_y + 58 - sc, fps_cw = 1105, fps_ch = 80;
    draw_rounded_rect(r, fps_cx, fps_cy, fps_cw, fps_ch, 4, color_dark_grey);

    for (int g = 0; g <= 4; g++) {
        int gx = fps_cx + (fps_cw - 1) * g / 4;
        int gy = fps_cy + (fps_ch - 1) * g / 2;
        lineRGBA(r, gx, fps_cy, gx, fps_cy + fps_ch - 1, 100, 100, 100, 45);
        if (g <= 2)
            lineRGBA(r, fps_cx, gy, fps_cx + fps_cw - 1, gy, 100, 100, 100, 65);
    }
    draw_text(r, font_xs, "60", 72, fps_cy - 6, color_grey, 0);
    draw_text(r, font_xs, "30", 72, fps_cy + fps_ch / 2 - 7, color_yellow, 0);
    draw_text(r, font_xs, "0", 79, fps_cy + fps_ch - 13, color_grey, 0);
    int ref30y = fps_cy + fps_ch / 2;
    lineRGBA(r, fps_cx, ref30y, fps_cx + fps_cw - 1, ref30y, 255, 180, 70, 120);

    if (hc > 0) {
        Sint16 xpts[FPS_HIST_SIZE+2], ypts[FPS_HIST_SIZE+2];
        int npts = hc;
        for (int i = 0; i < npts; i++) {
            int fv = fps_hist[(fps_hist_pos - npts + i + FPS_HIST_SIZE) % FPS_HIST_SIZE];
            if (fv > 60) fv = 60;
            if (fv < 0) fv = 0;
            xpts[i] = fps_cx + fps_cw - 1 - (npts - 1 - i) * (fps_cw - 1) / (FPS_HIST_SIZE - 1);
            ypts[i] = fps_cy + fps_ch - 1 - (fv * (fps_ch - 1) / 60);
        }
        if (npts >= 2) {
            Sint16 fx[FPS_HIST_SIZE+4], fy[FPS_HIST_SIZE+4];
            for (int i = 0; i < npts; i++) { fx[i] = xpts[i]; fy[i] = ypts[i]; }
            fx[npts] = xpts[npts-1]; fy[npts] = fps_cy + fps_ch - 1;
            fx[npts+1] = xpts[0]; fy[npts+1] = fps_cy + fps_ch - 1;
            filledPolygonRGBA(r, fx, fy, npts+2, 0, 210, 255, 40);
        }
        for (int i = 0; i < npts - 1; i++) {
            int fv1 = fps_hist[(fps_hist_pos - npts + i + FPS_HIST_SIZE) % FPS_HIST_SIZE];
            int fv2 = fps_hist[(fps_hist_pos - npts + i + 1 + FPS_HIST_SIZE) % FPS_HIST_SIZE];
            SDL_Color lc = (fv1 >= 55 && fv2 >= 55) ? color_green : ((fv1 >= 30 || fv2 >= 30) ? color_yellow : color_red);
            thickLineRGBA(r, xpts[i], ypts[i], xpts[i+1], ypts[i+1], 3, lc.r, lc.g, lc.b, 220);
        }
        filledCircleRGBA(r, xpts[npts-1], ypts[npts-1], 4, color_white.r, color_white.g, color_white.b, 255);
        filledCircleRGBA(r, xpts[npts-1], ypts[npts-1], 2, fps_color.r, fps_color.g, fps_color.b, 255);
    }
    snprintf(t, sizeof(t), s_current_fps_fmt[cur_lang], current_fps);
    draw_text(r, font_md, t, 70, chart_y + 151 - sc, fps_color, 0);
    int fps_min, fps_avg, fps_max;
    if (history_stats(fps_hist, fps_hist_count, fps_hist_pos, FPS_HIST_SIZE,
        &fps_min, &fps_avg, &fps_max)) {
        snprintf(t, sizeof(t), s_history_stats_fmt[cur_lang], fps_min, fps_avg, fps_max);
        draw_text_w(r, font_xs, t, 1215, chart_y + 151 - sc, color_grey, 2, 420);
    }
    draw_text(r, font_xs, s_history_oldest[cur_lang], fps_cx, chart_y + 178 - sc, color_grey, 0);
    draw_text_w(r, font_xs, s_history_now[cur_lang], fps_cx + fps_cw - 5,
        chart_y + 178 - sc, color_grey, 2, 100);

    // ═══════════════════════════════════════════════════════
    // ROW 3: System Health (3 columns, lowered thermal text)
    // ═══════════════════════════════════════════════════════
    int row3_y = 515 - sc;
    int r3w = 385;
    int r3h = 180;

    // ── Thermal Status (lowered text) ──
    draw_card(r, 40, row3_y, r3w, r3h, s_thermal_status[cur_lang]);
    if (skin > 0) {
        draw_temperature(r, font_md, skin, 70, row3_y + 52, get_temp_color(skin), 0);
        u32 tpct = skin < 25000 ? 0 : (skin > 70000 ? 100 : (skin - 25000) * 100 / 45000);
        draw_progress_bar(r, 185, row3_y + 56, 140, 16, tpct / 100.f, get_temp_color(skin), color_dark_grey);
        snprintf(t, sizeof(t), "%u%%", tpct);
        draw_text(r, font_sm, t, 405, row3_y + 56, color_grey, 2);
        const char *label = skin >= 55000 ? s_critical_limit[cur_lang] : (skin >= 45000 ? s_warm_hot[cur_lang] : s_cool[cur_lang]);
        draw_text(r, font_sm, label, 70, row3_y + 92, get_temp_color(skin), 0);
        if (soc_temp_valid) {
            snprintf(t, sizeof(t), "%s: %d \xC2\xB0" "C", s_soc_temp[cur_lang], soc_temp_c);
            draw_text(r, font_sm, t, 70, row3_y + 118, get_temp_color(soc_temp_c * 1000), 0);
        }
    } else {
        draw_text(r, font_sm, "N/A", 70, row3_y + 54, color_grey, 0);
    }

    // ── Application process memory usage ──
    draw_card(r, 440, row3_y, r3w, r3h, s_ram_usage[cur_lang]);
    if (mem_ok) {
        snprintf(t, sizeof(t), "%.1f / %.1f MB", mem_used / 1048576.0, mem_total / 1048576.0);
        draw_text(r, font_sm, t, 470, row3_y + 52, color_cyan, 0);
        draw_progress_bar(r, 470, row3_y + 82, 350, 18, mem_pct_f, color_cyan, color_dark_grey);
        float pct100 = mem_pct_f * 100;
        SDL_Color pcol = pct100 > 80 ? color_red : (pct100 > 60 ? color_yellow : color_green);
        snprintf(t, sizeof(t), "%.1f%%", pct100);
        draw_text(r, font_sm, t, 470, row3_y + 114, pcol, 0);
        const char *slabel = pct100 > 85 ? s_mem_status_high[cur_lang] : (pct100 > 70 ? s_mem_status_elevated[cur_lang] : s_normal[cur_lang]);
        draw_text(r, font_sm, slabel, 580, row3_y + 114, pcol, 0);
    } else {
        draw_text(r, font_sm, s_ram_info_na[cur_lang], 470, row3_y + 54, color_grey, 0);
    }

    // ── Performance Profile & Export ──
    draw_card(r, 855, row3_y, r3w, r3h, s_perf_export[cur_lang]);
    if (clk_ok) {
        u32 cpu_mhz_val = cpu_mhz;
        const char *prof = s_power_save[cur_lang];
        SDL_Color pcol = color_green;
        if (cpu_mhz_val >= 1500) { prof = s_boost_profile[cur_lang]; pcol = color_red; }
        else if (cpu_mhz_val >= 1020) { prof = s_high_perf[cur_lang]; pcol = color_yellow; }
        snprintf(t, sizeof(t), "%s: %s", s_active_profile[cur_lang], prof);
        draw_text(r, font_sm, t, 885, row3_y + 48, pcol, 0);
        int dock = appletGetOperationMode();
        draw_text(r, font_sm, dock ? s_docked_high[cur_lang] : s_handheld_throttled[cur_lang], 885, row3_y + 70, dock ? color_green : color_cyan, 0);
        struct stat st_sysclk;
        bool has_sysclk = (stat("sdmc:/config/sys-clk/config.ini", &st_sysclk) == 0 || stat("sdmc:/atmosphere/contents/00FF0000636C6BFF", &st_sysclk) == 0);
        if (has_sysclk) {
            draw_text(r, font_xs, cur_lang == 1 ? "sys-clk: Detecte (OC actif)" : "sys-clk: Detected (OC active)", 885, row3_y + 92, color_green, 0);
        } else {
            draw_text(r, font_xs, cur_lang == 1 ? "sys-clk: Non detecte (Stock)" : "sys-clk: Not detected (Stock)", 885, row3_y + 92, color_grey, 0);
        }
        u32 score = (cpu_mhz * 10 + gpu_mhz * 5 + mem_mhz * 2) / 20;
        snprintf(t, sizeof(t), s_perf_score_fmt[cur_lang], score);
        SDL_Color scol = score > 1300 ? color_green : (score > 800 ? color_yellow : color_grey);
        draw_text(r, font_xs, t, 885, row3_y + 109, scol, 0);
    }
    draw_rounded_box(r, 885, row3_y + 126, 330, 27, 6, color_card_border);
    draw_rounded_rect(r, 885, row3_y + 126, 330, 27, 6, color_cyan);
    draw_text_w(r, font_xs, s_export_perf_hint[cur_lang],
        1050, row3_y + 132, color_cyan, 1, 314);
    if (perf_export_msg[0]) {
        u64 elapsed = (armGetSystemTick() - perf_export_msg_tick) / armGetSystemTickFreq();
        if (elapsed < 5) {
            draw_text_w(r, font_xs, perf_export_msg, 885, row3_y + 155,
                perf_export_success ? color_green : color_red, 0, 330);
        } else {
            perf_export_msg[0] = 0;
        }
    }
    if (!perf_export_msg[0])
        draw_text_w(r, font_xs, "sdmc:/switch/SwitchInfoNX/perf_metrics_v06.csv",
            885, row3_y + 155, color_grey, 0, 350);

    // ── Row 4: Temperature History Graph ──
    int temp_graph_y = row3_y + r3h + 20;
    draw_card(r, 40, temp_graph_y, 1200, 160, s_skin_history[cur_lang]);
    int graph_lx = 120, graph_rx = 1210;
    int graph_w = graph_rx - graph_lx;
    int graph_top = temp_graph_y + 59;
    int graph_h = 61;
    int graph_by = graph_top + graph_h - 1;
    draw_rounded_rect(r, graph_lx, graph_top, graph_w, graph_h, 4, color_dark_grey);
    for (int gi = 0; gi <= 2; gi++) {
        int gy = graph_top + gi * (graph_h - 1) / 2;
        int gx = graph_lx + gi * (graph_w - 1) / 2;
        lineRGBA(r, graph_lx, gy, graph_rx - 1, gy, 100, 100, 100, 65);
        lineRGBA(r, gx, graph_top, gx, graph_by, 100, 100, 100, 45);
    }
    if (temp_alert_millic >= 25000 && temp_alert_millic <= 70000) {
        int alert_pct = (temp_alert_millic - 25000) * 100 / 45000;
        int alert_y = graph_by - alert_pct * (graph_h - 1) / 100;
        lineRGBA(r, graph_lx, alert_y, graph_rx - 1, alert_y, color_red.r, color_red.g, color_red.b, 150);
    }
    if (temp_hist_count > 0) {
        int start = temp_hist_count < PERF_HIST_SIZE ? 0 : temp_hist_pos;
        Sint16 xpts[PERF_HIST_SIZE], ypts[PERF_HIST_SIZE];
        for (int gi = 0; gi < temp_hist_count; gi++) {
            int idx = (start + gi) % PERF_HIST_SIZE;
            int val = temp_hist[idx];
            xpts[gi] = graph_lx + graph_w - 1 - (temp_hist_count - 1 - gi) * (graph_w - 1) / (PERF_HIST_SIZE - 1);
            ypts[gi] = graph_by - val * (graph_h - 1) / 100;
        }
        if (temp_hist_count >= 2) {
            Sint16 fill_x[PERF_HIST_SIZE+2], fill_y[PERF_HIST_SIZE+2];
            for (int gi = 0; gi < temp_hist_count; gi++) {
                fill_x[gi] = xpts[gi];
                fill_y[gi] = ypts[gi];
            }
            fill_x[temp_hist_count] = xpts[temp_hist_count - 1];
            fill_y[temp_hist_count] = graph_by;
            fill_x[temp_hist_count + 1] = xpts[0];
            fill_y[temp_hist_count + 1] = graph_by;
            filledPolygonRGBA(r, fill_x, fill_y, temp_hist_count + 2, color_cyan.r, color_cyan.g, color_cyan.b, 38);
        }
        for (int gi = 1; gi < temp_hist_count; gi++) {
            int val = temp_hist[(start + gi) % PERF_HIST_SIZE];
            int x = xpts[gi], y = ypts[gi];
            SDL_Color line_color = val >= 80 ? color_red : (val >= 50 ? color_yellow : color_cyan);
            int prev_val = temp_hist[(start + gi - 1) % PERF_HIST_SIZE];
            SDL_Color prev_color = prev_val >= 80 ? color_red : (prev_val >= 50 ? color_yellow : color_cyan);
            thickLineRGBA(r, xpts[gi - 1], ypts[gi - 1], x, y, 3,
                line_color.r, line_color.g, line_color.b, 230);
            pixelRGBA(r, x, y, prev_color.r, prev_color.g, prev_color.b, 255);
        }
        int last = temp_hist[(start + temp_hist_count - 1) % PERF_HIST_SIZE];
        SDL_Color latest_color = last >= 80 ? color_red : (last >= 50 ? color_yellow : color_cyan);
        filledCircleRGBA(r, xpts[temp_hist_count - 1], ypts[temp_hist_count - 1], 5,
            color_white.r, color_white.g, color_white.b, 255);
        filledCircleRGBA(r, xpts[temp_hist_count - 1], ypts[temp_hist_count - 1], 3,
            latest_color.r, latest_color.g, latest_color.b, 255);
    } else {
        draw_text(r, font_sm, "Collecting data...", (graph_lx + graph_rx) / 2, graph_top + 20, color_grey, 1);
    }
    draw_text(r, font_xs, "70 C", 70, graph_top - 5, color_grey, 0);
    draw_text(r, font_xs, "48 C", 70, graph_top + graph_h / 2 - 7, color_grey, 0);
    draw_text(r, font_xs, "25 C", 70, graph_by - 13, color_grey, 0);
    draw_text(r, font_xs, s_history_oldest[cur_lang], graph_lx, temp_graph_y + 132, color_grey, 0);
    draw_text(r, font_xs, s_history_mid[cur_lang], graph_lx + graph_w / 2 - 10, temp_graph_y + 132, color_grey, 0);
    draw_text_w(r, font_xs, s_history_now[cur_lang], graph_rx,
        temp_graph_y + 132, color_grey, 2, 100);
    if (skin_ok)
        draw_temperature(r, font_sm, skin, 1210, temp_graph_y + 15, get_temp_color(skin), 2);
    SDL_RenderSetClipRect(r, NULL);

    draw_modern_scrollbar(r, view_top, view_bottom, perf_scroll, perf_scroll_max);
}
// Page 5: Controller Button and Stick Test
static void draw_button_dot(SDL_Renderer *r, const char *label, int x, int y, bool pressed, SDL_Color on_color) {
    if (pressed) {
        filledCircleRGBA(r, x, y, 16, on_color.r, on_color.g, on_color.b, 255);
        circleRGBA(r, x, y, 16, color_white.r, color_white.g, color_white.b, 200);
        draw_text(r, font_sm, label, x, y - 9, color_bg, 1);
    } else {
        filledCircleRGBA(r, x, y, 16, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
        draw_text(r, font_sm, label, x, y - 9, color_grey, 1);
    }
}
#define draw_btn(r, l, x, y, h) draw_button_dot(r, l, x, y, h, color_cyan)
#define draw_btn_g(r, l, x, y, h) draw_button_dot(r, l, x, y, h, color_green)
#define draw_btn_y(r, l, x, y, h) draw_button_dot(r, l, x, y, h, color_yellow)
#define draw_btn_r(r, l, x, y, h) draw_button_dot(r, l, x, y, h, color_orange)

static void draw_pg5(SDL_Renderer *r, PadState *pad) {
    char t[128];

    int content_bottom = 1400;
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_h = content_bottom - view_top;
    ctrl_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (ctrl_scroll > ctrl_scroll_max) ctrl_scroll = ctrl_scroll_max;
    if (ctrl_scroll < 0) ctrl_scroll = 0;
    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = ctrl_scroll;

    draw_card(r, 40, 140 - sc, 1200, 530, s_jc_title[cur_lang]);

    u64 held = padGetButtons(pad);
    HidAnalogStickState stick_l = padGetStickPos(pad, 0);
    HidAnalogStickState stick_r = padGetStickPos(pad, 1);

    // Connection type & style
    u64 style = padGetStyleSet(pad);
    HidNpadIdType npad_id = (appletGetOperationMode() == 0 || (style & HidNpadStyleTag_NpadHandheld)) ?
        HidNpadIdType_Handheld : HidNpadIdType_No1;

    // Physical Joy-Con / Controller SPI hardware colors
    HidNpadControllerColor jc_color_l = {0}, jc_color_r = {0};
    bool have_split_color = R_SUCCEEDED(hidGetNpadControllerColorSplit(npad_id, &jc_color_l, &jc_color_r));
    SDL_Color jcl_color = have_split_color ? color_from_npad(jc_color_l.main, (SDL_Color){30, 120, 255, 255}) : (SDL_Color){30, 120, 255, 255};
    SDL_Color jcr_color = have_split_color ? color_from_npad(jc_color_r.main, (SDL_Color){255, 60, 80, 255}) : (SDL_Color){255, 60, 80, 255};

    // Left Joycon body
    int jcl_x = 340, jcl_y = 230 - sc;
    draw_rounded_box(r, jcl_x, jcl_y, 180, 360, 24, jcl_color);
    draw_rounded_rect(r, jcl_x, jcl_y, 180, 360, 24, color_white);
    draw_text(r, font_sm, "L", jcl_x + 90, jcl_y + 6, color_white, 1);

    // Left stick
    int scx = jcl_x + 90, scy = jcl_y + 100;
    filledCircleRGBA(r, scx, scy, 28, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
    int off_x = (int)(stick_l.x * 20.f / 32768.f);
    int off_y = (int)(-stick_l.y * 20.f / 32768.f);
    filledCircleRGBA(r, scx + off_x, scy + off_y, 16, color_cyan.r, color_cyan.g, color_cyan.b, 255);

    // D-Pad
    draw_btn(r, "U", jcl_x + 90, jcl_y + 200, held & HidNpadButton_Up);
    draw_btn(r, "D", jcl_x + 90, jcl_y + 280, held & HidNpadButton_Down);
    draw_btn(r, "L", jcl_x + 50, jcl_y + 240, held & HidNpadButton_Left);
    draw_btn(r, "R", jcl_x + 130, jcl_y + 240, held & HidNpadButton_Right);
    draw_btn(r, "-", jcl_x + 130, jcl_y + 40, held & HidNpadButton_Minus);
    draw_btn(r, "L3", jcl_x + 50, jcl_y + 320, held & HidNpadButton_StickL);

    // Right Joycon body
    int jcr_x = 760, jcr_y = 230 - sc;
    draw_rounded_box(r, jcr_x, jcr_y, 180, 360, 24, jcr_color);
    draw_rounded_rect(r, jcr_x, jcr_y, 180, 360, 24, color_white);
    draw_text(r, font_sm, "R", jcr_x + 90, jcr_y + 6, color_white, 1);

    // ABXY (colored)
    draw_btn_g(r, "X", jcr_x + 90, jcr_y + 100, held & HidNpadButton_X);
    draw_btn_r(r, "B", jcr_x + 90, jcr_y + 180, held & HidNpadButton_B);
    draw_btn_y(r, "Y", jcr_x + 50, jcr_y + 140, held & HidNpadButton_Y);
    draw_btn_g(r, "A", jcr_x + 130, jcr_y + 140, held & HidNpadButton_A);

    // Right stick
    int rscx = jcr_x + 90, rscy = jcr_y + 240;
    filledCircleRGBA(r, rscx, rscy, 28, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
    int roff_x = (int)(stick_r.x * 20.f / 32768.f);
    int roff_y = (int)(-stick_r.y * 20.f / 32768.f);
    filledCircleRGBA(r, rscx + roff_x, rscy + roff_y, 16, color_cyan.r, color_cyan.g, color_cyan.b, 255);

    draw_btn(r, "+", jcr_x + 50, jcr_y + 40, held & HidNpadButton_Plus);
    draw_btn(r, "R3", jcr_x + 130, jcr_y + 320, held & HidNpadButton_StickR);

    int trig_y = 605 - sc;
    draw_text(r, font_sm, s_triggers[cur_lang], 70, trig_y, color_grey, 0);
    draw_btn(r, "L", 210, trig_y + 8, held & HidNpadButton_L);
    draw_btn(r, "R", 250, trig_y + 8, held & HidNpadButton_R);
    draw_btn(r, "ZL", 300, trig_y + 8, held & HidNpadButton_ZL);
    draw_btn(r, "ZR", 350, trig_y + 8, held & HidNpadButton_ZR);

    // Stick position and center reference; live offset calculation
    circleRGBA(r, scx, scy, 20, color_cyan.r, color_cyan.g, color_cyan.b, 100);
    circleRGBA(r, rscx, rscy, 20, color_cyan.r, color_cyan.g, color_cyan.b, 100);
    circleRGBA(r, scx, scy, 3, color_white.r, color_white.g, color_white.b, 190);
    circleRGBA(r, rscx, rscy, 3, color_white.r, color_white.g, color_white.b, 190);

    float mag_l = sqrtf((float)stick_l.x * (float)stick_l.x + (float)stick_l.y * (float)stick_l.y) / 32768.f * 100.f;
    float mag_r = sqrtf((float)stick_r.x * (float)stick_r.x + (float)stick_r.y * (float)stick_r.y) / 32768.f * 100.f;
    // Only track peak drift when stick is near neutral / drift range (<= 30.0%) so normal stick deflections don't ruin the peak reading
    if (mag_l <= 30.0f && mag_l > stick_l_peak_drift) stick_l_peak_drift = mag_l;
    if (mag_r <= 30.0f && mag_r > stick_r_peak_drift) stick_r_peak_drift = mag_r;

    if (mag_l > 12.0f) {
        snprintf(t, sizeof(t), "L: X=%d Y=%d [Offset: %.0f%%]", stick_l.x, stick_l.y, mag_l);
        draw_text(r, font_sm, t, 500, trig_y, color_yellow, 0);
    } else {
        snprintf(t, sizeof(t), "L: X=%d Y=%d [Near center]", stick_l.x, stick_l.y);
        draw_text(r, font_sm, t, 500, trig_y, color_green, 0);
    }

    if (mag_r > 12.0f) {
        snprintf(t, sizeof(t), "R: X=%d Y=%d [Offset: %.0f%%]", stick_r.x, stick_r.y, mag_r);
        draw_text(r, font_sm, t, 850, trig_y, color_yellow, 0);
    } else {
        snprintf(t, sizeof(t), "R: X=%d Y=%d [Near center]", stick_r.x, stick_r.y);
        draw_text(r, font_sm, t, 850, trig_y, color_green, 0);
    }
    draw_text_w(r, font_xs, s_stick_test_hint[cur_lang], 640, 634 - sc, color_grey, 1, 500);

    // Connection type header
    const char *ctype = "Unknown";
    if (style & HidNpadStyleTag_NpadHandheld) ctype = "Handheld Mode";
    else if (style & HidNpadStyleTag_NpadFullKey) ctype = "Pro Controller";
    else if (style & HidNpadStyleTag_NpadJoyDual) ctype = "Dual Joy-Con";
    else if (style & HidNpadStyleTag_NpadJoyLeft) ctype = "Left JC Only";
    else if (style & HidNpadStyleTag_NpadJoyRight) ctype = "Right JC Only";
    snprintf(t, sizeof(t), "%s: %s", s_controller_type[cur_lang], ctype);
    draw_text(r, font_sm, t, 70, 190 - sc, color_cyan, 0);

    // Controller power readings
    HidPowerInfo jc_left = {0}, jc_right = {0}, pwr_single = {0};
    hidGetNpadPowerInfoSplit(npad_id, &jc_left, &jc_right);
    hidGetNpadPowerInfoSingle(npad_id, &pwr_single);

    // Small battery badges directly on Joy-Con bodies
    draw_battery_icon(r, jcl_x + 18, jcl_y + 12, 28, 12, jc_left.battery_level * 25, jc_left.is_charging);
    draw_battery_icon(r, jcr_x + 134, jcr_y + 12, 28, 12, jc_right.battery_level * 25, jc_right.is_charging);

    // Central Controller Power Panel (between L and R Joy-Cons)
    int cp_x = 535, cp_y = 230 - sc, cp_w = 210, cp_h = 360;
    draw_rounded_box(r, cp_x, cp_y, cp_w, cp_h, 16, color_card);
    draw_rounded_rect(r, cp_x, cp_y, cp_w, cp_h, 16, color_card_border);
    // Header
    draw_rounded_box(r, cp_x, cp_y, cp_w, 36, 16, color_bg3);
    draw_text_w(r, font_sm, "Controllers Power", cp_x + cp_w/2, cp_y + 8, color_cyan, 1, cp_w - 20);

    // Joy-Con Left section
    int sec_y = cp_y + 46;
    draw_rounded_box(r, cp_x + 16, sec_y + 6, 8, 8, 4, jcl_color);
    draw_text(r, font_sm, "Joy-Con (L)", cp_x + 30, sec_y, color_white, 0);
    sec_y += 26;
    draw_battery_icon(r, cp_x + 20, sec_y + 3, 34, 14, jc_left.battery_level * 25, jc_left.is_charging);
    snprintf(t, sizeof(t), "%s", get_joycon_battery_str(jc_left.battery_level));
    draw_text(r, font_xs, t, cp_x + 62, sec_y + 2, get_joycon_battery_color(jc_left.battery_level), 0);
    sec_y += 20;
    draw_text(r, font_xs, jc_left.is_charging ? "Charging" : "On Battery",
        cp_x + 20, sec_y, jc_left.is_charging ? color_cyan : color_grey, 0);
    snprintf(t, sizeof(t), "#%02X%02X%02X", jcl_color.r, jcl_color.g, jcl_color.b);
    draw_text(r, font_xs, t, cp_x + 120, sec_y, color_grey, 0);

    // Divider
    sec_y += 28;
    lineRGBA(r, cp_x + 15, sec_y, cp_x + cp_w - 15, sec_y, color_card_border.r, color_card_border.g, color_card_border.b, 255);
    sec_y += 12;

    // Joy-Con Right section
    draw_rounded_box(r, cp_x + 16, sec_y + 6, 8, 8, 4, jcr_color);
    draw_text(r, font_sm, "Joy-Con (R)", cp_x + 30, sec_y, color_white, 0);
    sec_y += 26;
    draw_battery_icon(r, cp_x + 20, sec_y + 3, 34, 14, jc_right.battery_level * 25, jc_right.is_charging);
    snprintf(t, sizeof(t), "%s", get_joycon_battery_str(jc_right.battery_level));
    draw_text(r, font_xs, t, cp_x + 62, sec_y + 2, get_joycon_battery_color(jc_right.battery_level), 0);
    sec_y += 20;
    draw_text(r, font_xs, jc_right.is_charging ? "Charging" : "On Battery",
        cp_x + 20, sec_y, jc_right.is_charging ? color_cyan : color_grey, 0);
    snprintf(t, sizeof(t), "#%02X%02X%02X", jcr_color.r, jcr_color.g, jcr_color.b);
    draw_text(r, font_xs, t, cp_x + 120, sec_y, color_grey, 0);

    // Divider & Pro Controller info
    sec_y += 28;
    lineRGBA(r, cp_x + 15, sec_y, cp_x + cp_w - 15, sec_y, color_card_border.r, color_card_border.g, color_card_border.b, 255);
    sec_y += 12;
    if (style & HidNpadStyleTag_NpadFullKey) {
        draw_text(r, font_sm, "Pro Controller", cp_x + 20, sec_y, color_purple, 0);
        sec_y += 24;
        draw_battery_icon(r, cp_x + 20, sec_y + 3, 34, 14, pwr_single.battery_level * 25, pwr_single.is_charging);
        snprintf(t, sizeof(t), "%s", get_joycon_battery_str(pwr_single.battery_level));
        draw_text(r, font_xs, t, cp_x + 62, sec_y + 2, get_joycon_battery_color(pwr_single.battery_level), 0);
    } else {
        draw_text(r, font_xs, "Vibration / Rumble: OK", cp_x + 20, sec_y, color_green, 0);
        sec_y += 20;
        draw_text(r, font_xs, "Six-Axis Gyro: Ready", cp_x + 20, sec_y, color_grey, 0);
    }

    // Gyro & Accel Sensor card
    draw_card(r, 40, 680 - sc, 1200, 240, s_gyro_accel[cur_lang]);
    if (sixaxis_init_ok) {
        HidSixAxisSensorState states[2];
        bool have_two = R_SUCCEEDED(hidGetSixAxisSensorStates(sixaxis_handles[1], states + 1, 1));
        hidGetSixAxisSensorStates(sixaxis_handles[0], states, 1);
        HidSixAxisSensorState *s0 = &states[0];
        HidSixAxisSensorState *s1 = have_two ? &states[1] : s0;

        draw_text(r, font_sm, have_two ? s_left_jc[cur_lang] : "Sensor", 70, 740 - sc, color_cyan, 0);
        snprintf(t, sizeof(t), "Gyro: X=%.1f Y=%.1f Z=%.1f deg/s",
            s0->angular_velocity.x * 57.2958f, s0->angular_velocity.y * 57.2958f, s0->angular_velocity.z * 57.2958f);
        draw_text(r, font_sm, t, 70, 770 - sc, color_white, 0);
        snprintf(t, sizeof(t), "Accel: X=%.2f Y=%.2f Z=%.2f G",
            s0->acceleration.x, s0->acceleration.y, s0->acceleration.z);
        draw_text(r, font_sm, t, 70, 795 - sc, color_white, 0);
        snprintf(t, sizeof(t), "Angle: X=%.1f Y=%.1f Z=%.1f deg",
            s0->angle.x * 57.2958f, s0->angle.y * 57.2958f, s0->angle.z * 57.2958f);
        draw_text(r, font_sm, t, 70, 820 - sc, color_grey, 0);

        if (have_two) {
            draw_text(r, font_sm, s_right_jc[cur_lang], 660, 740 - sc, color_cyan, 0);
            snprintf(t, sizeof(t), "Gyro: X=%.1f Y=%.1f Z=%.1f deg/s",
                s1->angular_velocity.x * 57.2958f, s1->angular_velocity.y * 57.2958f, s1->angular_velocity.z * 57.2958f);
            draw_text(r, font_sm, t, 660, 770 - sc, color_white, 0);
            snprintf(t, sizeof(t), "Accel: X=%.2f Y=%.2f Z=%.2f G",
                s1->acceleration.x, s1->acceleration.y, s1->acceleration.z);
            draw_text(r, font_sm, t, 660, 795 - sc, color_white, 0);
            snprintf(t, sizeof(t), "Angle: X=%.1f Y=%.1f Z=%.1f deg",
                s1->angle.x * 57.2958f, s1->angle.y * 57.2958f, s1->angle.z * 57.2958f);
            draw_text(r, font_sm, t, 660, 820 - sc, color_grey, 0);
        }
    } else {
        draw_text(r, font_sm, s_sixaxis_na[cur_lang], 70, 740 - sc, color_grey, 0);
    }

    // HD Rumble & Haptic Motor Test Card
    int rmb_card_y = 940 - sc;
    draw_card(r, 40, rmb_card_y, 1200, 160, cur_lang == 1 ? "Test Vibration Haptique HD & Moteurs" : "HD Rumble & Haptic Motor Test");
    draw_text_w(r, font_sm,
        cur_lang == 1 ? "Declenchez une pulsation haptique sur chaque manette pour tester les actionneurs lineaires (LRA)." :
        "Trigger instant haptic pulse vibrations to test Joy-Con Linear Resonant Actuators.",
        70, rmb_card_y + 45, color_grey, 0, 1140);

    u64 now_vibe = armGetSystemTick();
    bool l_rumbling = (ctrl_rumble_until[0] > 0 && now_vibe < ctrl_rumble_until[0]);
    bool r_rumbling = (ctrl_rumble_until[1] > 0 && now_vibe < ctrl_rumble_until[1]);

    int rmb_btn_y = rmb_card_y + 88;
    // Button Left
    draw_rounded_box(r, 70, rmb_btn_y, 340, 48, 8, l_rumbling ? color_green : color_card);
    draw_rounded_rect(r, 70, rmb_btn_y, 340, 48, 8, l_rumbling ? color_white : color_cyan);
    draw_text_w(r, font_sm, cur_lang == 1 ? "[Touch] Moteur Gauche (L)" : "[Touch] Left Motor (L)", 240, rmb_btn_y + 14, color_white, 1, 320);

    // Button Both
    draw_rounded_box(r, 450, rmb_btn_y, 340, 48, 8, (l_rumbling && r_rumbling) ? color_yellow : color_card);
    draw_rounded_rect(r, 450, rmb_btn_y, 340, 48, 8, (l_rumbling && r_rumbling) ? color_white : color_card_border);
    draw_text_w(r, font_sm, cur_lang == 1 ? "[Touch] Deux Moteurs (L+R)" : "[Touch] Dual Rumble (L+R)", 620, rmb_btn_y + 14, color_white, 1, 320);

    // Button Right
    draw_rounded_box(r, 830, rmb_btn_y, 340, 48, 8, r_rumbling ? color_orange : color_card);
    draw_rounded_rect(r, 830, rmb_btn_y, 340, 48, 8, r_rumbling ? color_white : color_cyan);
    draw_text_w(r, font_sm, cur_lang == 1 ? "[Touch] Moteur Droit (R)" : "[Touch] Right Motor (R)", 1000, rmb_btn_y + 14, color_white, 1, 320);

    // Stick Drift & Deadzone Live Analyzer Card
    int drift_card_y = 1120 - sc;
    draw_card(r, 40, drift_card_y, 1200, 240, cur_lang == 1 ? "Analyseur de Derive & Zone Morte (Stick Drift)" : "Stick Drift & Deadzone Live Analyzer");
    draw_text_w(r, font_xs,
        cur_lang == 1 ? "Laissez les sticks au repos pour mesurer le micro-jitter et verifier la precision du point neutre." :
        "Release sticks to measure rest jitter, neutral point precision, and identify stick drift.",
        70, drift_card_y + 45, color_grey, 0, 1140);

    int box_w = 560;
    int box_h = 108;
    int box_l_x = 60;
    int box_r_x = 660;
    int box_y = drift_card_y + 68;

    // Stick Left Sub-Panel
    draw_rounded_box(r, box_l_x, box_y, box_w, box_h, 8, color_bg3);
    draw_rounded_rect(r, box_l_x, box_y, box_w, box_h, 8, color_card_border);
    draw_rounded_box(r, box_l_x + 14, box_y + 14, 10, 10, 5, jcl_color);
    draw_text(r, font_sm, cur_lang == 1 ? "Stick Gauche (L)" : "Left Stick (L)", box_l_x + 32, box_y + 10, color_white, 0);
    snprintf(t, sizeof(t), "X=%5d  Y=%5d", stick_l.x, stick_l.y);
    draw_text(r, font_xs, t, box_l_x + 380, box_y + 12, color_grey, 0);

    snprintf(t, sizeof(t), "Deviation: %.1f%%  (Peak: %.1f%%)", mag_l, stick_l_peak_drift);
    SDL_Color l_stat_col = mag_l < 8.0f ? color_green : (mag_l < 18.0f ? color_yellow : color_red);
    draw_text(r, font_sm, t, box_l_x + 32, box_y + 36, l_stat_col, 0);
    draw_progress_bar(r, box_l_x + 32, box_y + 60, box_w - 64, 8, mag_l > 100.0f ? 1.0f : (mag_l / 100.0f), l_stat_col, color_card);

    const char *l_status = mag_l < 8.0f ? (cur_lang == 1 ? "Etat: Centrage Parfait (Aucun Drift)" : "Status: Perfect Center (No Drift)") :
                          (mag_l < 18.0f ? (cur_lang == 1 ? "Etat: Zone morte normale" : "Status: Minor Deadzone Jitter") :
                                          (cur_lang == 1 ? "Etat: DERIVE DETECTEE (Stick Drift)" : "Status: STICK DRIFT DETECTED"));
    draw_text(r, font_xs, l_status, box_l_x + 32, box_y + 78, l_stat_col, 0);

    // Stick Right Sub-Panel
    draw_rounded_box(r, box_r_x, box_y, box_w, box_h, 8, color_bg3);
    draw_rounded_rect(r, box_r_x, box_y, box_w, box_h, 8, color_card_border);
    draw_rounded_box(r, box_r_x + 14, box_y + 14, 10, 10, 5, jcr_color);
    draw_text(r, font_sm, cur_lang == 1 ? "Stick Droit (R)" : "Right Stick (R)", box_r_x + 32, box_y + 10, color_white, 0);
    snprintf(t, sizeof(t), "X=%5d  Y=%5d", stick_r.x, stick_r.y);
    draw_text(r, font_xs, t, box_r_x + 380, box_y + 12, color_grey, 0);

    snprintf(t, sizeof(t), "Deviation: %.1f%%  (Peak: %.1f%%)", mag_r, stick_r_peak_drift);
    SDL_Color r_stat_col = mag_r < 8.0f ? color_green : (mag_r < 18.0f ? color_yellow : color_red);
    draw_text(r, font_sm, t, box_r_x + 32, box_y + 36, r_stat_col, 0);
    draw_progress_bar(r, box_r_x + 32, box_y + 60, box_w - 64, 8, mag_r > 100.0f ? 1.0f : (mag_r / 100.0f), r_stat_col, color_card);

    const char *r_status = mag_r < 8.0f ? (cur_lang == 1 ? "Etat: Centrage Parfait (Aucun Drift)" : "Status: Perfect Center (No Drift)") :
                          (mag_r < 18.0f ? (cur_lang == 1 ? "Etat: Zone morte normale" : "Status: Minor Deadzone Jitter") :
                                          (cur_lang == 1 ? "Etat: DERIVE DETECTEE (Stick Drift)" : "Status: STICK DRIFT DETECTED"));
    draw_text(r, font_xs, r_status, box_r_x + 32, box_y + 78, r_stat_col, 0);

    // Reset Peak Tracker Button (centered below both stick cards)
    int rst_btn_y = drift_card_y + 186;
    int rst_btn_w = 460;
    int rst_btn_x = (1280 - rst_btn_w) / 2;
    draw_rounded_box(r, rst_btn_x, rst_btn_y, rst_btn_w, 40, 8, color_card_border);
    draw_rounded_rect(r, rst_btn_x, rst_btn_y, rst_btn_w, 40, 8, color_cyan);
    draw_text_w(r, font_sm, cur_lang == 1 ? "[Touch / Appuyer -] Reinitialiser Pics de Derive" : "[Touch / Press -] Reset Drift Peak Values", 640, rst_btn_y + 10, color_white, 1, rst_btn_w - 20);

    SDL_RenderSetClipRect(r, NULL);

    draw_modern_scrollbar(r, 140, 650, ctrl_scroll, ctrl_scroll_max);
}
#undef draw_btn
#undef draw_btn_g
#undef draw_btn_y
#undef draw_btn_r

// Tools
static void draw_pg6(SDL_Renderer *r) {
    char t[256];

    // Content extends past visible area → clear scrolling needed
    int content_bottom = 2360;
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_h = content_bottom - view_top;
    tools_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (tools_scroll > tools_scroll_max) tools_scroll = tools_scroll_max;
    if (tools_scroll < 0) tools_scroll = 0;

    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);

    int sc = tools_scroll;

    draw_card(r, 40, 140 - sc, 1200, 190, s_br_ctrl[cur_lang]);
    draw_rounded_box(r, 40, 148 - sc, 5, 174, 2, color_yellow);
    float br = 0;
    if (R_SUCCEEDED(brightness_read(&br))) {
        if (ctrl_brightness < 0) ctrl_brightness = br;
        snprintf(t, sizeof(t), s_current_br_fmt[cur_lang], ctrl_brightness * 100);
        draw_text(r, font_sm, t, 70, 200 - sc, color_yellow, 0);

        int sx = 70, sy = 235 - sc, sw = 1140, sh = 10;
        draw_rounded_box(r, sx, sy, sw, sh, 5, color_dark_grey);
        int hx = sx + (int)(ctrl_brightness * sw);
        draw_rounded_box(r, sx, sy, hx - sx, sh, 5, color_yellow);
        filledCircleRGBA(r, hx, sy + sh/2, 16, color_white.r, color_white.g, color_white.b, 255);
        circleRGBA(r, hx, sy + sh/2, 16, color_yellow.r, color_yellow.g, color_yellow.b, 255);

        bool auto_br_enabled = false;
        if (R_SUCCEEDED(lblInitialize())) {
            lblIsAutoBrightnessControlEnabled(&auto_br_enabled);
            lblExit();
        }
        draw_text(r, font_sm, s_br_adjust[cur_lang], 70, 275 - sc, color_grey, 0);
        draw_rounded_box(r, 930, 274 - sc, 280, 38, 7,
            auto_br_enabled ? color_green : color_card_border);
        draw_text_w(r, font_sm, auto_br_enabled ? s_auto_on_off[cur_lang] :
            s_auto_off_on[cur_lang], 1070, 284 - sc, color_white, 1, 260);
    } else {
        draw_text(r, font_sm, s_br_na[cur_lang], 70, 200 - sc, color_grey, 0);
    }

    draw_card(r, 40, 370 - sc, 580, 200, s_haptic_test[cur_lang]);
    draw_rounded_box(r, 40, 378 - sc, 5, 184, 2, color_purple);
    draw_text(r, font_sm, s_test_haptic[cur_lang], 70, 430 - sc, color_grey, 0);
    draw_rounded_box(r, 70, 460 - sc, 220, 40, 6, color_card_border);
    draw_text_w(r, font_sm, s_left_rumble[cur_lang], 180, 470 - sc, color_cyan, 1, 204);
    draw_rounded_box(r, 310, 460 - sc, 220, 40, 6, color_card_border);
    draw_text_w(r, font_sm, s_right_rumble[cur_lang], 420, 470 - sc, color_cyan, 1, 204);

    draw_card(r, 660, 370 - sc, 580, 200, s_report_export[cur_lang]);
    draw_rounded_box(r, 660, 378 - sc, 5, 184, 2, color_orange);
    draw_text(r, font_sm, s_export_desc[cur_lang], 690, 430 - sc, color_grey, 0);
    draw_rounded_box(r, 690, 460 - sc, 240, 40, 6, color_card_border);
    draw_text_w(r, font_sm, s_export_btn[cur_lang], 810, 470 - sc, color_orange, 1, 224);
    draw_text(r, font_sm, s_export_perf_hint[cur_lang], 690, 520 - sc, color_cyan, 0);
    draw_text(r, font_sm, s_export_save[cur_lang], 690, 545 - sc, color_grey, 0);

    if (export_msg[0]) {
        u64 elapsed_ticks = armGetSystemTick() - export_msg_tick;
        u64 elapsed_sec = elapsed_ticks / armGetSystemTickFreq();
        if (elapsed_sec < 5)
            draw_text(r, font_sm, export_msg, 690, 545 - sc, color_green, 0);
        else
            export_msg[0] = 0;
    }

    draw_card(r, 40, 610 - sc, 1200, 160, s_runtime_env[cur_lang]);
    draw_rounded_box(r, 40, 618 - sc, 5, 144, 2, color_green);
    draw_key_value(r, s_app_status[cur_lang], s_running_ok[cur_lang], 70, 665 - sc, color_green);
    draw_key_value(r, s_render[cur_lang], "SDL2 @ 60 FPS", 650, 665 - sc, color_white);
    int dock = appletGetOperationMode();
    draw_key_value(r, s_mode[cur_lang], dock ? s_docked[cur_lang] : s_handheld[cur_lang], 70, 693 - sc, dock ? color_green : color_cyan);
    draw_key_value(r, s_env_label[cur_lang], lbl_emulator ? s_emulator[cur_lang] : s_retail[cur_lang], 650, 693 - sc, lbl_emulator ? color_yellow : color_green);

    draw_rounded_box(r, 70, 725 - sc, 220, 32, 6, color_card_border);
    draw_text_w(r, font_sm, s_dead_pixel[cur_lang], 180, 731 - sc, color_red, 1, 204);

    {
        int fs_cx = 40, fs_cy = 790 - sc, fs_cw = 580, fs_ch = 180;
        draw_card(r, fs_cx, fs_cy, fs_cw, fs_ch, s_fan_ctrl[cur_lang]);
        draw_rounded_box(r, fs_cx, fs_cy + 8, 5, fs_ch - 16, 2, color_orange);
        draw_text_w(r, font_sm, s_fan_dev[cur_lang], 70, fs_cy + 58, color_yellow, 0, 520);
        draw_text_w(r, font_xs, s_fan_req[cur_lang], 70, fs_cy + 92, color_grey, 0, 520);
        if (fan_level_reported) {
            snprintf(t, sizeof(t), s_fan_feedback_fmt[cur_lang], fan_reported_pct);
            draw_text_w(r, font_xs, t, 70, fs_cy + 122, color_cyan, 0, 520);
        } else {
            draw_text_w(r, font_xs, s_fan_feedback_na[cur_lang], 70,
                fs_cy + 122, color_grey, 0, 520);
        }
    }

    draw_card(r, 660, 790 - sc, 580, 180, s_app_mode[cur_lang]);
    draw_rounded_box(r, 660, 798 - sc, 5, 164, 2, color_cyan);
    {
        int mod_y = 850 - sc;
        int cur_mode = app_mode;
        for (int i = 0; i < 3; i++) {
            int bx = 690 + i * 180;
            SDL_Color mod_col = (i == cur_mode) ? color_cyan : color_grey;
            SDL_Color mod_bg = (i == cur_mode) ? color_dark_grey : color_card_border;
            draw_rounded_box(r, bx, mod_y, 170, 30, 6, mod_bg);
            draw_text_w(r, font_sm, s_modes[cur_lang][i], bx + 85, mod_y + 4,
                mod_col, 1, 154);
        }
        draw_text_w(r, font_xs, s_mode_switch[cur_lang], 690, mod_y + 40, color_grey, 0, 530);
        if (overlay_status == OVERLAY_STATUS_UNKNOWN)
            overlay_status = detect_ultrahand_overlay();
        draw_text_w(r, font_xs, overlay_status_text(overlay_status), 690,
            mod_y + 65, overlay_status == OVERLAY_STATUS_DETECTED ?
            color_green : color_yellow, 0, 530);
        draw_text_w(r, font_xs, s_sysmodule_desc[cur_lang], 690, mod_y + 88, color_grey, 0, 530);
    }
    draw_rounded_box(r, 690, 974 - sc, 520, 32, 7, color_dark_grey);
    draw_rounded_rect(r, 690, 974 - sc, 520, 32, 7, color_cyan);
    draw_text_w(r, font_sm, s_tesla_guide_button[cur_lang], 950, 980 - sc, color_cyan, 1, 500);

    // CPU/GPU Frequency Scaling card (left)
    draw_card(r, 40, 1010 - sc, 580, 150, s_scale_title[cur_lang]);
    draw_rounded_box(r, 40, 1018 - sc, 5, 134, 2, color_yellow);
    {
        u32 cur_cpu=0, cur_gpu=0, cur_mem=0;
        bool scale_ok = read_clock_rate(PcvModuleId_CpuBus, &cur_cpu) &&
            read_clock_rate(PcvModuleId_GPU, &cur_gpu) &&
            read_clock_rate(PcvModuleId_EMC, &cur_mem);
        if (scale_ok) {
            const char *cpu_mode = cpu_scale_target_mhz == 0 ? s_scale_auto[cur_lang] : "Manual";
            const char *gpu_mode = gpu_scale_target_mhz == 0 ? s_scale_auto[cur_lang] : "Manual";
            snprintf(t, sizeof(t), s_scale_cpu_fmt[cur_lang], cur_cpu/1000000, cpu_mode);
            draw_text_w(r, font_xs, t, 70, 1058 - sc, color_cyan, 0, 310);
            snprintf(t, sizeof(t), s_scale_gpu_fmt[cur_lang], cur_gpu/1000000, gpu_mode);
            draw_text_w(r, font_xs, t, 70, 1082 - sc, color_yellow, 0, 310);
            snprintf(t, sizeof(t), "MEM Current: %u MHz", cur_mem/1000000);
            draw_text(r, font_xs, t, 70, 1106 - sc, color_green, 0);

            // Slider for CPU target
            int sl_x = 390, sl_y = 1065 - sc, sl_w = 200;
            draw_rounded_box(r, sl_x, sl_y, sl_w, 5, 2, color_dark_grey);
            int cpu_display_mhz = cpu_scale_target_mhz > 0 ? cpu_scale_target_mhz : (int)(cur_cpu / 1000000);
            int cpu_pct = (cpu_display_mhz - 102) * 100 / (1785 - 102);
            if (cpu_pct < 0) cpu_pct = 0;
            if (cpu_pct > 100) cpu_pct = 100;
            draw_rounded_box(r, sl_x, sl_y, cpu_pct * sl_w / 100, 5, 2, color_cyan);

            // Slider for GPU target
            sl_y = 1093 - sc;
            draw_rounded_box(r, sl_x, sl_y, sl_w, 5, 2, color_dark_grey);
            int gpu_display_mhz = gpu_scale_target_mhz > 0 ? gpu_scale_target_mhz : (int)(cur_gpu / 1000000);
            int gpu_pct = (gpu_display_mhz - 76) * 100 / (921 - 76);
            if (gpu_pct < 0) gpu_pct = 0;
            if (gpu_pct > 100) gpu_pct = 100;
            draw_rounded_box(r, sl_x, sl_y, gpu_pct * sl_w / 100, 5, 2, color_yellow);

            draw_text_w(r, font_xs, s_scale_set[cur_lang], 70, 1136 - sc, color_grey, 0, 520);
        } else {
            snprintf(t, sizeof(t), "Clock service unavailable (%08X)", (unsigned)clock_last_error);
            draw_text_w(r, font_sm, t, 70, 1060 - sc, color_red, 0, 520);
        }
    }

    // Installed titles list beneath the clock scaling controls.
    {
        char games_title[128];
        snprintf(games_title, sizeof(games_title), s_installed_games[cur_lang], installed_game_count);
        const int game_card_x = 40, game_card_y = 1180, game_card_w = 1200, game_card_h = 320;
        const int game_rows_y = game_card_y + 45;
        const int game_track_h = VISIBLE_INSTALLED_GAMES * INSTALLED_GAME_ROW_HEIGHT;
        draw_card(r, game_card_x, game_card_y - sc, game_card_w, game_card_h, games_title);
        draw_rounded_box(r, game_card_x, game_card_y + 8 - sc, 5, game_card_h - 16, 2, color_purple);
        int max_scroll = installed_game_count > VISIBLE_INSTALLED_GAMES
            ? installed_game_count - VISIBLE_INSTALLED_GAMES : 0;
        if (tools_game_scroll > max_scroll) tools_game_scroll = max_scroll;
        if (tools_game_scroll < 0) tools_game_scroll = 0;
        if (installed_game_count == 0) {
            draw_text_w(r, font_sm, s_no_installed_games[cur_lang],
                70, game_rows_y + 8 - sc, color_grey, 0, 1120);
        } else {
            int first = tools_game_scroll;
            int last = first + VISIBLE_INSTALLED_GAMES;
            if (last > installed_game_count) last = installed_game_count;
            int row_y = game_rows_y - sc;
            for (int i = first; i < last; ++i) {
                int icon_row = i - first;
                SDL_Texture *icon = get_installed_game_icon(r, icon_row, installed_game_ids[i]);
                if (icon) {
                    SDL_Rect icon_dst = {70, row_y - 2, 26, 26};
                    SDL_RenderCopy(r, icon, NULL, &icon_dst);
                }
                draw_text_w(r, font_sm, installed_game_names[i],
                    106, row_y + 2, color_white, 0, 1070);
                row_y += INSTALLED_GAME_ROW_HEIGHT;
            }
            if (max_scroll > 0) {
                const int track_y = game_rows_y - sc, track_h = game_track_h;
                int thumb_h = track_h * VISIBLE_INSTALLED_GAMES / installed_game_count;
                if (thumb_h < 30) thumb_h = 30;
                int thumb_y = track_y +
                    (tools_game_scroll * (track_h - thumb_h)) / max_scroll;
                draw_rounded_box(r, 1205, track_y, 6, track_h, 3, color_dark_grey);
                draw_rounded_box(r, 1205, thumb_y, 6, thumb_h, 3, color_cyan);
            }
        }
    }

    // Running Processes card
    scan_running_processes();
    draw_card(r, 40, 1530 - sc, 580, 210, s_running_games[cur_lang]);
    if (proc_count > 0) {
        int rp_y = 1585 - sc;
        for (int i = 0; i < proc_count && i < 4; i++) {
            SDL_Texture *icon = get_process_icon(r, i, proc_ids[i]);
            if (icon) {
                SDL_Rect dst = {70, rp_y - 2, 30, 30};
                SDL_RenderCopy(r, icon, NULL, &dst);
            } else {
                draw_rounded_box(r, 70, rp_y - 1, 28, 28, 5, color_card_border);
                draw_text(r, font_sm, "G", 84, rp_y + 3, color_cyan, 1);
            }
            draw_text_w(r, font_sm, proc_names[i], 112, rp_y + 3, color_cyan, 0, 490);
            rp_y += 34;
        }
        if (proc_count > 4) {
            snprintf(t, sizeof(t), "... and %d more", proc_count - 4);
            draw_text(r, font_xs, t, 112, rp_y, color_grey, 0);
        }
    } else if (proc_count == 0) {
        draw_text_w(r, font_sm, s_no_running_games[cur_lang], 70, 1595 - sc, color_grey, 0, 520);
    } else {
        draw_text(r, font_sm, "Process list unavailable", 70, 1595 - sc, color_red, 0);
    }

    // Game Usage Stats card
    {
        // Count valid entries (non-zero launches or known name)
        int valid_count = 0;
        for (int i = 0; i < game_stats_count; i++) {
            if (game_stats_launches[i] > 0 && game_stats_names[i][0] != '\0')
                valid_count++;
        }
        draw_card(r, 660, 1530 - sc, 580, 210, "Game Launch Stats");
        if (valid_count > 0) {
            int gs_y = 1590 - sc;
            int shown = 0;
            for (int i = 0; i < game_stats_count && shown < 4; i++) {
                if (game_stats_launches[i] == 0 || game_stats_names[i][0] == '\0') continue;
                char gs_name[32];
                strncpy(gs_name, game_stats_names[i], 28);
                gs_name[28] = 0;
                bool looks_like_id = (strlen(game_stats_names[i]) == 16);
                for (int ci = 0; looks_like_id && ci < 16; ++ci) {
                    char ch = game_stats_names[i][ci];
                    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')))
                        looks_like_id = false;
                }
                if (looks_like_id) continue;
                if (strlen(game_stats_names[i]) > 28) strcat(gs_name, "...");
                snprintf(t, sizeof(t), "%s", gs_name);
                draw_text_w(r, font_sm, t, 690, gs_y, color_white, 0, 480);
                snprintf(t, sizeof(t), "%d plays", game_stats_launches[i]);
                draw_text_w(r, font_xs, t, 690, gs_y + 18, color_grey, 0, 480);
                gs_y += 44;
                shown++;
            }
            if (shown == 0)
                draw_text_w(r, font_sm, s_no_running_games[cur_lang], 690, 1590 - sc, color_grey, 0, 520);
        } else {
            draw_text_w(r, font_sm, "No launch data yet", 690, 1590 - sc, color_grey, 0, 520);
        }
    }

    draw_card(r, 40, 1760 - sc, 580, 200, s_console_info[cur_lang]);
    {
        SetSysSerialNumber sn;
        if (R_SUCCEEDED(setsysGetSerialNumber(&sn)) && serial_looks_retail(sn.number))
            draw_key_value(r, s_serial[cur_lang], sn.number, 70, 1820 - sc, color_cyan);
        else
            draw_key_value(r, s_serial[cur_lang], s_serial_emu[cur_lang], 70, 1820 - sc, color_grey);

        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            snprintf(t, sizeof(t), "%u.%u.%u", fw.major, fw.minor, fw.micro);
            draw_key_value(r, s_firmware[cur_lang], t, 70, 1850 - sc, color_yellow);
        }

        SetSysDeviceNickName nick;
        if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0])
            draw_key_value(r, s_nickname[cur_lang], nick.nickname, 70, 1880 - sc, color_white);
    }

    draw_card(r, 660, 1760 - sc, 580, 280, s_sd_info_speed[cur_lang]);
    {
        FsFileSystem sd;
        if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
            s64 free = 0, total = 0;
            if (R_SUCCEEDED(fsFsGetFreeSpace(&sd, "/", &free)) &&
                R_SUCCEEDED(fsFsGetTotalSpace(&sd, "/", &total)) && total > 0) {
                float used_pct = (float)(total - free) / total;
                snprintf(t, sizeof(t), "%.1f GB", total / 1.0e9);
                draw_key_value(r, s_total_capacity[cur_lang], t, 690, 1820 - sc, color_white);
                snprintf(t, sizeof(t), "%.1f GB", free / 1.0e9);
                draw_key_value(r, s_free_space[cur_lang], t, 690, 1850 - sc, color_green);
                draw_progress_bar(r, 690, 1880 - sc, 510, 16, used_pct, color_cyan, color_dark_grey);
                snprintf(t, sizeof(t), s_pct_used_fmt[cur_lang], used_pct * 100);
                draw_text(r, font_sm, t, 690, 1910 - sc, color_grey, 0);
            }
            draw_rounded_box(r, 690, 1940 - sc, 240, 36, 6, color_card_border);
            draw_text_w(r, font_sm, s_read_speed_btn[cur_lang], 810, 1948 - sc, color_cyan, 1, 224);
            if (sd_speed_result > 0) {
                snprintf(t, sizeof(t), s_read_speed_fmt[cur_lang], sd_speed_result);
                draw_text(r, font_sm, t, 960, 1948 - sc, color_green, 0);
            } else if (sd_speed_result < 0) {
                if (sd_speed_result == -2)
                    draw_text(r, font_sm, s_testing[cur_lang], 960, 1948 - sc, color_yellow, 0);
                else
                    draw_text(r, font_sm, s_error[cur_lang], 960, 1948 - sc, color_red, 0);
            } else {
                draw_text(r, font_sm, s_not_tested[cur_lang], 960, 1948 - sc, color_grey, 0);
            }
            fsFsClose(&sd);
        } else {
            draw_text(r, font_sm, s_sd_info_na[cur_lang], 690, 1830 - sc, color_grey, 0);
        }
    }

    // User save data: enumerate account saves, identify them by title ID and
    // keep reversible dated snapshots on the SD card.
    if (!save_manager_loaded) {
        Result rc = save_manager_refresh();
        if (R_FAILED(rc)) {
            snprintf(popup_text, sizeof(popup_text), "Save list unavailable: %08X", (unsigned)rc);
            popup_type = POPUP_INFO;
            popup_active = true;
            popup_start_tick = armGetSystemTick();
        }
        save_manager_loaded = 1;
    }
    int save_count = save_manager_count();
    if (save_count && save_manager_selected >= save_count) save_manager_selected = save_count - 1;
    if (save_count && save_backup_list_save != save_manager_selected) {
        Result rc = save_manager_refresh_backups(save_manager_selected);
        save_backup_list_save = save_manager_selected;
        save_backup_selected = 0;
        if (R_FAILED(rc)) {
            snprintf(popup_text, sizeof(popup_text), "Backup history unavailable: %08X", (unsigned)rc);
            popup_type = POPUP_INFO;
            popup_active = true;
            popup_start_tick = armGetSystemTick();
        }
    }
    int backup_count = save_count ? save_manager_backup_count(save_manager_selected) : 0;
    if (backup_count && save_backup_selected >= backup_count)
        save_backup_selected = backup_count - 1;
    draw_card(r, 40, 2060 - sc, 1200, 270, s_save_manager_title[cur_lang]);
    draw_text_w(r, font_xs, s_save_manager_hint[cur_lang], 70, 2110 - sc, color_grey, 0, 1140);
    if (save_count > 0) {
        u64 title_id = 0; AccountUid uid = {{0}};
        if (R_SUCCEEDED(save_manager_get(save_manager_selected, &title_id, &uid))) {
            snprintf(t, sizeof(t), "%s   |   %s   |   Save %d/%d", get_save_title_label(title_id),
                get_save_profile_name(uid), save_manager_selected + 1, save_count);
            draw_text_w(r, font_sm, t, 70, 2160 - sc, color_cyan, 0, 1140);
            if (backup_count > 0) {
                u64 timestamp = 0;
                if (R_SUCCEEDED(save_manager_get_backup_timestamp(save_manager_selected, save_backup_selected, &timestamp))) {
                    time_t backup_time = (time_t)timestamp;
                    struct tm *backup_tm = localtime(&backup_time);
                    char date[32];
                    if (!backup_tm || !strftime(date, sizeof(date), "%Y-%m-%d %H:%M", backup_tm))
                        snprintf(date, sizeof(date), "Unix %llu", (unsigned long long)timestamp);
                    snprintf(t, sizeof(t), "%s %d/%d: %s", s_save_backup_history[cur_lang],
                        save_backup_selected + 1, backup_count, date);
                    draw_text_w(r, font_xs, t, 70, 2182 - sc, color_grey, 0, 800);
                }
            } else {
                draw_text_w(r, font_xs, s_save_no_backups[cur_lang], 70, 2182 - sc, color_grey, 0, 800);
            }
            draw_rounded_box(r, 70, 2210 - sc, 100, 42, 6, color_card_border);
            draw_text(r, font_sm, "<", 120, 2218 - sc, color_white, 1);
            draw_rounded_box(r, 180, 2210 - sc, 100, 42, 6, color_card_border);
            draw_text(r, font_sm, ">", 230, 2218 - sc, color_white, 1);
            draw_rounded_box(r, 310, 2210 - sc, 250, 42, 6, color_card_border);
            draw_text_w(r, font_xs, s_save_backup_btn[cur_lang], 435, 2221 - sc, color_green, 1, 230);
            bool confirm_live = save_restore_confirm && armGetSystemTick() - save_restore_confirm_tick < armGetSystemTickFreq() * 8;
            if (save_restore_confirm && !confirm_live) save_restore_confirm = false;
            draw_rounded_box(r, 580, 2210 - sc, 300, 42, 6,
                confirm_live ? color_red : (backup_count ? color_card_border : color_dark_grey));
            draw_text_w(r, font_xs, confirm_live ? s_save_restore_confirm_btn[cur_lang] : s_save_restore_btn[cur_lang], 730, 2221 - sc, color_white, 1, 280);
            draw_rounded_box(r, 900, 2180 - sc, 210, 42, 6,
                backup_count ? color_card_border : color_dark_grey);
            if (backup_count > 0) {
                char selector[64];
                snprintf(selector, sizeof(selector), s_save_backup_selector[cur_lang],
                    save_backup_selected + 1, backup_count);
                draw_text_w(r, font_xs, selector, 1005, 2191 - sc,
                    color_cyan, 1, 196);
            } else {
                draw_text_w(r, font_xs, s_save_no_backups[cur_lang], 1005,
                    2191 - sc, color_grey, 1, 196);
            }
            if (save_backup_result != 0) {
                draw_text_w(r, font_xs, save_backup_result > 0 ?
                    s_save_backup_success[cur_lang] : s_save_backup_failure[cur_lang],
                    435, 2228 - sc, save_backup_result > 0 ? color_green : color_red,
                    1, 360);
            }
        }
    } else {
        draw_text_w(r, font_sm, "No accessible user saves. A full title-mode homebrew launch may be required.", 70, 2130 - sc, color_yellow, 0, 1140);
    }

    SDL_RenderSetClipRect(r, NULL);

    draw_modern_scrollbar(r, view_top, view_bottom, tools_scroll, tools_scroll_max);
}

// About
static const char *about_features[LANG_MAX][7] = {
    {"System - Firmware, hardware and battery", "Storage - SD information and file browser", "Network - Wi-Fi, speed test and live traffic", "Transfer - FTP and USB MTP", "Performance - Clocks, RAM and temperatures", "Controller - Buttons, sticks and motion sensors", "Tools - Brightness, vibration and diagnostics"},
    {"Systeme - Micrologiciel, materiel et batterie", "Stockage - Carte SD et explorateur de fichiers", "Reseau - Wi-Fi, debit et trafic en direct", "Transfert - FTP et MTP USB", "Performances - Frequences, RAM et temperatures", "Manette - Boutons, sticks et capteurs de mouvement", "Outils - Luminosite, vibrations et diagnostics"},
    {"Sistema - Firmware, hardware e batteria", "Archiviazione - Scheda SD e file manager", "Rete - Wi-Fi, velocita e traffico in tempo reale", "Trasferimento - FTP e MTP USB", "Prestazioni - Frequenze, RAM e temperature", "Controller - Tasti, stick e sensori di movimento", "Strumenti - Luminosita, vibrazione e diagnostica"},
    {"Sistema - Firmware, hardware y bateria", "Almacenamiento - Tarjeta SD y explorador de archivos", "Red - Wi-Fi, velocidad y trafico en tiempo real", "Transferencia - FTP y MTP por USB", "Rendimiento - Frecuencias, RAM y temperaturas", "Mando - Botones, sticks y sensores de movimiento", "Herramientas - Brillo, vibracion y diagnostico"},
    {"System - Firmware, Hardware und Akku", "Speicher - SD-Karte und Dateimanager", "Netzwerk - WLAN, Tempo und Live-Verkehr", "Transfer - FTP und USB-MTP", "Leistung - Takt, RAM und Temperaturen", "Controller - Tasten, Sticks und Bewegungssensoren", "Werkzeuge - Helligkeit, Vibration und Diagnose"},
    {"Sistema - Firmware, hardware e bateria", "Armazenamento - Cartao SD e gestor de ficheiros", "Rede - Wi-Fi, velocidade e trafego em tempo real", "Transferencia - FTP e MTP por USB", "Desempenho - Frequencias, RAM e temperaturas", "Controle - Botoes, sticks e sensores de movimento", "Ferramentas - Brilho, vibracao e diagnostico"},
    {"Systeem - Firmware, hardware en batterij", "Opslag - SD-kaart en bestandsbeheer", "Netwerk - Wi-Fi, snelheid en live verkeer", "Overdracht - FTP en USB-MTP", "Prestaties - Klokken, RAM en temperaturen", "Controller - Knoppen, sticks en bewegingssensoren", "Hulpmiddelen - Helderheid, trilling en diagnose"},
    {"システム - ファームウェア、機種、バッテリー", "ストレージ - SD情報とファイル管理", "ネットワーク - Wi-Fi、速度、リアルタイム通信量", "転送 - FTPとUSB MTP", "性能 - クロック、RAM、温度", "コントローラー - ボタン、スティック、モーションセンサー", "ツール - 明るさ、振動、診断"},
    {"Система - прошивка, модель и аккумулятор", "Хранилище - SD-карта и файловый менеджер", "Сеть - Wi-Fi, скорость и трафик в реальном времени", "Передача - FTP и USB MTP", "Производительность - частоты, RAM и температура", "Контроллер - кнопки, стики и датчики движения", "Инструменты - яркость, вибрация и диагностика"},
    {"系统 - 固件、硬件型号与电池", "存储 - SD 卡信息与文件管理", "网络 - Wi-Fi、测速和实时流量", "传输 - FTP 与 USB MTP", "性能 - 时钟、内存与温度", "控制器 - 按键、摇杆与动作传感器", "工具 - 亮度、振动与诊断"},
    {"시스템 - 펌웨어, 기기 정보 및 배터리", "저장소 - SD 정보 및 파일 관리자", "네트워크 - Wi-Fi, 속도 및 실시간 트래픽", "전송 - FTP 및 USB MTP", "성능 - 클럭, RAM 및 온도", "컨트롤러 - 버튼, 스틱 및 모션 센서", "도구 - 밝기, 진동 및 진단"}
};

static const char *about_changelog[LANG_MAX][5] = {
    {"Redesigned Stick Drift Analyzer with live deflection bars & peak hold", "Clean top bar without IP leak, version badge & session uptime", "Smooth tab navigation with ZL/ZR triggers & touchable shoulder buttons", "Console-style key badges in footer & sys-clk OC detection", "Full vertical scrolling on Transfer page & fixed EmuMMC text framing"},
    {"Analyseur de drift repense avec jauges en direct et memoire de pic", "Barre superieure epuree sans fuite IP, badge v0.6 et uptime de session", "Navigation fluide avec gachettes ZL/ZR et boutons d'epaule L/R tactiles", "Pied de page console avec badges manette et detection sys-clk", "Defilement vertical complet dans Transfert et cadrage EmuMMC corrige"},
    {"Analizzatore di drift ridisegnato con barre in tempo reale e picco", "Barra superiore pulita senza perdita IP, badge v0.6 e uptime", "Navigazione schede fluida con ZL/ZR e pulsanti dorsali L/R tattili", "Piede pagina stile console con badge tasti e rilevamento sys-clk", "Scorrimento completo pagina Trasferimento e testo EmuMMC corretto"},
    {"Analizador de drift redisenado con barras en vivo y retencion de pico", "Barra superior limpia sin fuga de IP, badge v0.6 y uptime", "Navegacion fluida con gatillos ZL/ZR y botones L/R tactiles", "Pie de pagina estilo consola con botones y deteccion de sys-clk", "Desplazamiento completo en Transferencia y texto EmuMMC corregido"},
    {"Drift-Analyzer ueberarbeitet mit Live-Balken und Spitzenwert", "Saubere Kopfzeile ohne IP-Leck, Versionsbadge und Betriebszeit", "Reibungslose Tab-Navigation mit ZL/ZR und tastbaren L/R-Schultertasten", "Konsolen-Footer mit Tasten-Badges und sys-clk-Erkennung", "Vollstaendiges Scrollen in Transfer und korrigiertes EmuMMC-Layout"},
    {"Analisador de drift redesenhado com barras em tempo real e pico", "Barra superior limpa sem fuga de IP, badge v0.6 e tempo de atividade", "Navegacao fluida com gatilhos ZL/ZR e botoes L/R tateis", "Rodape estilo consola com botoes e detecao sys-clk", "Rolagem completa na pagina Transferencia e texto EmuMMC corrigido"},
    {"Drift-analyzer vernieuwd met live balken en piekhouder", "Schone bovenbalk zonder IP-lek, v0.6-badge en sessietijd", "Vloeiende tabnavigatie met ZL/ZR en aanraakbare L/R-schouderknoppen", "Console-stijl voettekst met toetsbadges en sys-clk-detectie", "Volledig scrollen op pagina Overdracht en gecorrigeerde EmuMMC-tekst"},
    {"ライブ偏向バーとピーク保持を備えたドリフトアナライザーの刷新", "IP非表示で安全な上部バー、v0.6バッジ、セッション稼働時間", "ZL/ZRトリガーとタッチ対応L/Rボタンによるスムーズなタブ移動", "コントローラー風フッターバッジとsys-clkオーバークロック検出", "転送画面の完全スクロール対応とEmuMMCテキスト枠の最適化"},
    {"Обновленный анализатор дрифта с живыми шкалами и фиксацией пика", "Чистая верхняя панель без утечки IP, бейдж v0.6 и аптайм", "Плавная навигация по вкладкам с ZL/ZR и сенсорными L/R", "Консольный футер с кнопками геймпада и обнаружение sys-clk", "Полная прокрутка на странице передачи и исправленный текст EmuMMC"},
    {"重构的摇杆漂移分析器（带实时偏移条和峰值保持）", "无 IP 泄露的顶部栏、v0.6 徽章和运行时间显示", "支持 ZL/ZR 扳机键和触控肩键的流畅标签页导航", "主机风格底部按键气泡和 sys-clk 超频检测", "传输页面完整向下滚动支持及 EmuMMC 文本边框修复"},
    {"실시간 편차 바와 피크 유지를 갖춘 스틱 드리프트 분석기 개편", "IP 노출 없는 깔끔한 상단 바, v0.6 배지 및 가동 시간 표시", "ZL/ZR 트리거 및 터치 L/R 숄더 버튼을 통한 부드러운 탭 이동", "콘솔 스타일 하단 키 배지 및 sys-clk 오버클럭 감지", "전송 페이지 전체 스크롤 지원 및 EmuMMC 텍스트 프레임 수정"}
};

static const char *about_roadmap[LANG_MAX][7] = {
    {"USB Ethernet adapter diagnostics", "Bluetooth device scan and diagnostics", "Save manager improvements", "Safe fan curve integration", "Theme import and export", "Sysmodule support (in development)", "Transfer history and queue"},
    {"Diagnostic des adaptateurs Ethernet USB", "Recherche et diagnostic des appareils Bluetooth", "Ameliorations du gestionnaire de sauvegardes", "Courbe ventilateur securisee", "Import et export de themes", "Support sysmodule (en developpement)", "Historique et file de transferts"},
    {"Diagnostica adattatori Ethernet USB", "Ricerca e diagnostica dispositivi Bluetooth", "Migliorie gestione salvataggi", "Curva ventola sicura", "Importazione ed esportazione temi", "Supporto sysmodule (in sviluppo)", "Cronologia e coda trasferimenti"},
    {"Diagnostico de adaptadores Ethernet USB", "Busqueda y diagnostico de dispositivos Bluetooth", "Mejoras del gestor de partidas", "Curva segura del ventilador", "Importar y exportar temas", "Soporte de sysmodule (en desarrollo)", "Historial y cola de transferencias"},
    {"Diagnose von USB-Ethernet-Adaptern", "Bluetooth-Gerate suchen und diagnostizieren", "Verbesserungen der Spielstandverwaltung", "Sichere Lufterkurve", "Themes importieren und exportieren", "Sysmodule-Unterstutzung (in Entwicklung)", "Transferverlauf und Warteschlange"},
    {"Diagnostico de adaptadores Ethernet USB", "Pesquisa e diagnostico de dispositivos Bluetooth", "Melhorias no gestor de saves", "Curva segura da ventoinha", "Importar e exportar temas", "Suporte a sysmodule (em desenvolvimento)", "Historico e fila de transferencias"},
    {"Diagnose van USB-Ethernetadapters", "Bluetoothapparaten zoeken en diagnosticeren", "Verbeteringen voor spelopslagbeheer", "Veilige ventilatorcurve", "Thema's importeren en exporteren", "Sysmodule-ondersteuning (in ontwikkeling)", "Overdrachtgeschiedenis en wachtrij"},
    {"USB Ethernetアダプター診断", "Bluetooth機器の検索と診断", "セーブ管理の改善", "安全なファンカーブ", "テーマのインポートとエクスポート", "Sysmodule対応（開発中）", "転送履歴とキュー"},
    {"Диагностика USB Ethernet-адаптеров", "Поиск и диагностика Bluetooth-устройств", "Улучшение менеджера сохранений", "Безопасная кривая вентилятора", "Импорт и экспорт тем", "Поддержка sysmodule (в разработке)", "История и очередь передачи"},
    {"USB 以太网适配器诊断", "蓝牙设备扫描与诊断", "改进游戏存档管理", "安全的风扇曲线", "主题导入与导出", "Sysmodule 支持（开发中）", "传输历史与队列"},
    {"USB 이더넷 어댑터 진단", "Bluetooth 기기 검색 및 진단", "게임 저장 관리 개선", "안전한 팬 곡선", "테마 가져오기 및 내보내기", "Sysmodule 지원(개발 중)", "전송 기록 및 대기열"}
};

static const char *about_libraries[LANG_MAX] = {
    "Libraries: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "Bibliotheques : libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "Librerie: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "Bibliotecas: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "Bibliotheken: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "Bibliotecas: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "Bibliotheken: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "使用ライブラリ: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "Библиотеки: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "使用库：libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType",
    "라이브러리: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType"
};

static const char *about_thanks[LANG_MAX] = {
    "Thanks to SwitchBrew, devkitPro, and the homebrew community.",
    "Merci a SwitchBrew, devkitPro et a la communaute homebrew.",
    "Grazie a SwitchBrew, devkitPro e alla comunita homebrew.",
    "Gracias a SwitchBrew, devkitPro y a la comunidad homebrew.",
    "Danke an SwitchBrew, devkitPro und die Homebrew-Community.",
    "Agradecimentos a SwitchBrew, devkitPro e a comunidade homebrew.",
    "Dank aan SwitchBrew, devkitPro en de homebrewgemeenschap.",
    "SwitchBrew、devkitPro、homebrewコミュニティに感謝します。",
    "Спасибо SwitchBrew, devkitPro и сообществу homebrew.",
    "感谢 SwitchBrew、devkitPro 和自制软件社区。",
    "SwitchBrew, devkitPro 및 홈브루 커뮤니티에 감사드립니다."
};

static const char *about_changelog_title[LANG_MAX] = {
    "v0.6 Changelog", "Nouveautes v0.6", "Novita v0.6", "Novedades v0.6", "Neuerungen v0.6", "Novidades v0.6", "Wijzigingen v0.6",
    "v0.6 更新内容", "Изменения v0.6", "v0.6 更新内容", "v0.6 변경 사항"
};
static const char *about_planned_title[LANG_MAX] = {
    "Planned for later", "Prevu pour plus tard", "Previsto per dopo", "Planeado para mas adelante", "Fuer spater geplant", "Planeado para depois", "Gepland voor later",
    "今後の予定", "Планы на будущее", "后续计划", "추후 계획"
};

static void draw_pg7(SDL_Renderer *r) {
    draw_card(r, 40, 140, 1200, 510, s_about_title[cur_lang]);

    // Calculate total content height (must match actual draw_y progression)
    int content_h = 0;
    content_h += 45;   // title
    content_h += 33;   // version
    content_h += 22;   // desc line 1
    content_h += 22;   // desc line 2
    content_h += 30;   // gap
    content_h += 7 * 25; // features
    content_h += 12;   // gap
    content_h += 22;   // libraries
    content_h += 22;   // thanks
    content_h += 30;   // gap before changelog
    content_h += 30;   // changelog header
    content_h += 5 * 24; // localized changelog summaries
    content_h += 14;   // gap
    content_h += 28;   // roadmap header
    content_h += 7 * 24; // roadmap items
    content_h += 20;   // bottom pad

    int view_h = (510 - 48) - 12; // visible body height after the 12 px top inset
    about_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (about_scroll > about_scroll_max) about_scroll = about_scroll_max;
    if (about_scroll < 0) about_scroll = 0;

    // Clip to card body
    SDL_Rect clip = {40, 188, 1200, 510 - 48};
    SDL_RenderSetClipRect(r, &clip);

    int y = 200 - about_scroll;

    draw_text(r, font_lg, "Switch Info NX", 70, y, color_cyan, 0);
    y += 45;
    draw_text_w(r, font_sm, s_created_by[cur_lang], 70, y, color_green, 0, 1140);
    y += 33;

    draw_text_w(r, font_sm, s_desc_line1[cur_lang], 70, y, color_white, 0, 1140);
    y += 22;
    draw_text_w(r, font_sm, s_desc_line2[cur_lang], 70, y, color_white, 0, 1140);
    y += 30;

    static const SDL_Color section_colors[7] = {
        {0, 210, 255, 255}, {170, 120, 255, 255}, {80, 190, 255, 255},
        {80, 220, 150, 255}, {255, 190, 80, 255}, {255, 100, 130, 255},
        {180, 190, 210, 255}
    };
    for (int i = 0; i < 7; i++) {
        filledCircleRGBA(r, 75, y + 8, 4, section_colors[i].r, section_colors[i].g,
            section_colors[i].b, 230);
        draw_text_w(r, font_sm, about_features[cur_lang][i], 90, y, color_grey, 0, 1120);
        y += 25;
    }

    y += 12;
    draw_text_w(r, font_sm, about_libraries[cur_lang], 70, y, color_cyan, 0, 1140);
    y += 22;
    draw_text_w(r, font_sm, about_thanks[cur_lang], 70, y, color_grey, 0, 1140);
    y += 30;

    // Changelog
    draw_text_w(r, font_md, about_changelog_title[cur_lang], 70, y, color_purple, 0, 1140);
    y += 30;
    for (int i = 0; i < 5; i++) {
        draw_text_w(r, font_sm, about_changelog[cur_lang][i], 90, y, color_grey, 0, 1120);
        y += 24;
    }
    y += 14;
    draw_text_w(r, font_md, about_planned_title[cur_lang], 70, y, color_purple, 0, 1140);
    y += 28;
    for (int i = 0; i < 7; i++) {
        draw_text_w(r, font_sm, about_roadmap[cur_lang][i], 90, y, color_grey, 0, 1120);
        y += 24;
    }
    y += 20;

    SDL_RenderSetClipRect(r, NULL);

    // Scroll indicators
    draw_modern_scrollbar(r, 140, 650, about_scroll, about_scroll_max);
}

// ─── Settings page ─────────────────────────────────────────
static void draw_pg8(SDL_Renderer *r) {
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_bottom = 1000;
    int content_h = content_bottom - view_top;
    settings_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (settings_scroll > settings_scroll_max) settings_scroll = settings_scroll_max;
    if (settings_scroll < 0) settings_scroll = 0;

    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = settings_scroll;

    draw_card(r, 40, 140 - sc, 1200, 900, settings_title[cur_lang]);

    int y = 210 - sc;
    // Language (two rows: 6 + 5)
    draw_text(r, font_md, settings_lang_label[cur_lang], 70, y, color_cyan, 0);
    y += 40;
    int lang_per_row = (LANG_MAX + 1) / 2;
    for (int i = 0; i < LANG_MAX; i++) {
        int row = i / lang_per_row;
        int col = i % lang_per_row;
        int bx = 70 + col * 110;
        int by = y + row * 44;
        SDL_Color col2 = (settings_sel == 0 && i == cur_lang) ? color_green : color_grey;
        draw_rounded_box(r, bx, by - 4, 104, 36, 6, (settings_sel == 0 && i == cur_lang) ? color_dark_grey : color_card_border);
        draw_text_w(r, font_sm, lang_names[i], bx + 52, by + 4, (i == cur_lang) ? color_cyan : col2, 1, 96);
    }
    y += 56 + 44;

    // Theme (two rows of 9 themes for v0.4)
    draw_text(r, font_md, settings_theme_label[cur_lang], 70, y, color_cyan, 0);
    y += 40;
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 9; col++) {
            int i = row * 9 + col;
            if (i >= THEME_MAX) break;
            int bx = 55 + col * 125;
            SDL_Color tc = theme_white[i];
            SDL_Color bc = theme_bg[i];
            draw_rounded_box(r, bx, y - 4, 116, 32, 6, (settings_sel == 1 && i == cur_theme) ? color_dark_grey : color_card_border);
            draw_rounded_box(r, bx + 4, y - 1, 16, 26, 4, bc);
            draw_rounded_rect(r, bx + 4, y - 1, 16, 26, 4, tc);
            draw_text_w(r, font_sm, theme_names[i], bx + 25, y + 3, (i == cur_theme) ? color_cyan : color_grey, 0, 88);
        }
        y += 40;
    }
    y += 16;
    if (cur_theme == THEME_CUSTOM) {
        draw_rounded_box(r, 70, y, 240, 32, 6, color_card_border);
        draw_text_w(r, font_sm, "[Touch] Custom Theme Editor", 190, y + 4, color_cyan, 1, 224);
        y += 40;
    } else {
        y += 8;
    }

    // Auto-refresh interval
    draw_text(r, font_md, settings_refresh_label[cur_lang], 70, y, color_cyan, 0);
    y += 40;
    static const int refresh_vals[] = { 0, 1, 3, 5, 10 };
    int rcnt = sizeof(refresh_vals) / sizeof(refresh_vals[0]);
    int cur_rsel = 0;
    for (int i = 0; i < rcnt; i++) {
        if (refresh_vals[i] == auto_refresh_interval) cur_rsel = i;
        int bx = 70 + i * 112;
        draw_rounded_box(r, bx, y - 4, 100, 36, 6, i == cur_rsel ? color_dark_grey : color_card_border);
        draw_text_w(r, font_sm, s_refresh_names[cur_lang][i], bx + 50, y + 4, i == cur_rsel ? color_cyan : color_grey, 1, 92);
    }
    y += 60;

    // Temp alert threshold
    draw_text(r, font_md, settings_temp_alert_label[cur_lang], 70, y, color_cyan, 0);
    y += 36;
    draw_rounded_box(r, 70, y, 200, 36, 6, (settings_sel == 3) ? color_dark_grey : color_card_border);
    draw_rounded_box(r, 80, y + 3, 34, 30, 5, color_card_border);
    draw_text(r, font_sm, "-", 97, y + 8, color_white, 1);
    draw_temperature(r, font_sm, temp_alert_millic, 170, y + 6, (settings_sel == 3) ? color_cyan : color_grey, 1);
    draw_rounded_box(r, 226, y + 3, 34, 30, 5, color_card_border);
    draw_text(r, font_sm, "+", 243, y + 8, color_white, 1);
    y += 52;

    // Memory alert threshold
    draw_text(r, font_md, settings_mem_alert_label[cur_lang], 70, y, color_cyan, 0);
    y += 36;
    char vmem[64]; snprintf(vmem, sizeof(vmem), "%d%%", (int)(mem_alert_frac * 100.0f));
    draw_rounded_box(r, 70, y, 200, 36, 6, (settings_sel == 4) ? color_dark_grey : color_card_border);
    draw_rounded_box(r, 80, y + 3, 34, 30, 5, color_card_border);
    draw_text(r, font_sm, "-", 97, y + 8, color_white, 1);
    draw_text(r, font_sm, vmem, 170, y + 6, (settings_sel == 4) ? color_cyan : color_grey, 1);
    draw_rounded_box(r, 226, y + 3, 34, 30, 5, color_card_border);
    draw_text(r, font_sm, "+", 243, y + 8, color_white, 1);
    y += 52;

    // Info
    draw_text(r, font_xs, settings_info[cur_lang], 70, y, color_grey, 0);
    draw_text(r, font_sm, s_settings_info2[cur_lang], 70, y + 40, color_grey, 0);
    draw_rounded_box(r, 70, 900 - sc, 390, 40, 8, color_card_border);
    draw_rounded_rect(r, 70, 900 - sc, 390, 40, 8, color_cyan);
    draw_text_w(r, font_sm, s_tutorial_settings[cur_lang], 265, 910 - sc,
        color_white, 1, 370);

    int preview_x = 690, preview_y = 830;
    draw_rounded_box(r, preview_x, preview_y, 510, 100, 8, color_card);
    draw_rounded_rect(r, preview_x, preview_y, 510, 100, 8, color_card_border);
    draw_text_w(r, font_xs, s_theme_preview[cur_lang], preview_x + 18, preview_y + 10,
        color_cyan, 0, 280);
    draw_text_w(r, font_sm, theme_names[cur_theme], preview_x + 18, preview_y + 35,
        color_white, 0, 180);
    draw_rounded_box(r, preview_x + 205, preview_y + 32, 90, 48, 6, color_bg);
    draw_rounded_rect(r, preview_x + 205, preview_y + 32, 90, 48, 6, color_card_border);
    draw_rounded_box(r, preview_x + 215, preview_y + 41, 70, 30, 4, color_card);
    draw_rounded_rect(r, preview_x + 215, preview_y + 41, 70, 30, 4, color_card_border);
    draw_rounded_box(r, preview_x + 310, preview_y + 43, 42, 16, 5, color_cyan);
    draw_rounded_box(r, preview_x + 360, preview_y + 43, 42, 16, 5, color_green);
    draw_text_w(r, font_xs, "UI colors", preview_x + 415, preview_y + 44, color_grey, 0, 80);

    SDL_RenderSetClipRect(r, NULL);

    // Scroll indicators
    draw_modern_scrollbar(r, 140, 650, settings_scroll, settings_scroll_max);

    // ── Custom Theme Editor (full-screen window) ──
    if (custom_theme_editing) {
        char cte_t[128];
        // Full background overlay
        SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
        SDL_Rect full = {0, 0, W, H};
        SDL_RenderFillRect(r, &full);

        // Editor window card
        draw_card_shadow(r, 80, 80, 1120, 560);
        draw_rounded_box(r, 80, 80, 1120, 560, 12, color_bg2);
        draw_rounded_rect(r, 80, 80, 1120, 560, 12, color_cyan);

        // Header with close button
        draw_text(r, font_lg, s_cte_title[cur_lang], 640, 96, color_cyan, 1);
        thickLineRGBA(r, 100, 132, 1180, 132, 2, color_card_border.r, color_card_border.g, color_card_border.b, 255);

        // Color slot grid (5 rows x 2 cols)
        SDL_Color *cte_colors[] = {&custom_bg,&custom_card,&custom_border,&custom_cyan,&custom_green,&custom_white,&custom_grey,&custom_dark_grey,&custom_bg2,&custom_bg3};
        int cte_count = sizeof(cte_colors)/sizeof(cte_colors[0]);

        int cx = 110, cy = 177;
        for (int i = 0; i < 3; i++) {
            int px = 110 + i * 365;
            draw_rounded_box(r, px, 138, 340, 30, 7,
                (SDL_Color){color_card.r, color_card.g, color_card.b, 255});
            draw_rounded_rect(r, px, 138, 340, 30, 7,
                i == 0 ? color_cyan : (i == 1 ? color_orange : color_green));
            filledCircleRGBA(r, px + 18, 153, 5,
                i == 0 ? 0 : (i == 1 ? 255 : 92),
                i == 0 ? 195 : (i == 1 ? 125 : 230),
                i == 0 ? 255 : (i == 1 ? 70 : 150), 255);
            draw_text_w(r, font_xs, s_cte_quick[cur_lang][i], px + 34, 146,
                color_white, 0, 280);
        }
        for (int i = 0; i < cte_count; i++) {
            int row = i / 2;
            int col = i % 2;
            int px = cx + col * 420;
            int py = cy + row * 40;
            int sel = (cte_sel == i);
            draw_rounded_box(r, px, py, 400, 36, 6, sel ? (SDL_Color){50,80,160,255} : color_card_border);
            draw_rounded_box(r, px + 4, py + 4, 28, 28, 4, *cte_colors[i]);
            draw_rounded_rect(r, px + 4, py + 4, 28, 28, 4, sel ? color_white : color_grey);
            draw_text_w(r, font_sm, s_cte_names[cur_lang][i], px + 40, py + 6, sel ? color_white : color_grey, 0, 155);
            snprintf(cte_t, sizeof(cte_t), "R:%03d G:%03d B:%03d", cte_colors[i]->r, cte_colors[i]->g, cte_colors[i]->b);
            draw_text(r, font_xs, cte_t, px + 200, py + 10, sel ? color_cyan : color_grey, 0);
        }

        // Channel edit area
        SDL_Color *sc = cte_colors[cte_sel];
        int edit_y = cy + cte_count/2 * 40 + 8;
        draw_text(r, font_sm, s_cte_edit_hint[cur_lang], 640, edit_y, color_cyan, 1);

        const char *chan_names[] = {"R", "G", "B"};
        u8 *chan_vals[] = {&sc->r, &sc->g, &sc->b};
        for (int ch = 0; ch < 3; ch++) {
            int by = edit_y + 24 + ch * 30;
            // Channel label
            draw_rounded_box(r, 180, by, 40, 24, 4, cte_chan == ch ? color_cyan : color_dark_grey);
            draw_text(r, font_sm, chan_names[ch], 200, by + 2, cte_chan == ch ? color_bg : color_grey, 1);
            // Slider
            draw_rounded_box(r, 240, by + 2, 460, 24, 4, color_dark_grey);
            draw_rounded_box(r, 240, by + 2, *chan_vals[ch] * 460 / 255, 24, 4, cte_chan == ch ? color_cyan : color_grey);
            // Value
            snprintf(cte_t, sizeof(cte_t), "%03d", *chan_vals[ch]);
            draw_text(r, font_md, cte_t, 720, by, color_white, 0);
            // Percentage
            snprintf(cte_t, sizeof(cte_t), "(%d%%)", *chan_vals[ch] * 100 / 255);
            draw_text(r, font_sm, cte_t, 770, by + 2, color_grey, 0);
        }

        // Save / Cancel buttons
        int btn_y = edit_y + 24 + 3 * 30 + 12;
        // Save button
        draw_rounded_box(r, 340, btn_y, 200, 44, 8, color_green);
        draw_rounded_rect(r, 340, btn_y, 200, 44, 8, (SDL_Color){0,180,100,255});
        draw_text(r, font_sm, s_cte_save[cur_lang], 440, btn_y + 10, color_bg, 1);
        // Cancel button
        draw_rounded_box(r, 600, btn_y, 200, 44, 8, (SDL_Color){180,30,30,255});
        draw_rounded_rect(r, 600, btn_y, 200, 44, 8, color_red);
        draw_text(r, font_sm, s_cte_cancel[cur_lang], 700, btn_y + 10, color_white, 1);

        // Touch hints
        draw_text(r, font_xs, s_cte_touch_hint[cur_lang], 640, btn_y + 56, color_grey, 1);
    }
}

static void draw_tutorial(SDL_Renderer *r) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 220);
    SDL_Rect scrim = {0, 0, W, H};
    SDL_RenderFillRect(r, &scrim);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    draw_card_shadow(r, 70, 70, 1140, 580);
    draw_rounded_box(r, 70, 70, 1140, 580, 14, color_bg2);
    draw_rounded_rect(r, 70, 70, 1140, 580, 14, color_cyan);
    draw_text_w(r, font_lg, s_tutorial_title[cur_lang], 640, 105, color_white, 1, 1060);

    char step_label[32];
    snprintf(step_label, sizeof(step_label), "%d / 5", tutorial_step + 1);
    draw_text(r, font_sm, step_label, 640, 178, color_cyan, 1);
    draw_rounded_box(r, 150, 218, 980, 6, 3, color_dark_grey);
    draw_rounded_box(r, 150, 218, 980 * (tutorial_step + 1) / 5, 6, 3, color_cyan);
    draw_rounded_box(r, 130, 250, 1020, 270, 12, color_card);
    draw_rounded_rect(r, 130, 250, 1020, 270, 12, color_card_border);
    draw_rounded_box(r, 130, 264, 5, 242, 2, color_cyan);

    SDL_Surface *body = TTF_RenderUTF8_Blended_Wrapped(font_md,
        s_tutorial_steps[cur_lang][tutorial_step], color_white, 980);
    if (body) {
        SDL_Texture *texture = SDL_CreateTextureFromSurface(r, body);
        if (texture) {
            SDL_Rect dst = {(W - body->w) / 2, 285, body->w, body->h};
            SDL_RenderCopy(r, texture, NULL, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(body);
    }

    if (tutorial_write_failed)
        draw_text_w(r, font_xs, s_tutorial_write_error[cur_lang], 640, 500, color_red, 1, 1040);

    int button_y = 560;
    draw_rounded_box(r, 120, button_y, 220, 48, 8,
        tutorial_step == 0 ? color_dark_grey : color_card_border);
    draw_text_w(r, font_sm, s_tutorial_previous[cur_lang], 230, button_y + 12,
        tutorial_step == 0 ? color_grey : color_white, 1, 200);
    draw_rounded_box(r, 380, button_y, 220, 48, 8, color_card_border);
    draw_text_w(r, font_sm, s_tutorial_skip[cur_lang], 490, button_y + 12, color_grey, 1, 200);
    bool last_step = tutorial_step == 4;
    draw_rounded_box(r, 940, button_y, 220, 48, 8, color_green);
    draw_text_w(r, font_sm, last_step ? s_tutorial_finish[cur_lang] :
        s_tutorial_next[cur_lang], 1050, button_y + 12, color_bg, 1, 200);
}

static void draw_tesla_guide(SDL_Renderer *r) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 220);
    SDL_Rect scrim = {0, 0, W, H};
    SDL_RenderFillRect(r, &scrim);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    draw_card_shadow(r, 70, 70, 1140, 580);
    draw_rounded_box(r, 70, 70, 1140, 580, 14, color_bg2);
    draw_rounded_rect(r, 70, 70, 1140, 580, 14, color_cyan);
    draw_text_w(r, font_lg, s_tesla_guide_title[cur_lang], 640, 105, color_white, 1, 1060);

    char step_label[32];
    snprintf(step_label, sizeof(step_label), "%d / 5", tesla_guide_step + 1);
    draw_text(r, font_sm, step_label, 640, 178, color_cyan, 1);
    draw_rounded_box(r, 150, 218, 980, 6, 3, color_dark_grey);
    draw_rounded_box(r, 150, 218, 980 * (tesla_guide_step + 1) / 5, 6, 3, color_cyan);

    SDL_Surface *body = TTF_RenderUTF8_Blended_Wrapped(font_md,
        s_tesla_guide_steps[cur_lang][tesla_guide_step], color_white, 980);
    if (body) {
        SDL_Texture *texture = SDL_CreateTextureFromSurface(r, body);
        if (texture) {
            SDL_Rect dst = {(W - body->w) / 2, 270, body->w, body->h};
            SDL_RenderCopy(r, texture, NULL, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(body);
    }

    draw_text_w(r, font_xs, "https://github.com/dodosiiii/SwitchInfoNX/releases",
        640, 500, color_cyan, 1, 1060);

    int button_y = 560;
    draw_rounded_box(r, 120, button_y, 220, 48, 8,
        tesla_guide_step == 0 ? color_dark_grey : color_card_border);
    draw_text_w(r, font_sm, s_tutorial_previous[cur_lang], 230, button_y + 12,
        tesla_guide_step == 0 ? color_grey : color_white, 1, 200);
    draw_rounded_box(r, 380, button_y, 220, 48, 8, color_card_border);
    draw_text_w(r, font_sm, s_tesla_guide_close[cur_lang], 490, button_y + 12,
        color_grey, 1, 200);
    bool last_step = tesla_guide_step == 4;
    draw_rounded_box(r, 940, button_y, 220, 48, 8, color_green);
    draw_text_w(r, font_sm, last_step ? s_tesla_guide_close[cur_lang] :
        s_tutorial_next[cur_lang], 1050, button_y + 12, color_bg, 1, 200);
}

// File browser modes
#define FB_MODE_BROWSE 0
#define FB_MODE_RENAME 1
#define FB_MODE_CONFIRM_DELETE 2
static int fb_mode = FB_MODE_BROWSE;
static char fb_rename_buf[256] = {0};
static int fb_rename_cursor = 0;
static char fb_clipboard_path[2048] = {0};
static int fb_clipboard_is_cut = 0;
static char fb_status_msg[128] = {0};
static u64 fb_status_tick = 0;

// ─── Full-screen File Browser ──────────────────────────────
static void draw_file_browser(SDL_Renderer *r) {
    char t[256];

    // Header
    draw_rounded_box(r, 40, 115, 1200, 70, 8, color_card);
    draw_rounded_rect(r, 40, 115, 1200, 70, 8, color_card_border);
    draw_text(r, font_md, s_file_browser[cur_lang], 60, 125, color_cyan, 0);
    // Path (truncated if too long)
    char path_display[80];
    int plen = (int)strlen(fb_path);
    if (plen > 72) {
        snprintf(path_display, sizeof(path_display), "...%s", fb_path + plen - 69);
    } else {
        snprintf(path_display, sizeof(path_display), "%s", fb_path);
    }
    draw_text(r, font_sm, path_display, 250, 132, color_yellow, 0);

    // Items box
    draw_rounded_box(r, 40, 195, 1200, 450, 8, color_card);
    draw_rounded_rect(r, 40, 195, 1200, 450, 8, color_card_border);

    if (fb_count == 0) {
        draw_text(r, font_sm, s_empty_dir[cur_lang], 640, 400, color_grey, 1);
    }

    int entry_h = 26;
    int list_x = 60;
    int list_w = 1160;
    int max_visible = (440 - 10) / entry_h;
    if (max_visible < 1) max_visible = 1;

    if (fb_scroll > fb_count - max_visible) fb_scroll = fb_count - max_visible;
    if (fb_scroll < 0) fb_scroll = 0;

    // Scroll indicators
    if (fb_scroll > 0)
        draw_text(r, font_xs, "^", 1190, 200, color_cyan, 1);
    if (fb_scroll + max_visible < fb_count)
        draw_text(r, font_xs, "v", 1190, 630, color_cyan, 1);

    // Column headers
    draw_text(r, font_xs, s_name_col[cur_lang], list_x, 202, color_grey, 0);
    char size_heading[64];
    snprintf(size_heading, sizeof(size_heading), "%s  [%s]", s_size_col[cur_lang], s_size_sort_names[cur_lang][fb_sort_mode]);
    draw_text_w(r, font_xs, size_heading, list_x + 820, 202, color_cyan, 0, 330);
    thickLineRGBA(r, list_x, 217, list_x + list_w, 217, 1,
        color_card_border.r, color_card_border.g, color_card_border.b, color_card_border.a);

    // Draw entries
    int list_top = 222;
    for (int i = 0; i < max_visible && fb_scroll + i < fb_count; i++) {
        int idx = fb_scroll + i;
        int ey = list_top + i * entry_h;
        SDL_Color fc = fb_is_dir[idx] ? color_yellow : color_white;

        // Selection highlight
        if (idx == fb_selected) {
            draw_rounded_box(r, list_x - 4, ey - 2, list_w + 8, entry_h, 4, (SDL_Color){40, 80, 160, 180});
            fc = color_cyan;
        }

        // Name (truncated)
        char name_disp[60];
        int nlen = (int)strlen(fb_entries[idx]);
        if (nlen > 55) {
            memcpy(name_disp, fb_entries[idx], 52);
            name_disp[52] = '.'; name_disp[53] = '.'; name_disp[54] = '.'; name_disp[55] = 0;
        } else {
            strncpy(name_disp, fb_entries[idx], sizeof(name_disp));
        }
        draw_text(r, font_sm, name_disp, list_x, ey, fc, 0);

        // Size column
        if (!fb_is_dir[idx] && fb_sizes[idx] > 0) {
            format_bytes((u64)fb_sizes[idx], t, sizeof(t));
            draw_text(r, font_xs, t, list_x + 820, ey + 2, color_grey, 0);
        } else if (fb_is_dir[idx]) {
            draw_text(r, font_xs, s_dir_tag[cur_lang], list_x + 820, ey + 2, color_yellow, 0);
        }
    }

    // Status message (brief)
    if (fb_status_msg[0]) {
        u64 elapsed = (armGetSystemTick() - fb_status_tick) / armGetSystemTickFreq();
        if (elapsed < 4)
            draw_text(r, font_sm, fb_status_msg, 640, 660, color_green, 1);
        else
            fb_status_msg[0] = 0;
    }

    // Footer bar for file browser
    SDL_Rect fbbg = {0, 660, W, 60};
    SDL_SetRenderDrawColor(r, color_bg2.r, color_bg2.g, color_bg2.b, 255);
    SDL_RenderFillRect(r, &fbbg);
    thickLineRGBA(r, 0, 660, W, 660, 2, color_card_border.r, color_card_border.g, color_card_border.b, 255);

    if (fb_mode == FB_MODE_RENAME) {
        draw_text(r, font_sm, s_fb_rename_hint[cur_lang], 20, 672, color_yellow, 0);
        char disp[300];
        snprintf(disp, sizeof(disp), "%s", fb_rename_buf[0] ? fb_rename_buf : s_fb_rename_empty[cur_lang]);
        draw_rounded_box(r, 20, 694, 700, 24, 4, color_card_border);
        draw_text(r, font_sm, disp, 30, 696, color_white, 0);
        // Cursor indicator
        int cursor_x = 30 + fb_rename_cursor * 11;
        if (cursor_x < 720) {
            SDL_Rect cur = {cursor_x, 716, 2, 2};
            SDL_SetRenderDrawColor(r, color_cyan.r, color_cyan.g, color_cyan.b, 255);
            SDL_RenderFillRect(r, &cur);
        }
        draw_rounded_box(r, 750, 684, 150, 30, 6, color_green);
        draw_text_w(r, font_xs, s_fb_rename_btn[cur_lang], 825, 692, color_white, 1, 136);
        draw_rounded_box(r, 920, 684, 150, 30, 6, color_card_border);
        draw_text_w(r, font_xs, s_cte_cancel[cur_lang], 995, 692, color_white, 1, 136);
    } else if (fb_mode == FB_MODE_CONFIRM_DELETE) {
        draw_text(r, font_sm, s_fb_delete_q[cur_lang], 20, 678, color_red, 0);
        draw_text_w(r, font_xs, s_fb_confirm_del[cur_lang], 20, 700, color_grey, 0, 620);
        draw_rounded_box(r, 790, 674, 170, 34, 6, color_red);
        draw_text_w(r, font_xs, s_fb_delete_btn[cur_lang], 875, 683, color_white, 1, 156);
        draw_rounded_box(r, 980, 674, 170, 34, 6, color_card_border);
        draw_text_w(r, font_xs, "Cancel", 1065, 683, color_white, 1, 156);
    } else {
        if (fb_selected >= 0 && fb_selected < fb_count) {
            char selected_info[360];
            char size_text[32];
            if (fb_is_dir[fb_selected]) {
                snprintf(selected_info, sizeof(selected_info), "%s  |  Folder",
                    fb_entries[fb_selected]);
            } else {
                format_bytes(fb_sizes[fb_selected], size_text, sizeof(size_text));
                snprintf(selected_info, sizeof(selected_info), "%s  |  File  |  %s",
                    fb_entries[fb_selected], size_text);
            }
            draw_text_w(r, font_xs, selected_info, 20, 674, color_cyan, 0, 620);
        } else {
            draw_text_w(r, font_xs, s_fb_browse_hint[cur_lang], 20, 674, color_grey, 0, 620);
        }
        if (fb_clipboard_path[0])
            draw_text_w(r, font_xs, s_fb_paste[cur_lang], 660, 674, color_cyan, 0, 560);
        draw_text_w(r, font_xs, s_fb_other_hint[cur_lang], 20, 696, color_grey, 0, 1220);
    }
}

// ─── File browser helpers ──────────────────────────────────
static void fb_delete_selected(void) {
    char fp[2048];
    snprintf(fp, sizeof(fp), "%s/%s", fb_path, fb_entries[fb_selected]);
    int sl = (int)strlen(fp);
    if (sl > 0 && fp[sl-1] == '/') fp[sl-1] = 0;

    int ok = 0;
    if (fb_is_dir[fb_selected])
        ok = (rmdir(fp) == 0);
    else
        ok = (remove(fp) == 0);

    if (ok) {
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Deleted: %s", fb_entries[fb_selected]);
        fb_open(fb_path);
    } else {
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Error deleting: %s", strerror(errno));
    }
    fb_status_tick = armGetSystemTick();
    fb_mode = FB_MODE_BROWSE;
}

static void fb_rename_selected(const char *new_name) {
    if (!new_name || !new_name[0]) return;
    char oldp[2048], newp[2048];
    snprintf(oldp, sizeof(oldp), "%s/%s", fb_path, fb_entries[fb_selected]);
    int sl = (int)strlen(oldp);
    if (sl > 0 && oldp[sl-1] == '/') oldp[sl-1] = 0;
    snprintf(newp, sizeof(newp), "%s/%s", fb_path, new_name);

    if (rename(oldp, newp) == 0) {
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Renamed to: %.110s", new_name);
        fb_open(fb_path);
    } else {
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Error renaming: %s", strerror(errno));
    }
    fb_status_tick = armGetSystemTick();
    fb_mode = FB_MODE_BROWSE;
}

static void fb_copy_selected(int cut) {
    snprintf(fb_clipboard_path, sizeof(fb_clipboard_path), "%s/%s", fb_path, fb_entries[fb_selected]);
    int sl = (int)strlen(fb_clipboard_path);
    if (sl > 0 && fb_clipboard_path[sl-1] == '/') fb_clipboard_path[sl-1] = 0;
    fb_clipboard_is_cut = cut;
    snprintf(fb_status_msg, sizeof(fb_status_msg), "%s: %s", cut ? "Cut" : "Copied", fb_entries[fb_selected]);
    fb_status_tick = armGetSystemTick();
}

static int fb_copy_file(const char *src, const char *dst) {
    FILE *sf = fopen(src, "rb");
    if (!sf) return -1;
    FILE *df = fopen(dst, "wb");
    if (!df) { fclose(sf); return -1; }
    char buf[16384];
    size_t n;
    int ok = 0;
    while ((n = fread(buf, 1, sizeof(buf), sf)) > 0) {
        if (fwrite(buf, 1, n, df) != n) { ok = -1; break; }
    }
    fclose(df);
    fclose(sf);
    return ok;
}

static int fb_copy_recursive(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0755) != 0) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *e;
        int ok = 0;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char s[2048], dd[2048];
            snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
            snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
            if (fb_copy_recursive(s, dd) != 0) { ok = -1; break; }
        }
        closedir(d);
        return ok;
    }
    return fb_copy_file(src, dst);
}

static void fb_paste(void) {
    if (!fb_clipboard_path[0]) return;
    const char *name = strrchr(fb_clipboard_path, '/');
    if (!name) return;
    name++;
    char dst[2048];
    snprintf(dst, sizeof(dst), "%s/%s", fb_path, name);

    // If destination exists, append _copy (before the extension for files)
    struct stat st;
    if (stat(dst, &st) == 0) {
        char base[256], ext[64];
        struct stat sst;
        int src_is_dir = (stat(fb_clipboard_path, &sst) == 0 && S_ISDIR(sst.st_mode));
        const char *dot = strrchr(name, '.');
        if (dot && !src_is_dir) {
            size_t blen = dot - name;
            if (blen > sizeof(base)-1) blen = sizeof(base)-1;
            memcpy(base, name, blen); base[blen] = 0;
            snprintf(ext, sizeof(ext), "%s", dot);
        } else {
            strncpy(base, name, sizeof(base)-1); base[sizeof(base)-1] = 0;
            ext[0] = 0;
        }
        snprintf(dst, sizeof(dst), "%s/%s_copy%s", fb_path, base, ext);
    }

    int ok = 0;
    if (fb_clipboard_is_cut) {
        ok = (rename(fb_clipboard_path, dst) == 0);
        if (ok) fb_clipboard_path[0] = 0;
    } else {
        // Copy files or directories (recursive)
        ok = (fb_copy_recursive(fb_clipboard_path, dst) == 0);
    }

    if (ok) {
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Pasted: %s", name);
        fb_open(fb_path);
    } else {
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Error pasting: %s", strerror(errno));
    }
    fb_status_tick = armGetSystemTick();
}

static bool ui_cursor_push_click(void) {
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.state = SDL_PRESSED;
    event.button.which = 1;
    event.button.x = ui_cursor_x;
    event.button.y = ui_cursor_y;
    int down_result = SDL_PushEvent(&event);

    event.type = SDL_MOUSEBUTTONUP;
    event.button.state = SDL_RELEASED;
    SDL_PushEvent(&event);
    return down_result == 1;
}

// ─── Main loop ─────────────────────────────────────────────

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    char clock_msg[96];
    bool sdl_initialized = false, ttf_initialized = false;

    // Initialize Switch services
    setsysInitialize();
    setInitialize();
    psmInitialize();
    nifmInitialize(NifmServiceType_User);
    clock_last_error = clkrstInitialize();
    plInitialize(PlServiceType_User);
    socketInitialize(&ftp_socket_config);
    nsInitialize();
    account_init_ok = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));
    splInitialize();

    // Pad state config
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    start_tick = armGetSystemTick();
    ctrl_brightness = -1.0f;
    lbl_detect_environment();

    // Load settings config
    load_config();
    bool tutorial_state_found = false;
    tutorial_completed = tutorial_load_completed(&tutorial_state_found);
    if (!tutorial_state_found && !tutorial_save_state(false))
        tutorial_write_failed = true;
    load_game_stats();

    // Initialize log mutexes once (used by Transfer page and server threads)
    mutexInit(&ftp_log_mtx);
    mutexInit(&mtp_log_mtx);
    mutexInit(&storage_scan_mutex);
    storage_scan_mutex_ready = true;

    // Initialize MAC address via socket ioctl
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        char buf[sizeof(struct ifreq) + 64];
        struct ifreq *ifr = (struct ifreq *)buf;
        strncpy(ifr->ifr_name, "wlan0", sizeof(ifr->ifr_name) - 1);
        if (ioctl(s, SIOCGIFINDEX, ifr) == 0) {
            ifr->ifr_addr.sa_family = AF_LINK;
            if (ioctl(s, SIOCGHWADDR, ifr) == 0) {
                struct sockaddr_dl *sdl = (struct sockaddr_dl *)&ifr->ifr_addr;
                if (sdl->sdl_alen >= 6) {
                    memcpy(mac_addr, LLADDR(sdl), 6);
                    mac_addr_valid = true;
                }
            }
        }
        close(s);
    }

    // Six-axis sensors for gyro/accel will be initialized in main loop after pad update
    sixaxis_init_ok = false;

    // Initialize SDL2
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        goto exit_app;
    }
    sdl_initialized = true;

    if (TTF_Init() < 0) {
        goto exit_app;
    }
    ttf_initialized = true;

    window = SDL_CreateWindow("Switch Info NX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, 0);
    if (!window) {
        goto exit_app;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        goto exit_app;
    }

    // Use the system's language font where the Latin/standard font has no glyphs.
    if (!load_ui_fonts()) {
        // No font available
        goto exit_app;
    }
    tutorial_active = !tutorial_completed;
    tutorial_step = 0;

    int cur = 0;
    bool quit = false;
    
    void (*pages[PGS])(SDL_Renderer *) = { 
        draw_pg0, draw_pg1, draw_pg2, draw_pg3, draw_pg4, NULL, draw_pg6, draw_pg7, draw_pg8
    };

    // Auto-refresh timer
    last_refresh = armGetSystemTick();

    while (appletMainLoop() && !quit) {
        if (net_ping_capture_pending && !net_test_running) {
            if (net_ping_ms > 0.0f) {
                net_ping_history[net_ping_history_pos] = net_ping_ms;
                net_ping_history_pos = (net_ping_history_pos + 1) % NET_PING_HIST_SIZE;
                if (net_ping_history_count < NET_PING_HIST_SIZE) net_ping_history_count++;
            }
            net_ping_capture_pending = false;
        }
        padUpdate(&pad);
        network_sample_traffic();
        u64 down = padGetButtonsDown(&pad);
        bool tutorial_input_captured = tutorial_active || tesla_guide_active;
        if (tutorial_active) {
            if (down & HidNpadButton_A) tutorial_next();
            if (down & HidNpadButton_B) {
                if (tutorial_step > 0) tutorial_step--;
                else tutorial_finish();
            }
            if (down & HidNpadButton_Left) {
                if (tutorial_step > 0) tutorial_step--;
            }
            if (down & HidNpadButton_Right) tutorial_next();
            down = 0;
        } else if (tesla_guide_active) {
            if (down & HidNpadButton_A) {
                if (tesla_guide_step < 4) tesla_guide_step++;
                else tesla_guide_active = false;
            }
            if (down & HidNpadButton_B) tesla_guide_active = false;
            if ((down & HidNpadButton_Left) && tesla_guide_step > 0)
                tesla_guide_step--;
            if (down & HidNpadButton_Right) {
                if (tesla_guide_step < 4) tesla_guide_step++;
                else tesla_guide_active = false;
            }
            down = 0;
        }
        bool virtual_a_click = false;
        if (!screen_test_active) {
            HidAnalogStickState left_stick = padGetStickPos(&pad, 0);
            HidAnalogStickState right_stick = padGetStickPos(&pad, 1);
            if (abs(left_stick.x) > 8000 || abs(left_stick.y) > 8000) {
                ui_cursor_x += left_stick.x * 18 / 32768;
                ui_cursor_y -= left_stick.y * 18 / 32768;
                if (ui_cursor_x < 40) ui_cursor_x = 40;
                if (ui_cursor_x > W - 40) ui_cursor_x = W - 40;
                if (ui_cursor_y < 35) ui_cursor_y = 35;
                if (ui_cursor_y > H - 30) ui_cursor_y = H - 30;
            }
            if (!tutorial_input_captured && !popup_active && !screen_test_active &&
                !custom_theme_editing && !(cur == 1 && fb_active)) {
                int *scroll_value = NULL;
                int scroll_limit = 0;
                switch (cur) {
                    case 0: scroll_value = &system_scroll; scroll_limit = system_scroll_max; break;
                    case 1: scroll_value = &storage_scroll; scroll_limit = storage_scroll_max; break;
                    case 2: scroll_value = &net_scroll; scroll_limit = net_scroll_max; break;
                    case 3: scroll_value = &transfer_scroll; scroll_limit = transfer_scroll_max; break;
                    case 4: scroll_value = &perf_scroll; scroll_limit = perf_scroll_max; break;
                    case 5: scroll_value = &ctrl_scroll; scroll_limit = ctrl_scroll_max; break;
                    case 6: scroll_value = &tools_scroll; scroll_limit = tools_scroll_max; break;
                    case 7: scroll_value = &about_scroll; scroll_limit = about_scroll_max; break;
                    case 8: scroll_value = &settings_scroll; scroll_limit = settings_scroll_max; break;
                    default: break;
                }
                if (scroll_value && scroll_limit > 0 && abs(right_stick.y) > 8000) {
                    int scroll_delta = -right_stick.y * 16 / 32768;
                    if (scroll_delta == 0) scroll_delta = right_stick.y > 0 ? -1 : 1;
                    *scroll_value += scroll_delta;
                    if (*scroll_value < 0) *scroll_value = 0;
                    if (*scroll_value > scroll_limit) *scroll_value = scroll_limit;
                }
            }
            if (!tutorial_input_captured && (down & HidNpadButton_A)) {
                virtual_a_click = ui_cursor_push_click();
                if (virtual_a_click) down &= ~HidNpadButton_A;
            }
        }
        fan_safety_update();
        if (!vibe_init_ok) {
            static u64 vibration_retry_tick = 0;
            u64 now = armGetSystemTick();
            if (now - vibration_retry_tick > armGetSystemTickFreq()) {
                vibration_retry_tick = now;
                init_vibration_for_pad(&pad);
            }
        }

        // Six-axis sensor init (keeps retrying for Pro-con / single Joy-Con with 1 handle)
        if (!sixaxis_init_ok) {
            static u64 sixaxis_last_try = 0;
            u64 now = armGetSystemTick();
            if (now - sixaxis_last_try > armGetSystemTickFreq() / 4) {
                sixaxis_last_try = now;
                Result rc = 1;
                HidNpadIdType hid_id = padIsHandheld(&pad) ? HidNpadIdType_Handheld : HidNpadIdType_No1;
                u32 style = padIsHandheld(&pad) ? HidNpadStyleTag_NpadHandheld : HidNpadStyleTag_NpadJoyDual;
                // Try 2 handles first (dual Joy-Con / handheld)
                rc = hidGetSixAxisSensorHandles(&sixaxis_handles[0], 2, hid_id, style);
                if (R_FAILED(rc)) {
                    // Fallback: try 1 handle (Pro Controller, single Joy-Con)
                    rc = hidGetSixAxisSensorHandles(&sixaxis_handles[0], 1, hid_id, style);
                    if (R_FAILED(rc)) {
                        static const u32 styles[] = { HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight };
                        for (int si = 0; si < 3 && R_FAILED(rc); si++)
                            rc = hidGetSixAxisSensorHandles(&sixaxis_handles[0], 1, hid_id, styles[si]);
                    }
                }
                if (R_SUCCEEDED(rc)) {
                    sixaxis_handles_count = 1;
                    hidStartSixAxisSensor(sixaxis_handles[0]);
                    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&sixaxis_handles[1], 1, hid_id, style))) {
                        hidStartSixAxisSensor(sixaxis_handles[1]);
                        sixaxis_handles_count = 2;
                    }
                    sixaxis_init_ok = true;
                }
            }
        }

        if (down & HidNpadButton_Plus) {
            quit = true;
        }

        // SDL events (touch + mouse support)
        SDL_Event event;
        static int touch_start_y = 0;
        static bool touch_scroll_blocked = false;
        static bool touch_scroll_active = false;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) quit = true;

            int mx = -1, my = -1;
            int ev_down = 0, ev_move = 0, ev_up = 0;

            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.which != SDL_TOUCH_MOUSEID) {
                mx = event.button.x; my = event.button.y; ev_down = 1;
            } else if (event.type == SDL_MOUSEBUTTONUP && event.button.which != SDL_TOUCH_MOUSEID) {
                mx = event.button.x; my = event.button.y; ev_up = 1;
            } else if (event.type == SDL_MOUSEMOTION && event.motion.which != SDL_TOUCH_MOUSEID && (event.motion.state & SDL_BUTTON_LMASK)) {
                mx = event.motion.x; my = event.motion.y; ev_move = 1;
            } else if (event.type == SDL_FINGERDOWN) {
                mx = (int)(event.tfinger.x * W); my = (int)(event.tfinger.y * H); ev_down = 1;
            } else if (event.type == SDL_FINGERUP) {
                mx = (int)(event.tfinger.x * W); my = (int)(event.tfinger.y * H); ev_up = 1;
            } else if (event.type == SDL_FINGERMOTION) {
                mx = (int)(event.tfinger.x * W); my = (int)(event.tfinger.y * H); ev_move = 1;
            }

            if (ev_up) {
                touch_vibe_index = -1;
                touch_scroll_active = false;
                touch_scroll_blocked = false;
                transfer_log_touch_y = -1;
                tools_game_touch_y = -1;
            }
            if (tutorial_active) {
                if (ev_down && my >= 560 && my <= 608) {
                    if (mx >= 120 && mx <= 340) {
                        if (tutorial_step > 0) tutorial_step--;
                    } else if (mx >= 380 && mx <= 600) {
                        tutorial_finish();
                    } else if (mx >= 940 && mx <= 1160) {
                        tutorial_next();
                    }
                }
                continue;
            }
            if (tesla_guide_active) {
                if (ev_down && my >= 560 && my <= 608) {
                    if (mx >= 120 && mx <= 340) {
                        if (tesla_guide_step > 0) tesla_guide_step--;
                    } else if (mx >= 380 && mx <= 600) {
                        tesla_guide_active = false;
                    } else if (mx >= 940 && mx <= 1160) {
                        if (tesla_guide_step < 4) tesla_guide_step++;
                        else tesla_guide_active = false;
                    }
                }
                continue;
            }
            if (cur == 6 && !popup_active && ev_down && mx >= 690 && mx <= 1210 &&
                my >= 974 - tools_scroll && my <= 1006 - tools_scroll) {
                tesla_guide_step = 0;
                tesla_guide_active = true;
                continue;
            }
            if (cur == 6 && !popup_active && ev_down && mx >= 930 && mx <= 1210 &&
                my >= 274 - tools_scroll && my <= 312 - tools_scroll) {
                Result toggle_rc = lbl_auto_supported() ? lblInitialize() : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
                if (R_SUCCEEDED(toggle_rc)) {
                    bool auto_br_enabled = false;
                    toggle_rc = lblIsAutoBrightnessControlEnabled(&auto_br_enabled);
                    if (R_SUCCEEDED(toggle_rc)) {
                        toggle_rc = auto_br_enabled ? lblDisableAutoBrightnessControl() :
                            lblEnableAutoBrightnessControl();
                    }
                    lblExit();
                }
                if (R_FAILED(toggle_rc)) {
                    snprintf(popup_text, sizeof(popup_text), "Auto brightness failed: %08X",
                        (unsigned)toggle_rc);
                    popup_type = POPUP_INFO;
                    popup_active = true;
                    popup_start_tick = armGetSystemTick();
                }
            }
            if (cur == 6 && ev_down) {
                int rumble_y = 460 - tools_scroll;
                if (my >= rumble_y && my <= rumble_y + 40) {
                    if (mx >= 70 && mx <= 290) touch_vibe_index = 0;
                    else if (mx >= 310 && mx <= 530) touch_vibe_index = 1;
                }
            }
            if (!ev_down && !ev_move && !ev_up) continue;

            // Popup interaction
            if (popup_active && ev_down) {
                if (popup_type == POPUP_FAN) {
                    int pw = 700, ph = 280, px = (W - pw) / 2, py = (H - ph) / 2;
                    // Close button (X in top-right corner)
                    if (mx >= px + pw - 40 && mx <= px + pw - 10 && my >= py + 10 && my <= py + 40) {
                        popup_active = false;
                        if (!fan_manual_active) fan_service_restore();
                        continue;
                    }
                    // Slider interaction
                    int sl_y = py + 120, sl_x = px + 80, sl_w = pw - 160;
                    if (my >= sl_y - 15 && my <= sl_y + 25) {
                        int val = 30 + (mx - sl_x) * 70 / sl_w;
                        if (val < 30) val = 30;
                        if (val > 100) val = 100;
                        popup_fan_val = val;
                        continue;
                    }
                    // - and + buttons
                    int btn_y = py + 180, bx = px + pw/2;
                    if (mx >= bx - 60 && mx <= bx - 10 && my >= btn_y && my <= btn_y + 40) {
                        popup_fan_val -= 5; if (popup_fan_val < 30) popup_fan_val = 30; continue;
                    }
                    if (mx >= bx + 10 && mx <= bx + 60 && my >= btn_y && my <= btn_y + 40) {
                        popup_fan_val += 5; if (popup_fan_val > 100) popup_fan_val = 100; continue;
                    }
                    int auto_x = px + pw/2 - 135, ap_x = px + pw/2 + 15, ap_y = btn_y + 60;
                    if (mx >= auto_x && mx <= auto_x + 120 && my >= ap_y && my <= ap_y + 40) {
                        fan_service_restore();
                        popup_active = false;
                        add_alert("Automatic fan control restored.");
                        continue;
                    }
                    if (mx >= ap_x && mx <= ap_x + 120 && my >= ap_y && my <= ap_y + 40) {
                        Result rc = fan_apply_safe_level(popup_fan_val);
                        if (R_FAILED(rc)) {
                            fan_service_restore();
                            add_alert("Fan control failed; automatic mode restored.");
                        }
                        else { fan_safety_tick = armGetSystemTick(); add_alert("Fan speed applied."); }
                        popup_active = false;
                        continue;
                    }
                    // Tap outside -> dismiss
                    if (mx < px || mx > px + pw || my < py || my > py + ph) {
                        popup_active = false;
                        if (!fan_manual_active) fan_service_restore();
                        continue;
                    }
                } else {
                    popup_active = false;
                    continue;
                }
            }
            // Popup touch_move for slider drag
            if (popup_active && popup_type == POPUP_FAN && ev_move) {
                int pw = 700, ph = 280, px = (W - pw) / 2, py = (H - ph) / 2;
                int sl_y = py + 120, sl_x = px + 80, sl_w = pw - 160;
                if (my >= sl_y - 15 && my <= sl_y + 25) {
                    int val = 30 + (mx - sl_x) * 70 / sl_w;
                    if (val < 30) val = 30;
                    if (val > 100) val = 100;
                    popup_fan_val = val;
                    continue;
                }
            }

            // Tab navigation (blocked when custom theme editor is open)
            if (ev_down && my >= 51 && my <= 111 && !(cur == 8 && custom_theme_editing)) {
                if (mx >= 4 && mx <= 42) {
                    cur = (cur - 1 + PGS) % PGS;
                } else if (mx >= W - 42 && mx <= W - 4) {
                    cur = (cur + 1) % PGS;
                } else if (mx >= 46 && mx <= 46 + 132 * PGS) {
                    int t = (mx - 46) / 132;
                    if (t >= 0 && t < PGS) cur = t;
                }
            }

            if (cur == 3 && ev_down) {
                // cards_y = 220 - transfer_scroll
                int pg3_cards_y = 220 - transfer_scroll;
                // FTP toggle button (cards_y+55 .. cards_y+89)
                if (mx >= 70 && mx <= 210 && my >= pg3_cards_y + 55 && my <= pg3_cards_y + 89) {
                    if (ftp_on) ftp_stop(); else ftp_start();
                }
                // MTP toggle button
                if (mx >= 690 && mx <= 830 && my >= pg3_cards_y + 55 && my <= pg3_cards_y + 89) {
                    if (mtp_on) mtp_stop_local(); else mtp_start_local();
                }
                // FTP/FTPD mode selector
                int mode_sel_y = pg3_cards_y + 252;
                if (my >= mode_sel_y && my <= mode_sel_y + 26) {
                    if ((mx >= 70 && mx <= 126) || (mx >= 134 && mx <= 194)) {
                        ftp_mode = !ftp_mode;
                        if (ftp_on) { ftp_stop(); ftp_start(); }
                    }
                }
            }

            if (cur == 3) {
                // Log area touch-scroll (absolute coords: log_card at cards_y+335, content ~+50 to +195)
                int pg3_log_top = 220 - transfer_scroll + 335 + 45;
                int pg3_log_bot = 220 - transfer_scroll + 335 + 200;
                if (ev_down && mx >= 55 && mx <= 1225 && my >= pg3_log_top && my <= pg3_log_bot)
                    transfer_log_touch_y = my;
                else if (ev_move && transfer_log_touch_y >= 0 && mx >= 55 && mx <= 1225) {
                    int dy = transfer_log_touch_y - my;
                    if (dy > 3 || dy < -3) {
                        transfer_log_scroll += dy / 19;
                        if (transfer_log_scroll < 0) transfer_log_scroll = 0;
                        if (transfer_log_scroll > transfer_log_scroll_max) transfer_log_scroll = transfer_log_scroll_max;
                        transfer_log_touch_y = my;
                    }
                }
            } else if (!ev_down && !ev_move) {
                transfer_log_touch_y = -1;
            }

            if (cur == 4 && ev_down) {
                int export_y = 616 - perf_scroll;
                if (mx >= 885 && mx <= 1215 && my >= export_y && my <= export_y + 27) {
                    save_perf_metrics();
                    continue;
                }
            }

            // Tools page: brightness slider drag
            if (cur == 6 && !lbl_emulator) {
                int bs_y = 235 - tools_scroll;
                if (mx >= 70 && mx <= 1210 && my >= bs_y - 20 && my <= bs_y + 20) {
                    float br = (float)(mx - 70) / 1140.0f;
                    if (br < 0.0f) br = 0.0f;
                    if (br > 1.0f) br = 1.0f;
                    ctrl_brightness = br;
                    brightness_apply(ctrl_brightness);
                }
            }
            // Tools page: buttons
            // Network test touch
            if (cur == 2 && ev_down) {
                if (my >= 600 - net_scroll && my <= 636 - net_scroll && mx >= 365 && mx <= 585 && !net_test_running) {
                    net_test_start();
                }
                int antiban_btn_y = 1140 + 160 - net_scroll;
                if (my >= antiban_btn_y && my <= antiban_btn_y + 38 && mx >= 880 && mx <= 1200 && !antiban_running) {
                    antiban_start_test();
                }
            }

            if (cur == 5 && ev_down) {
                // Rumble test buttons: y = 940 + 88 - ctrl_scroll, h = 48
                int rmb_btn_y = 940 + 88 - ctrl_scroll;
                if (my >= rmb_btn_y && my <= rmb_btn_y + 48) {
                    if (mx >= 70 && mx <= 410) {
                        ctrl_trigger_rumble(0, 300);
                    } else if (mx >= 450 && mx <= 790) {
                        ctrl_trigger_rumble(-1, 300);
                    } else if (mx >= 830 && mx <= 1170) {
                        ctrl_trigger_rumble(1, 300);
                    }
                }
                // Reset drift tracker button: y = 1120 + 186 - ctrl_scroll, h = 40, w = 460 (centered at 410..870)
                int rst_btn_y = 1120 + 186 - ctrl_scroll;
                if (my >= rst_btn_y && my <= rst_btn_y + 40 && mx >= 410 && mx <= 870) {
                    stick_l_peak_drift = 0.0f;
                    stick_r_peak_drift = 0.0f;
                    add_alert(cur_lang == 1 ? "Pics de derive des sticks reinitialises !" : "Stick drift peak values reset!");
                }
            }

            if (cur == 6 && ev_down) {
                int save_card_y = 2060 - tools_scroll;
                int save_action_y = 2210 - tools_scroll;
                if (mx >= 40 && mx <= 1240 && my >= save_card_y && my <= save_card_y + 270) {
                    int save_count = save_manager_count();
                    bool save_action_handled = false;
                    if (save_count > 0 && my >= save_action_y && my <= save_action_y + 44) {
                        if (mx >= 70 && mx <= 170) {
                            save_manager_selected = (save_manager_selected + save_count - 1) % save_count;
                            save_backup_list_save = -1;
                            save_backup_selected = 0;
                            save_backup_result = 0;
                            save_restore_confirm = false;
                            save_action_handled = true;
                        } else if (mx >= 180 && mx <= 280) {
                            save_manager_selected = (save_manager_selected + 1) % save_count;
                            save_backup_list_save = -1;
                            save_backup_selected = 0;
                            save_backup_result = 0;
                            save_restore_confirm = false;
                            save_action_handled = true;
                        } else if (mx >= 310 && mx <= 560) {
                            Result rc = save_manager_backup(save_manager_selected);
                            if (R_SUCCEEDED(rc)) {
                                save_backup_result = 1;
                                add_alert("Save backup created on SD.");
                                save_backup_list_save = -1;
                            }
                            else {
                                save_backup_result = -1;
                                snprintf(popup_text, sizeof(popup_text), "Backup failed: %08X", (unsigned)rc);
                                popup_type = POPUP_INFO;
                                popup_active = true;
                                popup_start_tick = armGetSystemTick();
                            }
                            save_restore_confirm = false;
                            save_action_handled = true;
                        } else if (mx >= 580 && mx <= 880) {
                            bool confirm_live = save_restore_confirm && armGetSystemTick() - save_restore_confirm_tick < armGetSystemTickFreq() * 8;
                            int backup_count = save_manager_backup_count(save_manager_selected);
                            if (!backup_count) {
                                add_alert("Create a backup before restoring.");
                            } else if (!confirm_live) {
                                save_restore_confirm = true;
                                save_restore_confirm_tick = armGetSystemTick();
                                add_alert("Tap RESTORE again within 8 seconds to confirm.");
                            } else {
                                save_restore_confirm = false;
                                Result rc = save_manager_restore_backup(save_manager_selected, save_backup_selected);
                                if (R_SUCCEEDED(rc)) {
                                    add_alert("Save restored; previous data backed up.");
                                    save_backup_list_save = -1;
                                }
                                else { snprintf(popup_text, sizeof(popup_text), "Restore failed: %08X", (unsigned)rc); popup_type = POPUP_INFO; popup_active = true; popup_start_tick = armGetSystemTick(); }
                            }
                            save_action_handled = true;
                        } else if (mx >= 900 && mx <= 1110) {
                            int backup_count = save_manager_backup_count(save_manager_selected);
                            if (backup_count > 0) save_backup_selected = (save_backup_selected + 1) % backup_count;
                            save_restore_confirm = false;
                            save_action_handled = true;
                        }
                    }
                    if (save_action_handled) continue;
                }
                if (mx >= 40 && mx <= 620 && my >= 790 - tools_scroll && my <= 970 - tools_scroll) {
                    Result rc = fan_service_start();
                    if (R_SUCCEEDED(rc)) {
                        popup_fan_val = fan_manual_active ? fan_requested_pct : 45;
                        popup_type = POPUP_FAN;
                        popup_active = true;
                        popup_start_tick = armGetSystemTick();
                    } else {
                        snprintf(popup_text, sizeof(popup_text), "Fan control unavailable (%08X)", (unsigned)rc);
                        popup_type = POPUP_INFO;
                        popup_active = true;
                        popup_start_tick = armGetSystemTick();
                    }
                    continue;
                }
                int exp_y = 460 - tools_scroll;
                int dp_y = 745 - tools_scroll;
                if (mx >= 690 && mx <= 930 && my >= exp_y && my <= exp_y + 40)
                    export_system_info();
                if (mx >= 70 && mx <= 290 && my >= dp_y && my <= dp_y + 32)
                    screen_test_active = 1;

                // App mode selector touch
                int mod_y = 850 - tools_scroll;
                for (int i = 0; i < 3; i++) {
                    int bx = 690 + i * 180;
                    if (mx >= bx && mx <= bx + 170 && my >= mod_y && my <= mod_y + 30) {
                        if (i == 0) app_mode = 0;
                        else if (i == 1) {
                            overlay_status = detect_ultrahand_overlay();
                            snprintf(popup_text, sizeof(popup_text), "%s",
                                overlay_status_text(overlay_status));
                            popup_type = POPUP_INFO;
                            popup_active = true;
                            popup_start_tick = armGetSystemTick();
                        }
                        else {
                            snprintf(popup_text, sizeof(popup_text), "%s",
                                s_sysmodule_desc[cur_lang]);
                            popup_type = POPUP_INFO;
                            popup_active = true;
                            popup_start_tick = armGetSystemTick();
                        }
                    }
                }

                // CPU Scaling controls touch
                int scale_card_y = 1010 - tools_scroll;
                if (mx >= 40 && mx <= 620 && my >= scale_card_y && my <= scale_card_y + 150) {
                    // Tap on CPU/GPU sliders to set target
                    if (my >= scale_card_y + 48 && my <= scale_card_y + 67) {
                        int sl_x = 390, sl_w = 200;
                        int tap_x = mx - sl_x;
                        if (tap_x >= 0 && tap_x <= sl_w) {
                            int desired_mhz = 102 + tap_x * (1785 - 102) / sl_w;
                            int applied_mhz = 0;
                            Result rc = scale_set_cpu(desired_mhz, &applied_mhz);
                            if (R_SUCCEEDED(rc)) {
                                cpu_scale_target_mhz = applied_mhz;
                                save_config();
                                snprintf(clock_msg, sizeof(clock_msg), "CPU clock set to %d MHz", applied_mhz);
                                add_alert(clock_msg);
                            } else {
                                snprintf(clock_msg, sizeof(clock_msg), "CPU clock failed (%08X)", (unsigned)rc);
                                add_alert(clock_msg);
                            }
                        }
                    }
                    if (my >= scale_card_y + 76 && my <= scale_card_y + 95) {
                        int sl_x = 390, sl_w = 200;
                        int tap_x = mx - sl_x;
                        if (tap_x >= 0 && tap_x <= sl_w) {
                            int desired_mhz = 76 + tap_x * (921 - 76) / sl_w;
                            int applied_mhz = 0;
                            Result rc = scale_set_gpu(desired_mhz, &applied_mhz);
                            if (R_SUCCEEDED(rc)) {
                                gpu_scale_target_mhz = applied_mhz;
                                save_config();
                                snprintf(clock_msg, sizeof(clock_msg), "GPU clock set to %d MHz", applied_mhz);
                                add_alert(clock_msg);
                            } else {
                                snprintf(clock_msg, sizeof(clock_msg), "GPU clock failed (%08X)", (unsigned)rc);
                                add_alert(clock_msg);
                            }
                        }
                    }
                }

                // SD Speed Test button
                int sd_btn_y = 1940 - tools_scroll;
                if (mx >= 690 && mx <= 930 && my >= sd_btn_y && my <= sd_btn_y + 36) {
                    sd_speed_result = -2;
                    SDL_SetRenderDrawColor(renderer, color_bg.r, color_bg.g, color_bg.b, 255);
                    SDL_RenderClear(renderer);
                    draw_header(renderer);
                    draw_tabs(renderer, 6);
                    draw_pg6(renderer);
                    draw_footer(renderer, 6);
                    SDL_RenderPresent(renderer);
                    sd_run_speed_test();
                }
            }

            // Floating refresh button
            if (cur != 6 && ev_down) {
                int dx = mx - (W - 70 + 25), dy = my - (148 + 25);
                if (dx*dx + dy*dy <= 22*22) refresh_count++;
            }

            // Storage page: tap file browser button or Y key
            if (cur == 1 && !fb_active && ev_down) {
                int sd_test_y = storage_test_btn_y - storage_scroll;
                int fb_btn_y = storage_fb_btn_y - storage_scroll;
                int scan_btn_y = storage_scan_button_y - storage_scroll;
                if (mx >= 1000 && mx <= 1210 && my >= scan_btn_y && my <= scan_btn_y + 34) {
                    storage_scan_start();
                    continue;
                }
                mutexLock(&storage_scan_mutex);
                bool scan_complete = storage_scan_state == STORAGE_SCAN_COMPLETE;
                mutexUnlock(&storage_scan_mutex);
                if (scan_complete) {
                    bool category_touched = false;
                    for (int i = 0; i < STORAGE_CATEGORY_COUNT; i++) {
                        int row_y = storage_scan_category_y + i * 27 - storage_scroll;
                        if (mx >= 60 && mx <= 1220 && my >= row_y - 2 && my <= row_y + 22) {
                            if (storage_scan_detail_category == i)
                                storage_scan_detail_ascending = !storage_scan_detail_ascending;
                            else {
                                storage_scan_detail_category = i;
                                storage_scan_detail_ascending = false;
                            }
                            category_touched = true;
                            break;
                        }
                    }
                    if (category_touched) continue;

                    int detail_y = storage_scan_details_y + 27 - storage_scroll;
                    bool detail_opened = false;
                    for (int i = 0; i < 5; i++) {
                        int row_y = detail_y + i * 20;
                        if (mx < 70 || mx > 1210 || my < row_y - 2 || my > row_y + 18) continue;
                        int category = storage_scan_detail_category;
                        if (category < 0 || category >= STORAGE_CATEGORY_COUNT) break;
                        int count = storage_scan_category_entries[category];
                        if (i >= count) break;
                        int order = storage_scan_detail_ascending ? count - 1 - i : i;
                        int entry_index = storage_scan_sorted_indices[category][order];
                        StorageScanEntry selected = storage_scan_entries[entry_index];
                        if (selected.is_dir) {
                            char path[512];
                            snprintf(path, sizeof(path), "sdmc:/%.255s", selected.name);
                            fb_open(path);
                        } else {
                            fb_open("sdmc:/");
                            for (int j = 0; j < fb_count; j++) {
                                if (strcmp(fb_entries[j], selected.name) == 0) {
                                    fb_selected = j;
                                    if (fb_selected >= fb_scroll + 15) fb_scroll = fb_selected - 14;
                                    break;
                                }
                            }
                        }
                        fb_mode = FB_MODE_BROWSE;
                        detail_opened = true;
                        break;
                    }
                    if (detail_opened) continue;
                }
                if (my >= fb_btn_y && my <= fb_btn_y + 40 && mx >= 690 && mx <= 950) {
                    fb_open("sdmc:/"); fb_mode = FB_MODE_BROWSE;
                }
                // Also tap SD speed test button
                if (my >= sd_test_y && my <= sd_test_y + 40 && mx >= 690 && mx <= 930) {
                    sd_speed_result = -2;
                    SDL_SetRenderDrawColor(renderer, color_bg.r, color_bg.g, color_bg.b, 255);
                    SDL_RenderClear(renderer);
                    draw_header(renderer);
                    draw_tabs(renderer, 1);
                    draw_pg1(renderer);
                    draw_footer(renderer, 1);
                    SDL_RenderPresent(renderer);
                    sd_run_speed_test();
                }
                // CFW Cleaner buttons
                int cleaner_btn_y = 900 + 150 - storage_scroll;
                if (my >= cleaner_btn_y && my <= cleaner_btn_y + 42 && mx >= 680 && mx <= 920) {
                    cfw_cleaner_scan();
                }
                if (my >= cleaner_btn_y && my <= cleaner_btn_y + 42 && mx >= 940 && mx <= 1200) {
                    cfw_cleaner_clean();
                }
            }
            // Storage page: tap file browser entry (full-screen)
            if (cur == 1 && fb_active && ev_down && fb_mode == FB_MODE_BROWSE &&
                my >= 198 && my <= 220 &&
                mx >= 860 && mx <= 1220) {
                int previous_sort_mode = fb_sort_mode;
                fb_sort_mode = (fb_sort_mode + 1) % 3;
                if (fb_sort_entries()) {
                    fb_scroll = 0;
                } else {
                    fb_sort_mode = previous_sort_mode;
                    snprintf(fb_status_msg, sizeof(fb_status_msg), "Not enough memory to sort files.");
                    fb_status_tick = armGetSystemTick();
                }
            }
            if (cur == 1 && fb_active && ev_down && fb_mode == FB_MODE_CONFIRM_DELETE) {
                if (mx >= 790 && mx <= 960 && my >= 674 && my <= 708) {
                    fb_delete_selected();
                } else if (mx >= 980 && mx <= 1150 && my >= 674 && my <= 708) {
                    fb_mode = FB_MODE_BROWSE;
                }
            }
            if (cur == 1 && fb_active && ev_down && fb_mode == FB_MODE_RENAME) {
                if (mx >= 750 && mx <= 900 && my >= 684 && my <= 714) {
                    if (fb_rename_buf[0]) fb_rename_selected(fb_rename_buf);
                } else if (mx >= 920 && mx <= 1070 && my >= 684 && my <= 714) {
                    fb_mode = FB_MODE_BROWSE;
                    fb_rename_buf[0] = 0;
                    fb_rename_cursor = 0;
                }
            }
            if (cur == 1 && fb_active && ev_down && fb_mode == FB_MODE_BROWSE &&
                my >= 222 && my <= 640 && mx >= 40 && mx <= 1240) {
                int tap_idx = (my - 222) / 26 + fb_scroll;
                if (tap_idx >= 0 && tap_idx < fb_count) {
                    fb_selected = tap_idx;
                    if (fb_is_dir[tap_idx]) {
                        char fp[2048];
                        snprintf(fp, sizeof(fp), "%s/%s", fb_path, fb_entries[tap_idx]);
                        int sl = (int)strlen(fp);
                        if (sl > 0 && fp[sl-1] == '/') fp[sl-1] = 0;
                        fb_open(fp);
                        fb_mode = FB_MODE_BROWSE;
                    }
                }
            }

            // Scrollable pages touch scroll
            int scroll_max = (cur == 0) ? system_scroll_max : (cur == 2) ? net_scroll_max : (cur == 3) ? transfer_scroll_max : (cur == 4) ? perf_scroll_max : (cur == 5) ? ctrl_scroll_max : (cur == 7) ? about_scroll_max : (cur == 6) ? tools_scroll_max : (cur == 1) ? storage_scroll_max : (cur == 8) ? settings_scroll_max : 0;
            int *scroll_var = (cur == 0) ? &system_scroll : (cur == 2) ? &net_scroll : (cur == 3) ? &transfer_scroll : (cur == 4) ? &perf_scroll : (cur == 5) ? &ctrl_scroll : (cur == 7) ? &about_scroll : (cur == 6) ? &tools_scroll : (cur == 1) ? &storage_scroll : (cur == 8) ? &settings_scroll : NULL;
            if (cur == 1 && fb_active) scroll_var = NULL;
            if (scroll_var && scroll_max > 0) {
                if (ev_down) {
                    touch_start_y = my;
                    touch_scroll_active = my >= 140 && my <= 650;
                    int save_action_y = 2210 - tools_scroll;
                    bool save_button_touched = my >= save_action_y && my <= save_action_y + 44 &&
                        ((mx >= 70 && mx <= 170) || (mx >= 180 && mx <= 280) ||
                         (mx >= 310 && mx <= 560) || (mx >= 580 && mx <= 880) ||
                         (mx >= 900 && mx <= 1110));
                    touch_scroll_blocked = cur == 6 && save_manager_count() > 0 && save_button_touched;
                    int game_list_top = 1180 - tools_scroll;
                    bool game_list_touched = cur == 6 && mx >= 40 && mx <= 1240 &&
                        my >= game_list_top + 45 &&
                        my <= game_list_top + 45 + VISIBLE_INSTALLED_GAMES * INSTALLED_GAME_ROW_HEIGHT;
                    if (game_list_touched) {
                        tools_game_touch_y = my;
                        touch_scroll_blocked = true;
                    }
                    if (cur == 1) {
                        int scan_button_y = storage_scan_button_y - storage_scroll;
                        bool analyzer_button = mx >= 1000 && mx <= 1210 &&
                            my >= scan_button_y && my <= scan_button_y + 34;
                        bool analyzer_row = mx >= 60 && mx <= 1220 &&
                            my >= storage_scan_category_y - storage_scroll - 2 &&
                            my <= storage_scan_category_y + STORAGE_CATEGORY_COUNT * 27 - storage_scroll + 22;
                        touch_scroll_blocked = touch_scroll_blocked || fb_active ||
                            analyzer_button || analyzer_row;
                        if (analyzer_button || analyzer_row || fb_active)
                            touch_scroll_active = false;
                    }
                    if (touch_scroll_blocked) touch_scroll_active = false;
                } else if (ev_move && touch_scroll_active && !touch_scroll_blocked) {
                    int dy = touch_start_y - my;
                    if (dy > 4 || dy < -4) {
                        *scroll_var += dy;
                        if (*scroll_var < 0) *scroll_var = 0;
                        if (*scroll_var > scroll_max) *scroll_var = scroll_max;
                        touch_start_y = my;
                    }
                }
                if (cur == 6 && ev_move && tools_game_touch_y >= 0 &&
                    mx >= 40 && mx <= 1240) {
                    int dy = tools_game_touch_y - my;
                    if (dy > 8 || dy < -8) {
                        int steps = dy / INSTALLED_GAME_ROW_HEIGHT;
                        if (!steps) steps = dy > 0 ? 1 : -1;
                        int max_scroll = installed_game_count > VISIBLE_INSTALLED_GAMES
                            ? installed_game_count - VISIBLE_INSTALLED_GAMES : 0;
                        int requested_scroll = tools_game_scroll + steps;
                        int overflow = requested_scroll < 0 ? requested_scroll :
                            (requested_scroll > max_scroll ? requested_scroll - max_scroll : 0);
                        tools_scroll += overflow * INSTALLED_GAME_ROW_HEIGHT;
                        if (tools_scroll < 0) tools_scroll = 0;
                        if (tools_scroll > tools_scroll_max) tools_scroll = tools_scroll_max;
                        tools_game_scroll = requested_scroll;
                        if (tools_game_scroll < 0) tools_game_scroll = 0;
                        if (tools_game_scroll > max_scroll) tools_game_scroll = max_scroll;
                        tools_game_touch_y = my;
                    }
                }
            }

            // Settings page: touch for language/theme/refresh selection
            if (cur == 8 && ev_down) {
                int ssc = settings_scroll;
                if (mx >= 70 && mx <= 460 && my >= 900 - ssc && my <= 940 - ssc) {
                    tutorial_step = 0;
                    tutorial_write_failed = false;
                    tutorial_active = true;
                    continue;
                }
                // Language buttons (2 rows)
                if (my >= 246 - ssc && my <= 326 - ssc) {
                    int lang_per_row = (LANG_MAX + 1) / 2;
                    for (int i = 0; i < LANG_MAX; i++) {
                        int row = i / lang_per_row;
                        int col = i % lang_per_row;
                        int bx = 70 + col * 110;
                        int by = 246 - ssc + row * 44;
                        if (mx >= bx && mx <= bx + 104 && my >= by && my <= by + 36) {
                            cur_lang = i;
                            load_ui_fonts();
                            save_config();
                            break;
                        }
                    }
                }
                // Theme buttons follow the rendered 9-column grid.
                if ((my >= 386 - ssc && my <= 418 - ssc) || (my >= 426 - ssc && my <= 458 - ssc)) {
                    int row = my <= 418 - ssc ? 0 : 1;
                    for (int col = 0; col < 9; col++) {
                        int i = row * 9 + col;
                        int bx = 55 + col * 125;
                        if (i < THEME_MAX && mx >= bx && mx <= bx + 116) {
                            cur_theme = i;
                            apply_theme(); save_config(); break;
                        }
                    }
                }
                // Custom theme editor button is below the theme grid.
                if (cur_theme == THEME_MAX - 1 && my >= 486 - ssc && my <= 518 - ssc && mx >= 70 && mx <= 310) {
                    custom_theme_editing = 1;
                    cte_backup[0]=custom_bg; cte_backup[1]=custom_card; cte_backup[2]=custom_border;
                    cte_backup[3]=custom_cyan; cte_backup[4]=custom_green; cte_backup[5]=custom_white;
                    cte_backup[6]=custom_grey; cte_backup[7]=custom_dark_grey; cte_backup[8]=custom_bg2;
                    cte_backup[9]=custom_bg3;
                }
                // Refresh options are below their rendered label and optional editor button.
                int ref_ys = (cur_theme == THEME_MAX - 1) ? 562 : 530;
                if (my >= ref_ys - ssc && my <= ref_ys + 36 - ssc) {
                    static const int rv[] = {0,1,3,5,10};
                    for (int i = 0; i < 5; i++) {
                        int bx = 70 + i * 112;
                        if (mx >= bx && mx <= bx + 104) {
                            auto_refresh_interval = rv[i];
                            settings_sel = 2;
                            save_config();
                            break;
                        }
                    }
                }
                int temp_alert_y = ref_ys + 100 - ssc;
                if (my >= temp_alert_y && my <= temp_alert_y + 36 &&
                    ((mx >= 80 && mx <= 114) || (mx >= 226 && mx <= 260))) {
                    settings_sel = 3;
                    if (mx < 170) {
                        if (temp_alert_millic > 30000) temp_alert_millic -= 5000;
                    } else if (temp_alert_millic < 90000) {
                        temp_alert_millic += 5000;
                    }
                    save_config();
                }
                int mem_alert_y = ref_ys + 188 - ssc;
                if (my >= mem_alert_y && my <= mem_alert_y + 36 &&
                    ((mx >= 80 && mx <= 114) || (mx >= 226 && mx <= 260))) {
                    settings_sel = 4;
                    if (mx < 170) {
                        if (mem_alert_frac > 0.5f) mem_alert_frac -= 0.05f;
                    } else if (mem_alert_frac < 0.95f) {
                        mem_alert_frac += 0.05f;
                    }
                    save_config();
                }
                // Custom theme editor touch (new layout coordinates)
                if (custom_theme_editing) {
                    for (int i = 0; i < 3; i++) {
                        int px = 110 + i * 365;
                        if (mx >= px && mx <= px + 340 && my >= 138 && my <= 168)
                            apply_custom_theme_palette(i);
                    }
                    int cte_cy = 177;
                    for (int i = 0; i < 10; i++) {
                        int row = i / 2;
                        int col = i % 2;
                        int px = 110 + col * 420;
                        int py = cte_cy + row * 40;
                        if (mx >= px && mx <= px + 400 && my >= py && my <= py + 36) {
                            cte_sel = i;
                        }
                    }
                    // Tap channel R/G/B sliders
                    int cte_edit_y = cte_cy + 5 * 40 + 8;
                    SDL_Color *cte_colors[] = {&custom_bg,&custom_card,&custom_border,&custom_cyan,&custom_green,&custom_white,&custom_grey,&custom_dark_grey,&custom_bg2,&custom_bg3};
                    for (int ch = 0; ch < 3; ch++) {
                        int by = cte_edit_y + 24 + ch * 30;
                        // Channel label
                        if (mx >= 180 && mx <= 220 && my >= by && my <= by + 24) {
                            cte_chan = ch;
                        }
                        // Slider value
                        if (mx >= 240 && mx <= 700 && my >= by && my <= by + 24) {
                            cte_chan = ch;
                            int val = (mx - 240) * 255 / 460;
                            if (val < 0) val = 0;
                            if (val > 255) val = 255;
                            if (ch == 0) cte_colors[cte_sel]->r = (u8)val;
                            else if (ch == 1) cte_colors[cte_sel]->g = (u8)val;
                            else cte_colors[cte_sel]->b = (u8)val;
                        }
                    }
                    // Tap Save button
                    int cte_btn_y = cte_edit_y + 24 + 3 * 30 + 12;
                    if (mx >= 340 && mx <= 540 && my >= cte_btn_y && my <= cte_btn_y + 44) {
                        custom_theme_editing = 0;
                        apply_theme();
                        save_config();
                    }
                    // Tap Cancel button
                    if (mx >= 600 && mx <= 800 && my >= cte_btn_y && my <= cte_btn_y + 44) {
                        custom_theme_editing = 0;
                        custom_bg = cte_backup[0]; custom_card = cte_backup[1]; custom_border = cte_backup[2];
                        custom_cyan = cte_backup[3]; custom_green = cte_backup[4]; custom_white = cte_backup[5];
                        custom_grey = cte_backup[6]; custom_dark_grey = cte_backup[7]; custom_bg2 = cte_backup[8];
                        custom_bg3 = cte_backup[9];
                        apply_theme();
                    }
                }
            }
        }

        // Screen test controls (works on any page once active)
        if (screen_test_active) {
            if (down & HidNpadButton_A) {
                screen_test_active++;
                // Cycle through phases 1..(SCREEN_TEST_PHASES-1); 0 = inactive.
                if (screen_test_active >= SCREEN_TEST_PHASES - 1) screen_test_active = 1;
            }
            if (down & HidNpadButton_B) screen_test_active = 0;
        }

        // Transfer page: D-Pad scrolls the page; A toggles WiFi FTP, X toggles USB, Y toggles FTP/FTPD mode
        if (cur == 3) {
            if (down & HidNpadButton_Up) {
                transfer_scroll -= 24;
                if (transfer_scroll < 0) transfer_scroll = 0;
            }
            if (down & HidNpadButton_Down) {
                transfer_scroll += 24;
                if (transfer_scroll > transfer_scroll_max) transfer_scroll = transfer_scroll_max;
            }
            if (down & HidNpadButton_A) { if (ftp_on) ftp_stop(); else ftp_start(); }
            if (down & HidNpadButton_X) { if (mtp_on) mtp_stop_local(); else mtp_start_local(); }
            if (down & HidNpadButton_Y) {
                ftp_mode = !ftp_mode;
                if (ftp_on) { ftp_stop(); ftp_start(); }
            }
        }

        // Tools page actions
        if (cur == 6) {
            if (down & HidNpadButton_B) export_system_info();
            if (down & HidNpadButton_A && lbl_auto_supported()) {
                if (R_SUCCEEDED(lblInitialize())) {
                    bool auto_br = false;
                    if (R_SUCCEEDED(lblIsAutoBrightnessControlEnabled(&auto_br))) {
                        if (auto_br) lblDisableAutoBrightnessControl();
                        else lblEnableAutoBrightnessControl();
                    }
                    lblExit();
                }
            }
            // CPU Scaling: ZR applies CPU target, ZL applies GPU target
            if (down & HidNpadButton_ZR) {
                if (cpu_scale_target_mhz > 0) {
                    int applied_mhz = 0;
                    Result rc = scale_set_cpu(cpu_scale_target_mhz, &applied_mhz);
                    if (R_SUCCEEDED(rc)) {
                        cpu_scale_target_mhz = applied_mhz;
                        save_config();
                        add_alert("CPU frequency applied.");
                    } else {
                        snprintf(clock_msg, sizeof(clock_msg), "CPU clock failed (%08X)", (unsigned)rc);
                        add_alert(clock_msg);
                    }
                } else {
                    add_alert("Set CPU target first using slider!");
                }
            }
            if (down & HidNpadButton_ZL) {
                if (gpu_scale_target_mhz > 0) {
                    int applied_mhz = 0;
                    Result rc = scale_set_gpu(gpu_scale_target_mhz, &applied_mhz);
                    if (R_SUCCEEDED(rc)) {
                        gpu_scale_target_mhz = applied_mhz;
                        save_config();
                        add_alert("GPU frequency applied.");
                    } else {
                        snprintf(clock_msg, sizeof(clock_msg), "GPU clock failed (%08X)", (unsigned)rc);
                        add_alert(clock_msg);
                    }
                } else {
                    add_alert("Set GPU target first using slider!");
                }
            }
            // L+R together -> reset all CPU/GPU clocks
            if ((down & HidNpadButton_L) && (down & HidNpadButton_R)) {
                Result rc = scale_reset_all();
                if (R_SUCCEEDED(rc)) {
                    save_config();
                    add_alert("Clocks reset to default.");
                } else {
                    snprintf(clock_msg, sizeof(clock_msg), "Clock reset failed (%08X)", (unsigned)rc);
                    add_alert(clock_msg);
                }
            }
            // Blue light filter toggle (Left Stick button)
            if (down & HidNpadButton_StickL) {
                blue_light_filter = (blue_light_filter + 1) % 6;
                if (blue_light_filter > 0)
                    add_alert("Blue light filter: ON");
                else
                    add_alert("Blue light filter: OFF");
            }
        }

        // Settings page: navigate options with Up/Down, change with Left/Right
        if (cur == 8 && !custom_theme_editing) {
            if (down & HidNpadButton_Down) { settings_sel = (settings_sel + 1) % 5; }
            if (down & HidNpadButton_Up) { settings_sel = (settings_sel - 1 + 5) % 5; }
            if (down & HidNpadButton_Left) {
                if (settings_sel == 0) { cur_lang = (cur_lang - 1 + LANG_MAX) % LANG_MAX; load_ui_fonts(); }
                else if (settings_sel == 1) { cur_theme = (cur_theme - 1 + THEME_MAX) % THEME_MAX; apply_theme(); }
                else if (settings_sel == 2) {
                    static const int rv[] = {0,1,3,5,10};
                    int ci = 0;
                    for (int i = 0; i < 5; i++) if (rv[i] == auto_refresh_interval) ci = i;
                    ci = (ci - 1 + 5) % 5;
                    auto_refresh_interval = rv[ci];
                } else if (settings_sel == 3) {
                    // decrease temp alert by 5C
                    if (temp_alert_millic > 30000) temp_alert_millic -= 5000;
                } else if (settings_sel == 4) {
                    // decrease mem alert by 5%
                    if (mem_alert_frac > 0.5f) mem_alert_frac -= 0.05f;
                }
                save_config();
            }
            if (down & HidNpadButton_Right) {
                if (settings_sel == 0) { cur_lang = (cur_lang + 1) % LANG_MAX; load_ui_fonts(); }
                else if (settings_sel == 1) { cur_theme = (cur_theme + 1) % THEME_MAX; apply_theme(); }
                else if (settings_sel == 2) {
                    static const int rv[] = {0,1,3,5,10};
                    int ci = 0;
                    for (int i = 0; i < 5; i++) if (rv[i] == auto_refresh_interval) ci = i;
                    ci = (ci + 1) % 5;
                    auto_refresh_interval = rv[ci];
                } else if (settings_sel == 3) {
                    if (temp_alert_millic < 90000) temp_alert_millic += 5000;
                } else if (settings_sel == 4) {
                    if (mem_alert_frac < 0.95f) mem_alert_frac += 0.05f;
                }
                save_config();
            }
            if (down & HidNpadButton_B) {
                cur = 0; settings_sel = 0; // Back to homepage
            }
        }

        // Custom Theme Editor controls (within Settings page)
        if (cur == 8 && custom_theme_editing) {
            SDL_Color *cte_colors[] = {&custom_bg,&custom_card,&custom_border,&custom_cyan,&custom_green,&custom_white,&custom_grey,&custom_dark_grey,&custom_bg2,&custom_bg3};
            if (down & HidNpadButton_Up) {
                cte_sel = (cte_sel - 1 + 10) % 10;
            }
            if (down & HidNpadButton_Down) {
                cte_sel = (cte_sel + 1) % 10;
            }
            if (down & HidNpadButton_Left) {
                cte_chan = (cte_chan - 1 + 3) % 3;
            }
            if (down & HidNpadButton_Right) {
                cte_chan = (cte_chan + 1) % 3;
            }
            // ZL decrease channel value, ZR increase
            if (down & HidNpadButton_ZL) {
                u8 *v = (cte_chan == 0) ? &cte_colors[cte_sel]->r : (cte_chan == 1) ? &cte_colors[cte_sel]->g : &cte_colors[cte_sel]->b;
                if (*v >= 5) *v -= 5; else *v = 0;
            }
            if (down & HidNpadButton_ZR) {
                u8 *v = (cte_chan == 0) ? &cte_colors[cte_sel]->r : (cte_chan == 1) ? &cte_colors[cte_sel]->g : &cte_colors[cte_sel]->b;
                if (*v <= 250) *v += 5; else *v = 255;
            }
            if (down & HidNpadButton_A) {
                // Save + apply
                custom_theme_editing = 0;
                apply_theme();
                save_config();
            }
            if (down & HidNpadButton_B) {
                // Cancel: restore backups
                custom_theme_editing = 0;
                custom_bg = cte_backup[0]; custom_card = cte_backup[1]; custom_border = cte_backup[2];
                custom_cyan = cte_backup[3]; custom_green = cte_backup[4]; custom_white = cte_backup[5];
                custom_grey = cte_backup[6]; custom_dark_grey = cte_backup[7]; custom_bg2 = cte_backup[8];
                custom_bg3 = cte_backup[9];
                apply_theme();
            }
        }

        // X on homepage opens Settings
        if (cur == 0 && (down & HidNpadButton_X)) cur = 8;

        // Tab navigation (supports L/R and ZL/ZR, blocked only when editing theme or in file browser)
        if (!custom_theme_editing && !(cur == 1 && fb_active)) {
            if (down & (HidNpadButton_L | HidNpadButton_ZL)) cur = (cur - 1 + PGS) % PGS;
            if (down & (HidNpadButton_R | HidNpadButton_ZR)) cur = (cur + 1) % PGS;
        }

        // Load installed titles when the Tools diagnostics are opened.
        if (cur == 6 && !installed_games_scanned) {
            scan_installed_games();
            seed_game_stats();
        }
        if (cur == 6) {
            scan_running_processes();
        }

        // Hold maximum rumble while X/Y held on page 6 or while pulse active from page 5.
        u64 held_buttons = padGetButtons(&pad);
        u64 now_vibe = armGetSystemTick();
        bool vibe_left = (cur == 6 && ((held_buttons & HidNpadButton_X) || touch_vibe_index == 0)) ||
                         (ctrl_rumble_until[0] > 0 && now_vibe < ctrl_rumble_until[0]);
        bool vibe_right = (cur == 6 && ((held_buttons & HidNpadButton_Y) || touch_vibe_index == 1)) ||
                          (ctrl_rumble_until[1] > 0 && now_vibe < ctrl_rumble_until[1]);
        if (ctrl_rumble_until[0] > 0 && now_vibe >= ctrl_rumble_until[0]) ctrl_rumble_until[0] = 0;
        if (ctrl_rumble_until[1] > 0 && now_vibe >= ctrl_rumble_until[1]) ctrl_rumble_until[1] = 0;
        vibration_hold(0, vibe_left);
        vibration_hold(1, vibe_right);

        // Storage page: file browser
        if (cur == 1) {
            if (!fb_active) {
                if (down & HidNpadButton_Y) { fb_open("sdmc:/"); fb_mode = FB_MODE_BROWSE; }
            } else {
                // Minus: exit file browser to homepage
                if (down & HidNpadButton_Minus) {
                    fb_active = 0;
                    cur = 0;
                }

                if (fb_mode == FB_MODE_BROWSE) {
                    if (down & HidNpadButton_A) {
                        if (fb_is_dir[fb_selected]) {
                            char fp[2048];
                            snprintf(fp, sizeof(fp), "%s/%s", fb_path, fb_entries[fb_selected]);
                            int sl = (int)strlen(fp);
                            if (sl > 0 && fp[sl-1] == '/') fp[sl-1] = 0;
                            fb_open(fp);
                            fb_mode = FB_MODE_BROWSE;
                        }
                    }
                    if (down & HidNpadButton_B) {
                        char parent[2048];
                        snprintf(parent, sizeof(parent), "%s", fb_path);
                        char *slash = strrchr(parent, '/');
                        if (slash && slash > parent + 5) {
                            *slash = 0;
                            fb_open(parent);
                        } else {
                            fb_active = 0;
                        }
                    }
                    if (down & HidNpadButton_X) {
                        if (fb_count > 0) fb_mode = FB_MODE_CONFIRM_DELETE;
                    }
                    if (down & HidNpadButton_Y) {
                        if (fb_count > 0) {
                            fb_mode = FB_MODE_RENAME;
                            fb_rename_cursor = 0;
                            strncpy(fb_rename_buf, fb_entries[fb_selected], sizeof(fb_rename_buf)-1);
                            int rl = (int)strlen(fb_rename_buf);
                            if (rl > 0 && fb_rename_buf[rl-1] == '/') fb_rename_buf[rl-1] = 0;
                        }
                    }
                    if (down & HidNpadButton_R) {
                        if (fb_count > 0) fb_copy_selected(0);
                    }
                    if (down & HidNpadButton_ZL) {
                        if (fb_count > 0) fb_copy_selected(1);
                    }
                    if (down & HidNpadButton_L) {
                        fb_paste();
                    }
                    if ((down & HidNpadButton_Down) && fb_selected < fb_count - 1) {
                        fb_selected++;
                        if (fb_selected > fb_scroll + 14) fb_scroll = fb_selected - 14;
                    }
                    if ((down & HidNpadButton_Up) && fb_selected > 0) {
                        fb_selected--;
                        if (fb_selected < fb_scroll) fb_scroll = fb_selected;
                    }
                } else if (fb_mode == FB_MODE_CONFIRM_DELETE) {
                    if (down & HidNpadButton_A) {
                        fb_delete_selected();
                    }
                    if (down & HidNpadButton_B) {
                        fb_mode = FB_MODE_BROWSE;
                    }
                } else if (fb_mode == FB_MODE_RENAME) {
                    int rlen = (int)strlen(fb_rename_buf);
                    if (down & HidNpadButton_A) {
                        if (rlen > 0) fb_rename_selected(fb_rename_buf);
                    }
                    if (down & HidNpadButton_B) {
                        fb_mode = FB_MODE_BROWSE;
                        fb_rename_buf[0] = 0;
                        fb_rename_cursor = 0;
                    }
                    if (down & HidNpadButton_Left) {
                        if (fb_rename_cursor > 0) fb_rename_cursor--;
                    }
                    if (down & HidNpadButton_Right) {
                        if (fb_rename_cursor < rlen) fb_rename_cursor++;
                        // If at end, add a char
                        if (fb_rename_cursor > rlen) {
                            if (rlen < (int)sizeof(fb_rename_buf) - 2) {
                                fb_rename_buf[rlen] = 'a';
                                fb_rename_buf[rlen+1] = 0;
                                fb_rename_cursor = rlen + 1;
                            }
                        }
                    }
                    if (down & HidNpadButton_Up) {
                        if (rlen > 0 && fb_rename_cursor < rlen) {
                            char c = fb_rename_buf[fb_rename_cursor];
                            c++;
                            if (c > 'z') c = 'a';
                            if (c == 'z'+1) c = '0';
                            if (c > '9' && c < 'a') c = 'a';
                            fb_rename_buf[fb_rename_cursor] = c;
                        } else if (rlen < (int)sizeof(fb_rename_buf) - 2) {
                            // Append new char at end
                            fb_rename_buf[rlen] = 'a';
                            fb_rename_buf[rlen+1] = 0;
                            fb_rename_cursor = rlen;
                        }
                    }
                    if (down & HidNpadButton_Down) {
                        if (rlen > 0 && fb_rename_cursor < rlen) {
                            char c = fb_rename_buf[fb_rename_cursor];
                            c--;
                            if (c < '0') c = 'z';
                            if (c < 'a' && c > '9') c = '9';
                            fb_rename_buf[fb_rename_cursor] = c;
                        }
                    }
                    // ZL deletes character at cursor
                    if (down & HidNpadButton_ZL) {
                        if (rlen > 0 && fb_rename_cursor < rlen) {
                            memmove(fb_rename_buf + fb_rename_cursor, fb_rename_buf + fb_rename_cursor + 1, rlen - fb_rename_cursor);
                        }
                    }
                }
            }
        }

        // DPad for brightness (only on Tools page)
        if (cur == 6 && !lbl_emulator) {
            u32 b = down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right);
            if (b) {
                if (ctrl_brightness < 0) {
                    float br = 0.5f;
                    if (R_FAILED(brightness_read(&br))) br = 0.5f;
                    ctrl_brightness = br;
                }
                if (b & (HidNpadButton_Up | HidNpadButton_Right)) {
                    ctrl_brightness += 0.05f;
                    if (ctrl_brightness > 1.0f) ctrl_brightness = 1.0f;
                } else {
                    ctrl_brightness -= 0.05f;
                    if (ctrl_brightness < 0.0f) ctrl_brightness = 0.0f;
                }
                brightness_apply(ctrl_brightness);
            }
        }

        // Scrolling with DPad for pages 0, 1, 2, 4, 5, 6, 7
        if (cur == 2 && net_scroll_max > 0) {
            if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                net_scroll -= 24;
                if (net_scroll < 0) net_scroll = 0;
            }
            if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                net_scroll += 24;
                if (net_scroll > net_scroll_max) net_scroll = net_scroll_max;
            }
        }
        if (cur == 5) {
            if (ctrl_scroll_max > 0) {
                if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                    ctrl_scroll -= 24;
                    if (ctrl_scroll < 0) ctrl_scroll = 0;
                }
                if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                    ctrl_scroll += 24;
                    if (ctrl_scroll > ctrl_scroll_max) ctrl_scroll = ctrl_scroll_max;
                }
            }
            if (down & (HidNpadButton_Minus | HidNpadButton_StickL | HidNpadButton_StickR)) {
                stick_l_peak_drift = 0.0f;
                stick_r_peak_drift = 0.0f;
                add_alert(cur_lang == 1 ? "Pics de derive des sticks reinitialises !" : "Stick drift peak values reset!");
            }
            if (down & HidNpadButton_X) {
                ctrl_trigger_rumble(-1, 300);
                add_alert(cur_lang == 1 ? "Test vibration declenche !" : "Haptic vibration triggered!");
            }
        }
        if (cur == 0 && system_scroll_max > 0) {
            if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                system_scroll -= 24;
                if (system_scroll < 0) system_scroll = 0;
            }
            if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                system_scroll += 24;
                if (system_scroll > system_scroll_max) system_scroll = system_scroll_max;
            }
        }
        if (cur == 4 && perf_scroll_max > 0) {
            if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                perf_scroll -= 24;
                if (perf_scroll < 0) perf_scroll = 0;
            }
            if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                perf_scroll += 24;
                if (perf_scroll > perf_scroll_max) perf_scroll = perf_scroll_max;
            }
        }
        if (cur == 6 && tools_scroll_max > 0) {
            if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                tools_scroll -= 24;
                if (tools_scroll < 0) tools_scroll = 0;
            }
            if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                tools_scroll += 24;
                if (tools_scroll > tools_scroll_max) tools_scroll = tools_scroll_max;
            }
        }
        if (cur == 1 && storage_scroll_max > 0) {
            if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                storage_scroll -= 24;
                if (storage_scroll < 0) storage_scroll = 0;
            }
            if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                storage_scroll += 24;
                if (storage_scroll > storage_scroll_max) storage_scroll = storage_scroll_max;
            }
        }
        if (cur == 7 && about_scroll_max > 0) {
            if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                about_scroll -= 24;
                if (about_scroll < 0) about_scroll = 0;
            }
            if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                about_scroll += 24;
                if (about_scroll > about_scroll_max) about_scroll = about_scroll_max;
            }
        }
        if (cur == 8 && settings_scroll_max > 0) {
            if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                settings_scroll -= 24;
                if (settings_scroll < 0) settings_scroll = 0;
            }
            if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                settings_scroll += 24;
                if (settings_scroll > settings_scroll_max) settings_scroll = settings_scroll_max;
            }
        }
        // Reset scroll when leaving pages
        if (cur != 0) system_scroll = 0;
        if (cur != 2) net_scroll = 0;
        if (cur != 3) transfer_scroll = 0;
        if (cur != 4) perf_scroll = 0;
        if (cur != 5) { ctrl_scroll = 0; stick_l_peak_drift = 0.0f; stick_r_peak_drift = 0.0f; }
        if (cur != 7) about_scroll = 0;
        if (cur != 6) tools_scroll = 0;
        if (cur != 1) storage_scroll = 0;
        if (cur != 8) { settings_scroll = 0; settings_sel = 0; }

        // Force manual refresh with Y (except Tools page)
        if (cur != 6 && (down & HidNpadButton_Y)) {
            refresh_count++;
            // If we're on the Perf page, also export metrics
            if (cur == 4) save_perf_metrics();
        }

        // Auto-refresh (interval from settings, 0 = off)
        u64 now = armGetSystemTick();
        if (auto_refresh_interval > 0) {
            u64 interval_ticks = (u64)auto_refresh_interval * armGetSystemTickFreq();
            if ((now - last_refresh) >= interval_ticks) {
                refresh_count++;
                last_refresh = armGetSystemTick();
            }
        }

        // FPS counter (once per second)
        fps_count++;
        if (now - fps_last_tick >= armGetSystemTickFreq()) {
            current_fps = fps_count;
            fps_count = 0;
            fps_last_tick = now;
        }

        // Network connection uptime tracking
        u32 check_ip = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpAddress(&check_ip)) && check_ip) {
            if (!net_connected_tick) net_connected_tick = now;
        } else {
            net_connected_tick = 0;
        }

        // Render Frame
        if (screen_test_active) {
            SDL_Color colors[] = {{0,0,0,255},{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255},{0,0,0,255}};
            SDL_Color c = colors[screen_test_active % SCREEN_TEST_PHASES];
            SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
            SDL_RenderClear(renderer);
            char label[64];
            snprintf(label, sizeof(label), s_dead_pixel_fmt[cur_lang], s_dead_pixel_pages[cur_lang][screen_test_active % SCREEN_TEST_PHASES]);
            SDL_Color tc = (screen_test_active == 4) ? (SDL_Color){0,0,0,255} : color_white;
            draw_text(renderer, font_md, label, W/2, H-40, tc, 1);
        } else {
            SDL_SetRenderDrawColor(renderer, color_bg.r, color_bg.g, color_bg.b, 255);
            SDL_RenderClear(renderer);

            draw_header(renderer);
            draw_tabs(renderer, cur);
            
            if (cur == 1 && fb_active) {
                draw_file_browser(renderer);
            } else if (cur == 5) {
                draw_pg5(renderer, &pad);
            } else if (pages[cur]) {
                pages[cur](renderer);
            }

            // Floating refresh button (all pages except Tools)
            if (cur != 6) {
                int cx = W - 70 + 25, cy = 148 + 25;
                filledCircleRGBA(renderer, cx, cy, 22, 255,255,255,20);
                circleRGBA(renderer, cx, cy, 22, color_cyan.r, color_cyan.g, color_cyan.b, 180);
                SDL_SetRenderDrawColor(renderer, color_cyan.r, color_cyan.g, color_cyan.b, 255);
                SDL_RenderDrawLine(renderer, cx - 8, cy - 4, cx, cy - 10);
                SDL_RenderDrawLine(renderer, cx, cy - 10, cx + 8, cy - 4);
                circleRGBA(renderer, cx, cy, 10, color_cyan.r, color_cyan.g, color_cyan.b, 200);
            }

            if (!(cur == 1 && fb_active)) draw_footer(renderer, cur);

            // Blue light filter overlay (applied before toasts so toasts show on top)
            if (blue_light_filter > 0) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 255, 140, 0, blue_light_filter * 25); // 25..125 alpha
                SDL_Rect blf_rect = {0, 0, W, H};
                SDL_RenderFillRect(renderer, &blf_rect);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            }

            // Toast notifications (bottom-right)
            // Modern floating toast notification badges (v0.4)
            u64 now_t = armGetSystemTick();
            u64 freq_t = armGetSystemTickFreq();
            int toast_base_y = 120;
            int drawn_toasts = 0;
            for (int ni = notif_count - 1; ni >= 0; ni--) {
                if (notif_ticks[ni] == 0) continue;
                u64 elapsed = now_t - notif_ticks[ni];
                u64 secs = elapsed / freq_t;
                if (secs >= NOTIF_DURATION) { notif_ticks[ni] = 0; continue; }
                float alpha = (secs >= NOTIF_DURATION - 1) ? (NOTIF_DURATION - secs) * 255 : 255;
                int ta = (int)alpha;
                if (ta < 10) { notif_ticks[ni] = 0; continue; }
                int tw = 0;
                TTF_SizeUTF8(font_sm, notif_msgs[ni], &tw, NULL);
                int bw = tw + 36, bh = 32;
                if (bw < 140) bw = 140;
                int tx = W - bw - 24;
                int ty = toast_base_y + drawn_toasts * 38;
                drawn_toasts++;

                // Translucent pill card with cyan border & glow pip
                roundedBoxRGBA(renderer, tx, ty, tx + bw, ty + bh, 8, 20, 22, 28, ta > 230 ? 230 : ta);
                roundedRectangleRGBA(renderer, tx, ty, tx + bw, ty + bh, 8, color_cyan.r, color_cyan.g, color_cyan.b, ta);
                filledCircleRGBA(renderer, tx + 12, ty + bh / 2, 4, color_cyan.r, color_cyan.g, color_cyan.b, ta);

                // Text
                SDL_Color tc = {color_white.r, color_white.g, color_white.b, (u8)ta};
                SDL_Surface *sf = TTF_RenderUTF8_Blended(font_sm, notif_msgs[ni], tc);
                if (sf) {
                    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, sf);
                    if (tex) {
                        SDL_Rect dr = {tx + 24, ty + 5, sf->w, sf->h};
                        SDL_RenderCopy(renderer, tex, NULL, &dr);
                        SDL_DestroyTexture(tex);
                    }
                    SDL_FreeSurface(sf);
                }
            }
            // Reset blend mode
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

            // Popup overlay
            if (popup_active) {
                u64 elapsed_ticks = armGetSystemTick() - popup_start_tick;
                u64 elapsed_sec = elapsed_ticks / armGetSystemTickFreq();
                if (elapsed_sec < 100) {
                    // Semi-transparent overlay
                    SDL_Rect overlay = {0, 0, W, H};
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
                    SDL_RenderFillRect(renderer, &overlay);
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

                    if (popup_type == POPUP_FAN) {
                        int pw = 700, ph = 280, px = (W - pw) / 2, py = (H - ph) / 2;
                        draw_rounded_box(renderer, px, py, pw, ph, 12, color_card);
                        draw_rounded_rect(renderer, px, py, pw, ph, 12, color_orange);
                        // Title
                        draw_text_w(renderer, font_md, s_fan_title[cur_lang], px + pw/2, py + 20, color_orange, 1, pw - 100);
                        // Close X
                        draw_text(renderer, font_sm, "X", px + pw - 30, py + 15, color_grey, 1);
                        // Slider track
                        int sl_y = py + 120, sl_x = px + 80, sl_w = pw - 160;
                        thickLineRGBA(renderer, sl_x, sl_y, sl_x + sl_w, sl_y, 6, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
                        // Slider fill
                        int fill_w = (popup_fan_val - 30) * sl_w / 70;
                        thickLineRGBA(renderer, sl_x, sl_y, sl_x + fill_w, sl_y, 6, color_orange.r, color_orange.g, color_orange.b, 255);
                        // Thumb
                        int thumb_x = sl_x + fill_w;
                        filledCircleRGBA(renderer, thumb_x, sl_y, 14, color_cyan.r, color_cyan.g, color_cyan.b, 255);
                        circleRGBA(renderer, thumb_x, sl_y, 14, color_white.r, color_white.g, color_white.b, 200);
                        // Percentage display
                        char pct_str[16];
                        snprintf(pct_str, sizeof(pct_str), "%d%%", popup_fan_val);
                        draw_text(renderer, font_md, pct_str, px + pw/2, py + 70, color_white, 1);
                        draw_text_w(renderer, font_xs, s_fan_safe_hint[cur_lang],
                            px + pw/2, py + 95, color_grey, 1, pw - 60);
                        if (fan_level_reported) {
                            char feedback[64];
                            snprintf(feedback, sizeof(feedback),
                                s_fan_feedback_fmt[cur_lang], fan_reported_pct);
                            draw_text_w(renderer, font_xs, feedback,
                                px + pw/2, py + 147, color_cyan, 1, pw - 80);
                        }
                        // - and + buttons
                        int btn_y = py + 180, bx = px + pw/2;
                        draw_rounded_box(renderer, bx - 60, btn_y, 50, 40, 6, color_card_border);
                        draw_text(renderer, font_lg, "-", bx - 35, btn_y + 4, color_white, 1);
                        draw_rounded_box(renderer, bx + 10, btn_y, 50, 40, 6, color_card_border);
                        draw_text(renderer, font_lg, "+", bx + 35, btn_y + 4, color_white, 1);
                        // Apply button
                        int ap_y = btn_y + 60, auto_x = px + pw/2 - 135, ap_x = px + pw/2 + 15;
                        draw_rounded_box(renderer, auto_x, ap_y, 120, 40, 6, color_card_border);
                        draw_text_w(renderer, font_xs, s_fan_auto[cur_lang], auto_x + 60, ap_y + 10, color_white, 1, 108);
                        draw_rounded_box(renderer, ap_x, ap_y, 120, 40, 6, color_orange);
                        draw_text_w(renderer, font_xs, s_fan_apply[cur_lang], ap_x + 60, ap_y + 10, color_white, 1, 108);
                    } else {
                        // Standard info popup
                        int pw = 700, ph = 160, px = (W - pw) / 2, py = (H - ph) / 2;
                        draw_rounded_box(renderer, px, py, pw, ph, 12, color_card);
                        draw_rounded_rect(renderer, px, py, pw, ph, 12, color_cyan);
                        draw_text_w(renderer, font_sm, popup_text, px + pw/2, py + 52, color_white, 1, pw - 80);
                        draw_text(renderer, font_sm, s_popup_dismiss[cur_lang], px + pw/2, py + 110, color_grey, 1);
                    }
                } else {
                    popup_active = false;
                    if (popup_type == POPUP_FAN && !fan_manual_active) fan_service_restore();
                }
            }

            // Blue light filter overlay (last thing before present)
            if (blue_light_filter > 0) {
                int bl_alpha = 10 + blue_light_filter * 15; // 25 to 85
                if (bl_alpha > 120) bl_alpha = 120;
                SDL_Rect bl_rect = {0, 0, W, H};
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 255, 140, 50, bl_alpha);
                SDL_RenderFillRect(renderer, &bl_rect);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            }
            if (tesla_guide_active) draw_tesla_guide(renderer);
            if (tutorial_active) draw_tutorial(renderer);
            if (!screen_test_active) {
                bool is_pressing = (virtual_a_click || (held_buttons & HidNpadButton_A));
                int r_outer = is_pressing ? 16 : 20;
                int r_inner = is_pressing ? 11 : 15;
                int pulse_alpha = 30 + (int)(sin((double)armGetSystemTick() / (double)armGetSystemTickFreq() * 4.0) * 15.0);
                if (pulse_alpha < 15) pulse_alpha = 15;
                if (pulse_alpha > 55) pulse_alpha = 55;

                filledCircleRGBA(renderer, ui_cursor_x, ui_cursor_y, r_outer + 4,
                    color_cyan.r, color_cyan.g, color_cyan.b, pulse_alpha);
                circleRGBA(renderer, ui_cursor_x, ui_cursor_y, r_outer,
                    is_pressing ? color_white.r : color_cyan.r,
                    is_pressing ? color_white.g : color_cyan.g,
                    is_pressing ? color_white.b : color_cyan.b, 255);
                circleRGBA(renderer, ui_cursor_x, ui_cursor_y, r_inner,
                    color_white.r, color_white.g, color_white.b, is_pressing ? 255 : 190);
                filledCircleRGBA(renderer, ui_cursor_x, ui_cursor_y, is_pressing ? 5 : 3,
                    is_pressing ? color_yellow.r : color_cyan.r,
                    is_pressing ? color_yellow.g : color_cyan.g,
                    is_pressing ? color_yellow.b : color_cyan.b, 255);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

exit_app:
    fan_service_restore();
    if (ftp_on) ftp_stop();
    if (mtp_on) {
        mtp_server_stop();
        mtp_on = 0;
    }
    if (sixaxis_init_ok) {
        hidStopSixAxisSensor(sixaxis_handles[0]);
        if (sixaxis_handles_count >= 2)
            hidStopSixAxisSensor(sixaxis_handles[1]);
    }
    net_test_cleanup();
    antiban_cleanup();
    storage_scan_cleanup();
    vibration_stop(0);
    vibration_stop(1);
    if (vibe_session_started) hidEndPermitVibrationSession();

    if (font_xs) TTF_CloseFont(font_xs);
    if (font_sm) TTF_CloseFont(font_sm);
    if (font_md) TTF_CloseFont(font_md);
    if (font_lg) TTF_CloseFont(font_lg);
    text_cache_clear();
    for (int i = 0; i < 4; ++i) {
        if (proc_icons[i]) SDL_DestroyTexture(proc_icons[i]);
        proc_icons[i] = NULL;
        proc_icon_cache_ids[i] = 0;
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (ttf_initialized) TTF_Quit();
    if (sdl_initialized) SDL_Quit();

    socketExit();
    plExit();
    clkrstExit();
    nifmExit();
    psmExit();
    setExit();
    setsysExit();
    nsExit();
    splExit();
    if (account_init_ok) accountExit();
    
    return 0;
}
