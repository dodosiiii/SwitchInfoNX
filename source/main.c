#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_dl.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#include "mtp/mtp_wrapper.h"

#define W 1280
#define H 720
#define PGS 9

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

static HidVibrationDeviceHandle vibe_handles[2];
static bool vibe_init_ok = false;

// Export status message
static char export_msg[128] = {0};
static u64 export_msg_tick = 0;
static float sd_speed_result = 0.0f; // MB/s, 0 = not tested, -2 = testing, -1 = error

// FPS history chart
#define FPS_HIST_SIZE 60
static int fps_hist[FPS_HIST_SIZE] = {0};
static int fps_hist_pos = 0;
static int fps_hist_count = 0;

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
#define LANG_MAX 7
static int cur_lang = LANG_EN;

static const char *lang_names[LANG_MAX] = { "English", "Francais", "Italiano", "Espanol", "Deutsch", "Portugues", "Nederlands" };
static const char *tab_names[LANG_MAX][PGS] = {
    { "System", "Storage", "Network", "Transfer", "Perf", "Controller", "Tools", "About", "Settings" },
    { "Systeme", "Stockage", "Reseau", "Transfert", "Perf", "Manette", "Outils", "A propos", "Reglages" },
    { "Sistema", "Archiv.", "Rete", "Trasf.", "Prest.", "Controller", "Strumenti", "Info", "Impost." },
    { "Sistema", "Almac.", "Red", "Transf.", "Rendim.", "Mando", "Herram.", "Acerca", "Ajustes" },
    { "System", "Speicher", "Netzwerk", "Uebertr.", "Leistung", "Controller", "Werkzeuge", "Ueber", "Einstell." },
    { "Sistema", "Armaz.", "Rede", "Transf.", "Desemp.", "Controle", "Ferramentas", "Sobre", "Config." },
    { "Systeem", "Opslag", "Netwerk", "Overdr.", "Prest.", "Controller", "Gereedschap", "Over", "Instell." },
};
static const char *settings_title[LANG_MAX] = {
    "Settings", "Reglages", "Impostazioni", "Ajustes", "Einstellungen", "Configuracoes", "Instellingen"
};
static const char *settings_lang_label[LANG_MAX] = {
    "Language", "Langue", "Lingua", "Idioma", "Sprache", "Idioma", "Taal"
};
static const char *settings_theme_label[LANG_MAX] = {
    "Theme", "Theme", "Tema", "Tema", "Design", "Tema", "Thema"
};
static const char *settings_refresh_label[LANG_MAX] = {
    "Auto Refresh", "Auto Rafraichir", "Aggiorn. Auto", "Auto Actualizar", "Auto Aktual.", "Auto Atualizar", "Auto Ververs."
};
static const char *settings_temp_alert_label[LANG_MAX] = {
    "Temp Alert", "Alerte Temp", "Allerta Temp", "Alerta Temp", "Temp Alarm", "Alerta Temp", "Temp Alarm"
};
static const char *settings_mem_alert_label[LANG_MAX] = {
    "Memory Alert", "Alerte Memoire", "Allerta Memoria", "Alerta Memoria", "Speicher Alarm", "Alerta Memoria", "Geheugen Alarm"
};
static const char *settings_info[LANG_MAX] = {
    "Press B to return   |   D-Pad Up/Down to change",
    "Appuyez sur B pour revenir   |   D-Pad Haut/Bas pour changer",
    "Premi B per tornare   |   D-Pad Su/Giu per cambiare",
    "Presione B para volver   |   D-Pad Arriba/Abajo para cambiar",
    "Druecke B zum Zurueck   |   D-Pad Hoch/Runter zum Aendern",
    "Pressione B para voltar   |   D-Pad Cima/Baixo para mudar",
    "Druk B om terug te keren   |   D-Pad Omhoog/Omlaag om te wijzigen"
};

// ─── Comprehensive i18n string table ────────────────────────

// --- Page 0: System ---
static const char *s_fw_hw[LANG_MAX] = {
    "Firmware & Hardware", "Firmware & Materiel", "Firmware e Hardware", "Firmware y Hardware",
    "Firmware & Hardware", "Firmware e Hardware", "Firmware & Hardware"
};
static const char *s_firmware[LANG_MAX] = {
    "Firmware", "Firmware", "Firmware", "Firmware", "Firmware", "Firmware", "Firmware"
};
static const char *s_serial[LANG_MAX] = {
    "Serial No.", "No. Serie", "N. Seriale", "No. Serie",
    "Serien-Nr.", "N. Serie", "Serie Nr."
};
static const char *s_hardware[LANG_MAX] = {
    "Hardware", "Materiel", "Hardware", "Hardware",
    "Hardware", "Hardware", "Hardware"
};
static const char *s_mode[LANG_MAX] = {
    "Mode", "Mode", "Modalita", "Modo",
    "Modus", "Modo", "Modus"
};
static const char *s_docked[LANG_MAX] = {
    "Docked (TV Output)", "Dock (Sortie TV)", "Dock (Uscita TV)", "Acoplado (Salida TV)",
    "Docked (TV-Ausgang)", "Dock (Saida TV)", "Gedockt (TV-uitgang)"
};
static const char *s_handheld[LANG_MAX] = {
    "Handheld (Portable)", "Portable", "Portatile", "Portatil",
    "Handheld (Tragbar)", "Portatil", "Handheld (Draagbaar)"
};
static const char *s_arch[LANG_MAX] = {
    "Architecture", "Architecture", "Architettura", "Arquitectura",
    "Architektur", "Arquitetura", "Architectuur"
};
static const char *s_arch_val[LANG_MAX] = {
    "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)",
    "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)", "ARMv8-A (4x Cortex-A57)"
};
static const char *s_dev_name[LANG_MAX] = {
    "Device Name", "Nom de l'appareil", "Nome dispositivo", "Nombre del dispositivo",
    "Geraetename", "Nome do dispositivo", "Apparaatnaam"
};
static const char *s_region[LANG_MAX] = {
    "Region", "Region", "Regione", "Region",
    "Region", "Regiao", "Regio"
};
static const char *s_sys_lang[LANG_MAX] = {
    "System Language", "Langue du systeme", "Lingua di sistema", "Idioma del sistema",
    "Systemsprache", "Idioma do sistema", "Systeemtaal"
};
static const char *s_batt_power[LANG_MAX] = {
    "Battery & Power", "Batterie & Alimentation", "Batteria & Alimentazione", "Bateria y Alimentacion",
    "Akku & Strom", "Bateria & Energia", "Batterij & Stroom"
};
static const char *s_batt_level[LANG_MAX] = {
    "Battery Level", "Niveau batterie", "Livello batteria", "Nivel de bateria",
    "Akku-Stand", "Nivel da bateria", "Batterijniveau"
};
static const char *s_charging_label[LANG_MAX] = {
    "Charging", "Charge", "Carica", "Carga",
    "Ladevorgang", "Carregando", "Opladen"
};
static const char *s_charging[LANG_MAX] = {
    "Charging", "En charge", "In carica", "Cargando",
    "Laden", "Carregando", "Opladen"
};
static const char *s_discharging[LANG_MAX] = {
    "Discharging", "Decharge", "In scarica", "Descargando",
    "Entladen", "Descarregando", "Ontladen"
};
static const char *s_charger[LANG_MAX] = {
    "Charger", "Chargeur", "Caricatore", "Cargador",
    "Ladegeraet", "Carregador", "Lader"
};
static const char *s_ac_adapter[LANG_MAX] = {
    "AC Adapter (USB-PD)", "Adaptateur secteur (USB-PD)", "Adattatore AC (USB-PD)", "Adaptador CA (USB-PD)",
    "Netzteil (USB-PD)", "Adaptador AC (USB-PD)", "Netadapter (USB-PD)"
};
static const char *s_usb_slow[LANG_MAX] = {
    "USB Power (Slow)", "USB (Lent)", "USB (Lento)", "USB (Lento)",
    "USB (Langsam)", "USB (Lento)", "USB (Langzaam)"
};
static const char *s_none[LANG_MAX] = {
    "None", "Aucun", "Nessuno", "Ninguno",
    "Keiner", "Nenhum", "Geen"
};
static const char *s_capacity[LANG_MAX] = {
    "Capacity", "Capacite", "Capacita", "Capacidad",
    "Kapazitaet", "Capacidade", "Capaciteit"
};
static const char *s_jc_l[LANG_MAX] = {
    "Joy-Con L", "Joy-Con G", "Joy-Con S", "Joy-Con I",
    "Joy-Con L", "Joy-Con E", "Joy-Con L"
};
static const char *s_jc_r[LANG_MAX] = {
    "Joy-Con R", "Joy-Con D", "Joy-Con D", "Joy-Con D",
    "Joy-Con R", "Joy-Con D", "Joy-Con R"
};
static const char *s_thermals[LANG_MAX] = {
    "Thermals & Display", "Thermique & Ecran", "Termiche & Display", "Termica y Pantalla",
    "Thermik & Display", "Termica & Ecra", "Thermiek & Scherm"
};
static const char *s_skin_temp[LANG_MAX] = {
    "Skin Temp", "Temp. boitier", "Temp. scocca", "Temp. carcasa",
    "Gehaeuse-Temp.", "Temp. do corpo", "Behuizing temp."
};
static const char *s_thermal_state[LANG_MAX] = {
    "Thermal State", "Etat thermique", "Stato termico", "Estado termico",
    "Thermischer Status", "Estado termico", "Thermische status"
};
static const char *s_normal[LANG_MAX] = {
    "Normal", "Normal", "Normale", "Normal",
    "Normal", "Normal", "Normaal"
};
static const char *s_hot[LANG_MAX] = {
    "HOT!", "CHAUDE!", "CALDO!", "CALIENTE!",
    "HEISS!", "QUENTE!", "HEET!"
};
static const char *s_warm[LANG_MAX] = {
    "Warm", "Tiede", "Caldo", "Templado",
    "Warm", "Morno", "Warm"
};
static const char *s_brightness[LANG_MAX] = {
    "Brightness", "Luminosite", "Luminosita", "Brillo",
    "Helligkeit", "Brilho", "Helderheid"
};
static const char *s_na_emu[LANG_MAX] = {
    "N/A (emulator)", "N/D (emulateur)", "N/D (emulatore)", "N/D (emulador)",
    "N/V (Emulator)", "N/D (emulador)", "N/B (emulator)"
};
static const char *s_resolution[LANG_MAX] = {
    "Resolution", "Resolution", "Risoluzione", "Resolucion",
    "Aufloesung", "Resolucao", "Resolutie"
};
static const char *s_res_val[LANG_MAX] = {
    "1280x720 (720p)", "1280x720 (720p)", "1280x720 (720p)", "1280x720 (720p)",
    "1280x720 (720p)", "1280x720 (720p)", "1280x720 (720p)"
};
static const char *s_refresh_rate[LANG_MAX] = {
    "Refresh Rate", "Taux de rafraichissement", "Frequenza aggiornamento", "Frecuencia actualizacion",
    "Bildwiederholrate", "Taxa de atualizacao", "Verversingssnelheid"
};
static const char *s_sys_uptime[LANG_MAX] = {
    "System Uptime", "Temps de fonctionnement", "Tempo di accensione", "Tiempo de actividad",
    "System-Betriebszeit", "Tempo de atividade", "Systeem-uptime"
};
static const char *s_app_uptime[LANG_MAX] = {
    "App Uptime", "Temps d'ouverture", "Tempo apertura app", "Tiempo de ejecucion",
    "App-Laufzeit", "Tempo de execucao", "App-uptime"
};
static const char *s_failed_read_clocks[LANG_MAX] = {
    "Failed to read system clocks.", "Echec de lecture des horloges.", "Lettura orologi fallita.", "Fallo al leer relojes.",
    "Fehler beim Lesen der Takte.", "Falha ao ler clocks.", "Kan systeemklokken niet lezen."
};
static const char *s_country_names[LANG_MAX][6] = {
    {"Japan","Americas","Europe","Australia/NZ","HK/TW/KR","China"},
    {"Japon","Ameriques","Europe","Australie/NZ","HK/TW/KR","Chine"},
    {"Giappone","Americhe","Europa","Australia/NZ","HK/TW/KR","Cina"},
    {"Japon","Americas","Europa","Australia/NZ","HK/TW/KR","China"},
    {"Japan","Amerika","Europa","Australien/NZ","HK/TW/KR","China"},
    {"Japao","Americas","Europa","Australia/NZ","HK/TW/KR","China"},
    {"Japan","Amerika","Europa","Australie/NZ","HK/TW/KR","China"}
};
static const char *s_lang_names_18[LANG_MAX][18] = {
    {"Japanese","English US","French","German","Italian","Spanish","Chinese","Korean","Dutch","Portuguese","Russian","Chinese TW","English UK","French CA","Spanish LA","Chinese Hans","Chinese Hant","Brazilian PT"},
    {"Japonais","Anglais US","Francais","Allemand","Italien","Espagnol","Chinois","Coreen","Neerlandais","Portugais","Russe","Chinois TW","Anglais UK","Francais CA","Espagnol LA","Chinois Hans","Chinois Hant","Portugais BR"},
    {"Giapponese","Inglese US","Francese","Tedesco","Italiano","Spagnolo","Cinese","Coreano","Olandese","Portoghese","Russo","Cinese TW","Inglese UK","Francese CA","Spagnolo LA","Cinese Hans","Cinese Hant","Portoghese BR"},
    {"Japones","Ingles EU","Frances","Aleman","Italiano","Espanol","Chino","Coreano","Neerlandes","Portugues","Ruso","Chino TW","Ingles RU","Frances CA","Espanol LA","Chino Hans","Chino Hant","Portugues BR"},
    {"Japanisch","Englisch US","Franzoesisch","Deutsch","Italienisch","Spanisch","Chinesisch","Koreanisch","Niederlaendisch","Portugiesisch","Russisch","Chinesisch TW","Englisch UK","Franzoesisch CA","Spanisch LA","Chinesisch Hans","Chinesisch Hant","Portugiesisch BR"},
    {"Japones","Ingles EU","Frances","Alemao","Italiano","Espanhol","Chines","Coreano","Holandes","Portugues","Russo","Chines TW","Ingles UK","Frances CA","Espanhol LA","Chines Hans","Chines Hant","Portugues BR"},
    {"Japans","Engels US","Frans","Duits","Italiaans","Spaans","Chinees","Koreaans","Nederlands","Portugees","Russisch","Chinees TW","Engels UK","Frans CA","Spaans LA","Chinees Hans","Chinees Hant","Portugees BR"}
};

// --- Page 1: Storage ---
static const char *s_sd_card[LANG_MAX] = {
    "SD Card (sdmc:/)", "Carte SD (sdmc:/)", "SD Card (sdmc:/)", "Tarjeta SD (sdmc:/)",
    "SD-Karte (sdmc:/)", "Cartao SD (sdmc:/)", "SD-kaart (sdmc:/)"
};
static const char *s_total[LANG_MAX] = {
    "Total", "Total", "Totale", "Total",
    "Gesamt", "Total", "Totaal"
};
static const char *s_used[LANG_MAX] = {
    "Used", "Utilise", "Usato", "Usado",
    "Belegt", "Usado", "Gebruikt"
};
static const char *s_free[LANG_MAX] = {
    "Free", "Libre", "Libero", "Libre",
    "Frei", "Livre", "Vrij"
};
static const char *s_storage_usage[LANG_MAX] = {
    "Storage Usage", "Utilisation stockage", "Utilizzo archivio", "Uso de almacenamiento",
    "Speichernutzung", "Uso do armazenamento", "Opslaggebruik"
};
static const char *s_root_fmt[LANG_MAX] = {
    "Root: %d folders, %d files", "Racine: %d dossiers, %d fichiers", "Radice: %d cartelle, %d file", "Raiz: %d carpetas, %d archivos",
    "Root: %d Ordner, %d Dateien", "Raiz: %d pastas, %d arquivos", "Root: %d mappen, %d bestanden"
};
static const char *s_sd_fail[LANG_MAX] = {
    "Failed to read SD card info", "Echec de lecture de la carte SD", "Lettura SD fallita", "Fallo al leer tarjeta SD",
    "SD-Karteninfo konnte nicht gelesen werden", "Falha ao ler cartao SD", "Kan SD-kaartinfo niet lezen"
};
static const char *s_read_speed_fmt[LANG_MAX] = {
    "Read Speed: %.1f MB/s", "Vitesse: %.1f Mo/s", "Velocita: %.1f MB/s", "Velocidad: %.1f MB/s",
    "Lesegeschw.: %.1f MB/s", "Velocidade: %.1f MB/s", "Leessnelheid: %.1f MB/s"
};
static const char *s_read_testing[LANG_MAX] = {
    "Read Speed: Testing...", "Vitesse: Test...", "Velocita: Test...", "Velocidad: Probando...",
    "Lesegeschw.: Test...", "Velocidade: Testando...", "Leessnelheid: Testen..."
};
static const char *s_read_error[LANG_MAX] = {
    "Read Speed: Error", "Vitesse: Erreur", "Velocita: Errore", "Velocidad: Error",
    "Lesegeschw.: Fehler", "Velocidade: Erro", "Leessnelheid: Fout"
};
static const char *s_read_untested[LANG_MAX] = {
    "Read Speed: Not tested", "Vitesse: Non testee", "Velocita: Non testata", "Velocidad: No probada",
    "Lesegeschw.: Nicht getestet", "Velocidade: Nao testado", "Leessnelheid: Niet getest"
};
static const char *s_sd_no_mount[LANG_MAX] = {
    "SD Card not inserted or failed to mount", "Carte SD non inseree ou echec montage", "SD non inserita o mount fallito", "Tarjeta SD no insertada o fallo al montar",
    "SD-Karte nicht eingelegt oder Mount fehlgeschlagen", "Cartao SD nao inserido ou falha ao montar", "SD-kaart niet geplaatst of mount mislukt"
};
static const char *s_nand_parts[LANG_MAX] = {
    "NAND Partitions (Internal)", "Partitions NAND (Interne)", "Partizioni NAND (Interne)", "Particiones NAND (Interna)",
    "NAND-Partitionen (Intern)", "Particoes NAND (Interno)", "NAND-partities (Intern)"
};
static const char *s_sys_part[LANG_MAX] = {
    "System Partition", "Partition Systeme", "Partizione di Sistema", "Particion del Sistema",
    "Systempartition", "Particao do Sistema", "Systeempartitie"
};
static const char *s_user_part[LANG_MAX] = {
    "User Partition", "Partition Utilisateur", "Partizione Utente", "Particion de Usuario",
    "Benutzerpartition", "Particao do Usuario", "Gebruikerspartitie"
};
static const char *s_sd_breakdown[LANG_MAX] = {
    "SD Card Content Breakdown", "Contenu de la carte SD", "Contenuto SD", "Contenido de la tarjeta SD",
    "SD-Karteninhalt", "Conteudo do cartao SD", "SD-kaartinhoud"
};
static const char *s_touch_test[LANG_MAX] = {
    "[Touch] Test Speed", "[Touch] Test Vitesse", "[Touch] Test Velocita", "[Touch] Probar Velocidad",
    "[Touch] Geschw. testen", "[Touch] Testar Velocidade", "[Touch] Snelheid testen"
};
static const char *s_open_fb[LANG_MAX] = {
    "[Y] Open File Browser", "[Y] Explorateur", "[Y] Esplora file", "[Y] Explorar archivos",
    "[Y] Dateibrowser", "[Y] Explorar arquivos", "[Y] Bestandsverkenner"
};
static const char *s_empty_dir[LANG_MAX] = {
    "(empty directory)", "(dossier vide)", "(cartella vuota)", "(directorio vacio)",
    "(leeres Verzeichnis)", "(diretorio vazio)", "(lege map)"
};
static const char *s_folders[LANG_MAX] = {
    "Folders: %d", "Dossiers: %d", "Cartelle: %d", "Carpetas: %d",
    "Ordner: %d", "Pastas: %d", "Mappen: %d"
};
static const char *s_nro_count[LANG_MAX] = {
    "NRO: %d", "NRO: %d", "NRO: %d", "NRO: %d",
    "NRO: %d", "NRO: %d", "NRO: %d"
};
static const char *s_images[LANG_MAX] = {
    "Images: %d", "Images: %d", "Immagini: %d", "Imagenes: %d",
    "Bilder: %d", "Imagens: %d", "Afbeeldingen: %d"
};
static const char *s_videos[LANG_MAX] = {
    "Videos: %d", "Videos: %d", "Video: %d", "Videos: %d",
    "Videos: %d", "Videos: %d", "Videos: %d"
};
static const char *s_music[LANG_MAX] = {
    "Music: %d", "Musique: %d", "Musica: %d", "Musica: %d",
    "Musik: %d", "Musica: %d", "Muziek: %d"
};
static const char *s_games[LANG_MAX] = {
    "Games: %d", "Jeux: %d", "Giochi: %d", "Juegos: %d",
    "Spiele: %d", "Jogos: %d", "Spelletjes: %d"
};
static const char *s_docs[LANG_MAX] = {
    "Docs: %d", "Documents: %d", "Documenti: %d", "Documentos: %d",
    "Dokumente: %d", "Documentos: %d", "Documenten: %d"
};
static const char *s_other[LANG_MAX] = {
    "Other: %d", "Autres: %d", "Altri: %d", "Otros: %d",
    "Andere: %d", "Outros: %d", "Anders: %d"
};
static const char *s_homebrew_count[LANG_MAX] = {
    "Homebrew Apps: %d", "Apps Homebrew: %d", "App Homebrew: %d", "Apps Homebrew: %d",
    "Homebrew-Apps: %d", "Apps Homebrew: %d", "Homebrew-apps: %d"
};

// --- Page 2: Network ---
static const char *s_ip_config[LANG_MAX] = {
    "IP Configuration", "Configuration IP", "Configurazione IP", "Configuracion IP",
    "IP-Konfiguration", "Configuracao IP", "IP-configuratie"
};
static const char *s_ip_addr[LANG_MAX] = {
    "IP Address", "Adresse IP", "Indirizzo IP", "Direccion IP",
    "IP-Adresse", "Endereco IP", "IP-adres"
};
static const char *s_subnet[LANG_MAX] = {
    "Subnet Mask", "Masque sous-reseau", "Maschera di rete", "Mascara de subred",
    "Subnetzmaske", "Mascara de rede", "Subnetmasker"
};
static const char *s_gateway[LANG_MAX] = {
    "Gateway", "Passerelle", "Gateway", "Puerta de enlace",
    "Gateway", "Gateway", "Gateway"
};
static const char *s_primary_dns[LANG_MAX] = {
    "Primary DNS", "DNS Primaire", "DNS Primario", "DNS Primario",
    "Primaer-DNS", "DNS Primario", "Primaire DNS"
};
static const char *s_secondary_dns[LANG_MAX] = {
    "Secondary DNS", "DNS Secondaire", "DNS Secondario", "DNS Secundario",
    "Sekundaer-DNS", "DNS Secundario", "Secundaire DNS"
};
static const char *s_hostname[LANG_MAX] = {
    "Hostname", "Nom d'hote", "Hostname", "Nombre de host",
    "Hostname", "Hostname", "Hostnaam"
};
static const char *s_mac_addr[LANG_MAX] = {
    "MAC Address", "Adresse MAC", "Indirizzo MAC", "Direccion MAC",
    "MAC-Adresse", "Endereco MAC", "MAC-adres"
};
static const char *s_adapter[LANG_MAX] = {
    "Adapter", "Adaptateur", "Adattatore", "Adaptador",
    "Adapter", "Adaptador", "Adapter"
};
static const char *s_conn_details[LANG_MAX] = {
    "Connection Details", "Details de connexion", "Dettagli connessione", "Detalles de conexion",
    "Verbindungsdetails", "Detalhes da conexao", "Verbindingsdetails"
};
static const char *s_interface[LANG_MAX] = {
    "Interface", "Interface", "Interfaccia", "Interfaz",
    "Schnittstelle", "Interface", "Interface"
};
static const char *s_wifi_wireless[LANG_MAX] = {
    "Wi-Fi (Wireless)", "Wi-Fi (Sans fil)", "Wi-Fi (Wireless)", "Wi-Fi (Inalambrico)",
    "WLAN (Kabellos)", "Wi-Fi (Sem fio)", "Wi-Fi (Draadloos)"
};
static const char *s_ethernet_wired[LANG_MAX] = {
    "Ethernet (Wired)", "Ethernet (Filaire)", "Ethernet (Cavo)", "Ethernet (Cableado)",
    "Ethernet (Kabel)", "Ethernet (Cabo)", "Ethernet (Bekabeld)"
};
static const char *s_internet[LANG_MAX] = {
    "Internet", "Internet", "Internet", "Internet",
    "Internet", "Internet", "Internet"
};
static const char *s_connected[LANG_MAX] = {
    "Connected", "Connecte", "Connesso", "Conectado",
    "Verbunden", "Conectado", "Verbonden"
};
static const char *s_limited[LANG_MAX] = {
    "Limited", "Limite", "Limitato", "Limitado",
    "Eingeschraenkt", "Limitado", "Beperkt"
};
static const char *s_signal_quality[LANG_MAX] = {
    "Signal Quality", "Qualite du signal", "Qualita segnale", "Calidad de senhal",
    "Signalqualitaet", "Qualidade do sinal", "Signaalkwaliteit"
};
static const char *s_link[LANG_MAX] = {
    "Link", "Lien", "Collegamento", "Enlace",
    "Verbindung", "Link", "Verbinding"
};
static const char *s_wired_stable[LANG_MAX] = {
    "Wired (Stable)", "Filaire (Stable)", "Cavo (Stabile)", "Cableado (Estable)",
    "Kabel (Stabil)", "Cabo (Estavel)", "Bekabeld (Stabiel)"
};
static const char *s_connected_for[LANG_MAX] = {
    "Connected For", "Connecte depuis", "Connesso da", "Conectado desde",
    "Verbunden seit", "Conectado ha", "Verbonden sinds"
};
static const char *s_band[LANG_MAX] = {
    "Band", "Bande", "Banda", "Banda",
    "Band", "Banda", "Band"
};
static const char *s_status[LANG_MAX] = {
    "Status", "Statut", "Stato", "Estado",
    "Status", "Estado", "Status"
};
static const char *s_full_internet[LANG_MAX] = {
    "Full Internet Access", "Acces Internet complet", "Accesso Internet completo", "Acceso completo a Internet",
    "Voller Internetzugriff", "Acesso total a Internet", "Volledige internettoegang"
};
static const char *s_local_only[LANG_MAX] = {
    "Local Network Only", "Reseau local seulement", "Solo rete locale", "Solo red local",
    "Nur lokales Netzwerk", "Apenas rede local", "Alleen lokaal netwerk"
};
static const char *s_no_conn[LANG_MAX] = {
    "No Connection", "Pas de connexion", "Nessuna connessione", "Sin conexion",
    "Keine Verbindung", "Sem conexao", "Geen verbinding"
};
static const char *s_wifi_diag[LANG_MAX] = {
    "Wi-Fi Signal Diagnostics", "Diagnostic signal Wi-Fi", "Diagnostica segnale Wi-Fi", "Diagnostico de senhal Wi-Fi",
    "WLAN-Signal-Diagnose", "Diagnostico de sinal Wi-Fi", "Wi-Fi-signaal diagnostiek"
};
static const char *s_ssid[LANG_MAX] = {
    "SSID", "SSID", "SSID", "SSID",
    "SSID", "SSID", "SSID"
};
static const char *s_connected_label[LANG_MAX] = {
    "(connected)", "(connecte)", "(connesso)", "(conectado)",
    "(verbunden)", "(conectado)", "(verbonden)"
};
static const char *s_rssi[LANG_MAX] = {
    "RSSI", "RSSI", "RSSI", "RSSI",
    "RSSI", "RSSI", "RSSI"
};
static const char *s_link_quality[LANG_MAX] = {
    "Link Quality", "Qualite du lien", "Qualita collegamento", "Calidad de enlace",
    "Verbindungsqualitaet", "Qualidade do link", "Linkkwaliteit"
};
static const char *s_signal_power[LANG_MAX] = {
    "Signal Power", "Puissance du signal", "Potenza segnale", "Potencia de senhal",
    "Signalstaerke", "Potencia do sinal", "Signaalsterkte"
};
static const char *s_signal_history[LANG_MAX] = {
    "Signal History (60s):", "Historique signal (60s):", "Cronologia segnale (60s):", "Historial de senhal (60s):",
    "Signalverlauf (60s):", "Historico de sinal (60s):", "Signaalgeschiedenis (60s):"
};
static const char *s_not_connected[LANG_MAX] = {
    "Not connected to any network.", "Non connecte a un reseau.", "Non connesso a nessuna rete.", "No conectado a ninguna red.",
    "Mit keinem Netzwerk verbunden.", "Nao conectado a nenhuma rede.", "Niet verbonden met een netwerk."
};
static const char *s_go_to_settings[LANG_MAX] = {
    "Go to Switch System Settings to connect.", "Allez dans Parametres Switch pour vous connecter.", "Vai su Impostazioni Switch per connetterti.", "Ve a Configuracion del Switch para conectarte.",
    "Gehe zu den Switch-Systemeinstellungen.", "Vai para Configuracoes do Switch para conectar.", "Ga naar Switch-systeeminstellingen om te verbinden."
};
static const char *s_supports_both[LANG_MAX] = {
    "Supports both Wi-Fi and USB Ethernet.", "Prend en charge Wi-Fi et Ethernet USB.", "Supporta Wi-Fi ed Ethernet USB.", "Compatible con Wi-Fi y Ethernet USB.",
    "Unterstuetzt WLAN und USB-Ethernet.", "Suporta Wi-Fi e Ethernet USB.", "Ondersteunt zowel Wi-Fi als USB Ethernet."
};
static const char *s_wifi_not_active[LANG_MAX] = {
    "Wi-Fi not active or not connected.", "Wi-Fi inactif ou non connecte.", "Wi-Fi non attivo o non connesso.", "Wi-Fi inactivo o no conectado.",
    "WLAN nicht aktiv oder nicht verbunden.", "Wi-Fi inativo ou nao conectado.", "Wi-Fi niet actief of niet verbonden."
};
static const char *s_wlan_na[LANG_MAX] = {
    "WLAN diagnostics not available.", "Diagnostic WLAN indisponible.", "Diagnostica WLAN non disponibile.", "Diagnostico WLAN no disponible.",
    "WLAN-Diagnose nicht verfuegbar.", "Diagnostico WLAN indisponivel.", "WLAN-diagnostiek niet beschikbaar."
};

// --- Page 3: Transfer ---
static const char *s_wifi_transfer[LANG_MAX] = {
    "WiFi %s Transfer", "Transfert WiFi %s", "Trasferimento WiFi %s", "Transferencia WiFi %s",
    "WiFi-%s-Uebertragung", "Transferencia WiFi %s", "WiFi %s-overdracht"
};
static const char *s_running[LANG_MAX] = {
    "RUNNING", "ACTIF", "ATTIVO", "ACTIVO",
    "AKTIV", "ATIVO", "ACTIEF"
};
static const char *s_stopped[LANG_MAX] = {
    "STOPPED", "ARRETE", "FERMO", "DETENIDO",
    "GESTOPPT", "PARADO", "GESTOPT"
};
static const char *s_address[LANG_MAX] = {
    "Address", "Adresse", "Indirizzo", "Direccion",
    "Adresse", "Endereco", "Adres"
};
static const char *s_current_dir[LANG_MAX] = {
    "Current Dir", "Repertoire courant", "Cartella corrente", "Directorio actual",
    "Aktuelles Verz.", "Diretorio atual", "Huidige map"
};
static const char *s_transfers[LANG_MAX] = {
    "Transfers", "Transferts", "Trasferimenti", "Transferencias",
    "Uebertragungen", "Transferencias", "Overdrachten"
};
static const char *s_data_total[LANG_MAX] = {
    "Data Total", "Total donnees", "Dati totali", "Datos totales",
    "Daten gesamt", "Total de dados", "Gegevens totaal"
};
static const char *s_toggle_wifi[LANG_MAX] = {
    "[A] Toggle WiFi  |  Touch FTP/FTPD", "[A] WiFi ON/OFF  |  Toucher FTP/FTPD", "[A] WiFi ON/OFF  |  Tocca FTP/FTPD", "[A] WiFi ON/OFF  |  Tocar FTP/FTPD",
    "[A] WiFi ein/aus  |  FTP/FTPD beruehren", "[A] Alternar WiFi  |  Tocar FTP/FTPD", "[A] WiFi aan/uit  |  Raak FTP/FTPD"
};
static const char *s_mtp_title[LANG_MAX] = {
    "MTP File Transfer", "Transfert MTP", "Trasferimento MTP", "Transferencia MTP",
    "MTP-Dateiuebertragung", "Transferencia MTP", "MTP-bestandsoverdracht"
};
static const char *s_active[LANG_MAX] = {
    "ACTIVE", "ACTIF", "ATTIVO", "ACTIVO",
    "AKTIV", "ATIVO", "ACTIEF"
};
static const char *s_protocol[LANG_MAX] = {
    "Protocol", "Protocole", "Protocollo", "Protocolo",
    "Protokoll", "Protocolo", "Protocol"
};
static const char *s_mtp_protocol[LANG_MAX] = {
    "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)",
    "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)", "MTP (Media Transfer Protocol)"
};
static const char *s_mtp_xfers_fmt[LANG_MAX] = {
    "%llu transfers", "%llu transferts", "%llu trasferimenti", "%llu transferencias",
    "%llu Uebertragungen", "%llu transferencias", "%llu overdrachten"
};
static const char *s_mtp_desc[LANG_MAX] = {
    "Appears as MTP device on PC (no driver needed)", "Apparait comme peripherique MTP sur PC (pas de pilote)", "Appare come dispositivo MTP su PC (driver non richiesto)", "Aparece como dispositivo MTP en PC (sin controlador)",
    "Erscheint als MTP-Geraet am PC (kein Treiber noetig)", "Aparece como dispositivo MTP no PC (sem driver)", "Verschijnt als MTP-apparaat op pc (geen stuurprogramma nodig)"
};
static const char *s_toggle_mtp[LANG_MAX] = {
    "[X] Toggle MTP  |  Touch badge", "[X] MTP ON/OFF  |  Toucher badge", "[X] MTP ON/OFF  |  Tocca badge", "[X] MTP ON/OFF  |  Tocar indicador",
    "[X] MTP ein/aus  |  Symbol beruehren", "[X] Alternar MTP  |  Tocar indicador", "[X] MTP aan/uit  |  Raak badge"
};
static const char *s_activity_log[LANG_MAX] = {
    "Activity Log", "Journal d'activite", "Registro attivita", "Registro de actividad",
    "Aktivitaetsprotokoll", "Registro de atividade", "Activiteitenlogboek"
};
static const char *s_no_activity[LANG_MAX] = {
    "No activity. Press [A] or touch badge to start.", "Aucune activite. Appuyez sur [A] ou touchez le badge.", "Nessuna attivita. Premi [A] o tocca il badge.", "Sin actividad. Presione [A] o toque el indicador.",
    "Keine Aktivitaet. [A] druecken oder Symbol beruehren.", "Sem atividade. Pressione [A] ou toque no indicador.", "Geen activiteit. Druk [A] of raak badge aan."
};

// --- Page 4: Performance ---
static const char *s_live_clocks[LANG_MAX] = {
    "Live Clock Frequencies", "Frequences en direct", "Frequenze in tempo reale", "Frecuencias en vivo",
    "Live-Taktfrequenzen", "Frequencias ao vivo", "Live klokfrequenties"
};
static const char *s_cpu_clock[LANG_MAX] = {
    "CPU Core Clock", "Horloge CPU", "Clock CPU", "Reloj CPU",
    "CPU-Takt", "Clock CPU", "CPU-klok"
};
static const char *s_gpu_clock[LANG_MAX] = {
    "GPU Core Clock", "Horloge GPU", "Clock GPU", "Reloj GPU",
    "GPU-Takt", "Clock GPU", "GPU-klok"
};
static const char *s_mem_bus[LANG_MAX] = {
    "Memory Bus Clock", "Horloge bus memoire", "Clock bus memoria", "Reloj bus memoria",
    "Speicherbus-Takt", "Clock barramento memoria", "Geheugenbusklok"
};
static const char *s_thermal_status[LANG_MAX] = {
    "Thermal Status", "Etat thermique", "Stato termico", "Estado termico",
    "Thermischer Status", "Estado termico", "Thermische status"
};
static const char *s_core_skin_temp[LANG_MAX] __attribute__((unused)) = {
    "Core Skin Temp", "Temp. boitier coeur", "Temp. scocca core", "Temp. carcasa nucleo",
    "Kern-Gehaeuse-Temp.", "Temp. corpo nucleo", "Kern-behuizing temp."
};
static const char *s_heat_index[LANG_MAX] __attribute__((unused)) = {
    "Heat Index", "Indice de chaleur", "Indice di calore", "Indice de calor",
    "Hitzeindex", "Indice de calor", "Hitte-index"
};
static const char *s_perf_profile[LANG_MAX] __attribute__((unused)) = {
    "Performance Profile", "Profil de performances", "Profilo prestazioni", "Perfil de rendimiento",
    "Leistungsprofil", "Perfil de desempenho", "Prestatieprofiel"
};
static const char *s_active_profile[LANG_MAX] = {
    "Active Profile", "Profil actif", "Profilo attivo", "Perfil activo",
    "Aktives Profil", "Perfil ativo", "Actief profiel"
};
static const char *s_dock_status[LANG_MAX] __attribute__((unused)) = {
    "Dock Status", "Etat du dock", "Stato dock", "Estado del dock",
    "Dock-Status", "Estado do dock", "Dockstatus"
};
static const char *s_docked_high[LANG_MAX] = {
    "Docked (High Speed)", "Dock (Haute vitesse)", "Dock (Alta velocita)", "Acoplado (Alta velocidad)",
    "Docked (Hochgeschw.)", "Dock (Alta velocidade)", "Gedockt (Hoge snelheid)"
};
static const char *s_handheld_throttled[LANG_MAX] = {
    "Handheld (Throttled)", "Portable (Limite)", "Portatile (Limitato)", "Portatil (Limitado)",
    "Handheld (Gedaempft)", "Portatil (Limitado)", "Handheld (Beperkt)"
};
static const char *s_frame_rate[LANG_MAX] __attribute__((unused)) = {
    "Frame Rate", "Taux d'images", "Frame rate", "Velocidad de fotogramas",
    "Bildrate", "Taxa de quadros", "Framerate"
};
static const char *s_perf_score[LANG_MAX] __attribute__((unused)) = {
    "Perf Score", "Score perf", "Punteggio prest.", "Puntuacion rend.",
    "Leistungswert", "Pontuacao desem.", "Prestatiescore"
};
static const char *s_cpu_gpu[LANG_MAX] __attribute__((unused)) = {
    "CPU/GPU", "CPU/GPU", "CPU/GPU", "CPU/GPU",
    "CPU/GPU", "CPU/GPU", "CPU/GPU"
};
static const char *s_thermal[LANG_MAX] __attribute__((unused)) = {
    "Thermal", "Thermique", "Termico", "Termico",
    "Thermisch", "Termico", "Thermisch"
};
static const char *s_cpu_load[LANG_MAX] __attribute__((unused)) = {
    "CPU Load", "Charge CPU", "Carico CPU", "Carga CPU",
    "CPU-Last", "Carga CPU", "CPU-belasting"
};
static const char *s_gpu_load[LANG_MAX] __attribute__((unused)) = {
    "GPU Load", "Charge GPU", "Carico GPU", "Carga GPU",
    "GPU-Last", "Carga GPU", "GPU-belasting"
};
static const char *s_mem_load[LANG_MAX] __attribute__((unused)) = {
    "MEM Load", "Charge MEM", "Carico MEM", "Carga MEM",
    "Speicherlast", "Carga MEM", "GEHEUGEN-belasting"
};
static const char *s_fps_history[LANG_MAX] = {
    "FPS History (60 frames):", "Historique FPS (60 images):", "Cronologia FPS (60 frame):", "Historial FPS (60 fotogramas):",
    "FPS-Verlauf (60 Bilder):", "Historico FPS (60 quadros):", "FPS-geschiedenis (60 frames):"
};
static const char *s_ram_usage[LANG_MAX] = {
    "RAM Usage", "Utilisation RAM", "Utilizzo RAM", "Uso de RAM",
    "RAM-Nutzung", "Uso de RAM", "RAM-gebruik"
};
static const char *s_used_total[LANG_MAX] __attribute__((unused)) = {
    "Used / Total", "Utilise / Total", "Usato / Totale", "Usado / Total",
    "Belegt / Gesamt", "Usado / Total", "Gebruikt / Totaal"
};
static const char *s_mem_usage[LANG_MAX] __attribute__((unused)) = {
    "Memory Usage", "Utilisation memoire", "Utilizzo memoria", "Uso de memoria",
    "Speichernutzung", "Uso de memoria", "Geheugengebruik"
};
static const char *s_ram_info_na[LANG_MAX] = {
    "RAM info unavailable", "Infos RAM indisponibles", "Info RAM non disponibile", "Informacion RAM no disponible",
    "RAM-Info nicht verfuegbar", "Info RAM indisponivel", "RAM-info niet beschikbaar"
};
static const char *s_cool[LANG_MAX] = {
    "Cool", "Frais", "Fresco", "Frio",
    "Kuehl", "Frio", "Koel"
};
static const char *s_critical_limit[LANG_MAX] = {
    "CRITICAL LIMIT", "LIMITE CRITIQUE", "LIMITE CRITICO", "LIMITE CRITICO",
    "KRITISCHE GRENZE", "LIMITE CRITICO", "KRITIEKE GRENS"
};
static const char *s_warm_hot[LANG_MAX] = {
    "Warm / Hot", "Tiede / Chaud", "Caldo / Molto caldo", "Templado / Caliente",
    "Warm / Heiss", "Morno / Quente", "Warm / Heet"
};
static const char *s_power_save[LANG_MAX] = {
    "Power Saving Mode", "Mode economie d'energie", "Modalita risparmio energetico", "Modo ahorro de energia",
    "Stromsparmodus", "Modo economia de energia", "Energiebesparingsmodus"
};
static const char *s_high_perf[LANG_MAX] = {
    "High Performance", "Haute performance", "Alte prestazioni", "Alto rendimiento",
    "Hohe Leistung", "Alto desempenho", "Hoge prestatie"
};
static const char *s_boost_profile[LANG_MAX] = {
    "Horizon Boost Profile", "Profil Boost Horizon", "Profilo Boost Horizon", "Perfil Boost Horizon",
    "Horizon-Boost-Profil", "Perfil Boost Horizon", "Horizon Boost-profiel"
};
static const char *s_system_load[LANG_MAX] = {
    "System Load Indicators", "Indicateurs de charge", "Indicatori di carico", "Indicadores de carga",
    "Systemlast-Anzeigen", "Indicadores de carga", "Systeembelastingsindicatoren"
};
static const char *s_perf_score_fmt[LANG_MAX] = {
    "Performance Score: %u", "Score performance: %u", "Punteggio prestazioni: %u", "Puntuacion rendimiento: %u",
    "Leistungswert: %u", "Pontuacao de desempenho: %u", "Prestatiescore: %u"
};
static const char *s_current_fps_fmt[LANG_MAX] = {
    "Current: %d FPS", "Actuel: %d FPS", "Attuale: %d FPS", "Actual: %d FPS",
    "Aktuell: %d FPS", "Atual: %d FPS", "Huidig: %d FPS"
};
static const char *s_mem_status_high[LANG_MAX] = {
    "HIGH", "ELEVE", "ALTO", "ALTO",
    "HOCH", "ALTO", "HOOG"
};
static const char *s_mem_status_elevated[LANG_MAX] = {
    "Elevated", "Eleve", "Elevato", "Elevado",
    "Erhoeht", "Elevado", "Verhoogd"
};
// --- Page 5: Controller ---
static const char *s_jc_title[LANG_MAX] = {
    "Joy-Con Inputs & Analog Sticks Diagnostics", "Test Joy-Con & Joysticks", "Test Joy-Con & Stick analogici", "Prueba de mandos y joysticks",
    "Joy-Con-Eingaben & Analogsticks", "Teste Joy-Con & Analógicos", "Joy-Con-ingangen & analoge sticks test"
};
static const char *s_triggers[LANG_MAX] = {
    "Triggers:", "Gachettes:", "Grilletto:", "Gatillos:",
    "Trigger:", "Gatilhos:", "Triggers:"
};
static const char *s_controller_type[LANG_MAX] = {
    "Controller Type", "Type de manette", "Tipo di controller", "Tipo de mando",
    "Controller-Typ", "Tipo de controle", "Controllertype"
};
static const char *s_gyro_accel[LANG_MAX] = {
    "Gyroscope & Accelerometer", "Gyroscope & Accelerometre", "Giroscopio & Accelerometro", "Giroscopio & Acelerometro",
    "Gyroskop & Beschleunigungsmesser", "Giroscopio & Acelerometro", "Gyroscoop & Accelerometer"
};
static const char *s_left_jc[LANG_MAX] = {
    "Left Joy-Con", "Joy-Con Gauche", "Joy-Con Sinistro", "Joy-Con Izquierdo",
    "Linker Joy-Con", "Joy-Con Esquerdo", "Linker Joy-Con"
};
static const char *s_right_jc[LANG_MAX] = {
    "Right Joy-Con", "Joy-Con Droit", "Joy-Con Destro", "Joy-Con Derecho",
    "Rechter Joy-Con", "Joy-Con Direito", "Rechter Joy-Con"
};
static const char *s_sixaxis_na[LANG_MAX] = {
    "Six-axis sensors not available (unsupported controller or emulator)", "Capteurs 6 axes non disponibles (manette non compatible ou emulateur)", "Sensori 6-assi non disponibili (controller non supportato o emulatore)", "Sensores de 6 ejes no disponibles (mando no compatible o emulador)",
    "6-Achsen-Sensoren nicht verfuegbar (nicht unterstuetzter Controller oder Emulator)", "Sensores 6 eixos indisponiveis (controle nao suportado ou emulador)", "6-assige sensoren niet beschikbaar (niet-ondersteunde controller of emulator)"
};

// --- Page 6: Tools ---
static const char *s_br_ctrl[LANG_MAX] = {
    "Display Brightness Control", "Controle de luminosite", "Controllo luminosita", "Control de brillo",
    "Display-Helligkeit", "Controle de brilho", "Helderheidsbediening"
};
static const char *s_current_br_fmt[LANG_MAX] = {
    "Current: %.0f%%", "Actuel: %.0f%%", "Attuale: %.0f%%", "Actual: %.0f%%",
    "Aktuell: %.0f%%", "Atual: %.0f%%", "Huidig: %.0f%%"
};
static const char *s_auto_on_off[LANG_MAX] = {
    "Auto: ON  [A] disable", "Auto: ON  [A] desactiver", "Auto: ON  [A] disattiva", "Auto: ON  [A] desactivar",
    "Auto: AN  [A] ausschalten", "Auto: ON  [A] desativar", "Auto: AAN  [A] uitschakelen"
};
static const char *s_auto_off_on[LANG_MAX] = {
    "Auto: OFF  [A] enable", "Auto: OFF  [A] activer", "Auto: OFF  [A] attiva", "Auto: OFF  [A] activar",
    "Auto: AUS  [A] einschalten", "Auto: OFF  [A] ativar", "Auto: UIT  [A] inschakelen"
};
static const char *s_br_adjust[LANG_MAX] = {
    "[DPad] Adjust  |  Touch drag", "[DPad] Ajuster  |  Toucher glisser", "[DPad] Regola  |  Tocca trascina", "[DPad] Ajustar  |  Tocar arrastrar",
    "[DPad] Einstellen  |  Beruehren ziehen", "[DPad] Ajustar  |  Tocar arrastar", "[DPad] Aanpassen  |  Raak en sleep"
};
static const char *s_br_na[LANG_MAX] = {
    "Brightness N/A (emulator)", "Luminosite N/D (emulateur)", "Luminosita N/D (emulatore)", "Brillo N/D (emulador)",
    "Helligkeit N/V (Emulator)", "Brilho N/D (emulador)", "Helderheid N/B (emulator)"
};
static const char *s_haptic_test[LANG_MAX] = {
    "Haptic Vibration Test", "Test vibration haptique", "Test vibrazione aptica", "Prueba de vibracion",
    "Haptischer Vibrationstest", "Teste de vibracao haptica", "Haptische vibratietest"
};
static const char *s_test_haptic[LANG_MAX] = {
    "Test Joy-Con haptic motors:", "Tester moteurs haptiques Joy-Con:", "Test motori aptici Joy-Con:", "Probar motores hapticos Joy-Con:",
    "Joy-Con-Haptikmotoren testen:", "Testar motores hapticos Joy-Con:", "Test Joy-Con haptische motoren:"
};
static const char *s_left_rumble[LANG_MAX] = {
    "[X] Left Rumble", "[X] Rumble Gauche", "[X] Rumble Sinistro", "[X] Vibracion Izquierda",
    "[X] Links Rumble", "[X] Rumble Esquerdo", "[X] Links trillen"
};
static const char *s_right_rumble[LANG_MAX] = {
    "[Y] Right Rumble", "[Y] Rumble Droit", "[Y] Rumble Destro", "[Y] Vibracion Derecha",
    "[Y] Rechts Rumble", "[Y] Rumble Direito", "[Y] Rechts trillen"
};
static const char *s_report_export[LANG_MAX] = {
    "System Report Export", "Export rapport systeme", "Esportazione rapporto", "Exportar informe",
    "Systembericht exportieren", "Exportar relatorio", "Systeemrapport exporteren"
};
static const char *s_perf_export[LANG_MAX] = {
    "Perf Metrics Export", "Export metrics Perf", "Esporta metriche Perf", "Exportar metrics Perf",
    "Perf-Metriken exportieren", "Exportar metricas Perf", "Exporteren perf-metrics"
};
static const char *s_perf_export_desc[LANG_MAX] __attribute__((unused)) = {
    "Export live perf metrics to SD card:", "Exporter les metrics Perf vers SD:", "Esporta metriche Perf su SD:", "Exportar metricas Perf a SD:",
    "Perf-Metriken auf SD exportieren:", "Exportar metricas Perf para SD:", "Exporteren perf-metrics naar SD:"
};
static const char *s_export_desc[LANG_MAX] = {
    "Export full diagnostics to SD card:", "Exporter le diagnostic complet vers carte SD:", "Esporta diagnostica completa su SD:", "Exportar diagnostico completo a tarjeta SD:",
    "Vollstaendige Diagnose auf SD-Karte exportieren:", "Exportar diagnostico completo para cartao SD:", "Volledige diagnostiek naar SD-kaart exporteren:"
};
static const char *s_export_btn[LANG_MAX] = {
    "[B] Export Report", "[B] Exporter rapport", "[B] Esporta rapporto", "[B] Exportar informe",
    "[B] Bericht exportieren", "[B] Exportar relatorio", "[B] Rapport exporteren"
};
static const char *s_export_perf_hint[LANG_MAX] = {
    "Press Y to export perf metrics", "Appuyez sur Y pour exporter les metrics", "Premi Y per esportare le metriche", "Presiona Y para exportar metricas",
    "Druecke Y zum Exportieren der Metriken", "Pressione Y para exportar metricas", "Druk Y om perf-metrics te exporteren"
};
static const char *s_export_save[LANG_MAX] = {
    "Saves to sdmc:/switch/SwitchInfoNX/", "Sauvegarde dans sdmc:/switch/SwitchInfoNX/", "Salvato in sdmc:/switch/SwitchInfoNX/", "Guardado en sdmc:/switch/SwitchInfoNX/",
    "Speichert nach sdmc:/switch/SwitchInfoNX/", "Salvo em sdmc:/switch/SwitchInfoNX/", "Opslaan naar sdmc:/switch/SwitchInfoNX/"
};
static const char *s_runtime_env[LANG_MAX] = {
    "Runtime Environment", "Environnement d'execution", "Ambiente di esecuzione", "Entorno de ejecucion",
    "Laufzeitumgebung", "Ambiente de execucao", "Runtime-omgeving"
};
static const char *s_app_status[LANG_MAX] = {
    "App Status", "Etat de l'app", "Stato app", "Estado de la app",
    "App-Status", "Estado do app", "App-status"
};
static const char *s_running_ok[LANG_MAX] = {
    "Running Normally", "Fonctionne normalement", "In esecuzione normale", "Funcionando normalmente",
    "Laeuft normal", "Executando normalmente", "Normaal actief"
};
static const char *s_render[LANG_MAX] = {
    "Render", "Rendu", "Render", "Render",
    "Render", "Render", "Render"
};
static const char *s_env_label[LANG_MAX] = {
    "Environment", "Environnement", "Ambiente", "Entorno",
    "Umgebung", "Ambiente", "Omgeving"
};
static const char *s_emulator[LANG_MAX] = {
    "Emulator", "Emulateur", "Emulatore", "Emulador",
    "Emulator", "Emulador", "Emulator"
};
static const char *s_retail[LANG_MAX] = {
    "Retail Hardware", "Materiel retail", "Hardware retail", "Hardware minorista",
    "Retail-Hardware", "Hardware original", "Retail hardware"
};
static const char *s_dead_pixel[LANG_MAX] = {
    "[Touch] Dead-Pixel Test", "[Touch] Test pixel mort", "[Touch] Test pixel morto", "[Touch] Prueba de pixeles",
    "[Touch] Dead-Pixel-Test", "[Touch] Teste pixel morto", "[Touch] Dode pixel test"
};
static const char *s_fan_ctrl[LANG_MAX] = {
    "Custom Fan Speed Control", "Controle ventilateur personnalise", "Controllo ventola personalizzato", "Control de ventilador personalizado",
    "Benutzerdefinierte Lueftersteuerung", "Controle de ventoinha personalizado", "Aangepaste ventilatorbediening"
};
static const char *s_fan_speed[LANG_MAX] = {
    "Fan Speed", "Vitesse vent.", "Velocita ventola", "Velocidad ventilador",
    "Lueftergeschw.", "Velocidade vent.", "Ventilatorsnelheid"
};
static const char *s_fan_adjust[LANG_MAX] = {
    "[ZL/ZR] Adjust  [Touch] Drag  [L+R] Reset", "[ZL/ZR] Ajuster  [Touch] Glisser  [L+R] Reinit", "[ZL/ZR] Regola  [Touch] Trascina  [L+R] Reset", "[ZL/ZR] Ajustar  [Touch] Arrastrar  [L+R] Restablecer",
    "[ZL/ZR] Einstellen  [Beruehren] Ziehen  [L+R] Zurueck", "[ZL/ZR] Ajustar  [Tocar] Arrastar  [L+R] Reiniciar", "[ZL/ZR] Aanpassen  [Raak] Sleep  [L+R] Reset"
};
static const char *s_fan_dev[LANG_MAX] = {
    "Under development - fan not available", "En developpement - ventilateur non disponible", "In sviluppo - ventola non disponibile", "En desarrollo - ventilador no disponible",
    "In Entwicklung - Luefter nicht verfuegbar", "Em desenvolvimento - ventoinha indisponivel", "In ontwikkeling - ventilator niet beschikbaar"
};
static const char *s_fan_req[LANG_MAX] = {
    "Requires custom sysmodule (CFW)", "Requiert sysmodule personnalise (CFW)", "Richiede sysmodule personalizzato (CFW)", "Requiere sysmodule personalizado (CFW)",
    "Erfordert benutzerdefiniertes Sysmodule (CFW)", "Requer sysmodule personalizado (CFW)", "Vereist aangepaste sysmodule (CFW)"
};
static const char *s_app_mode[LANG_MAX] = {
    "App Mode", "Mode App", "Modalita App", "Modo de App",
    "App-Modus", "Modo do App", "App-modus"
};
static const char *s_modes[LANG_MAX][3] = {
    {"Normal App", "Overlay (Tesla)", "Sysmodule"},
    {"App normale", "Overlay (Tesla)", "Sysmodule"},
    {"App normale", "Overlay (Tesla)", "Sysmodule"},
    {"App normal", "Overlay (Tesla)", "Sysmodule"},
    {"Normale App", "Overlay (Tesla)", "Sysmodule"},
    {"App normal", "Overlay (Tesla)", "Sysmodule"},
    {"Normale app", "Overlay (Tesla)", "Sysmodule"}
};
static const char *s_mode_switch[LANG_MAX] = {
    "[L/R] Switch mode  (requires restart)", "[L/R] Changer mode  (redemarrage requis)", "[L/R] Cambia modalita  (richiede riavvio)", "[L/R] Cambiar modo  (requiere reinicio)",
    "[L/R] Modus wechseln  (Neustart erforderlich)", "[L/R] Mudar modo  (requer reinicializacao)", "[L/R] Modus wijzigen  (herstart vereist)"
};
static const char *s_overlay_desc[LANG_MAX] = {
    "Overlay: runs as Tesla overlay", "Overlay: fonctionne comme overlay Tesla", "Overlay: funziona come overlay Tesla", "Overlay: funciona como superposicion Tesla",
    "Overlay: laeuft als Tesla-Overlay", "Overlay: funciona como overlay Tesla", "Overlay: werkt als Tesla-overlay"
};
static const char *s_sysmodule_desc[LANG_MAX] = {
    "Sysmodule: runs as background service", "Sysmodule: fonctionne comme service d'arriere-plan", "Sysmodule: funziona come servizio in background", "Sysmodule: funciona como servicio en segundo plano",
    "Sysmodule: laeuft als Hintergrunddienst", "Sysmodule: funciona como servico em segundo plano", "Sysmodule: werkt als achtergrondservice"
};
static const char *s_console_info[LANG_MAX] = {
    "Console Information", "Informations console", "Informazioni console", "Informacion de la consola",
    "Konsoleninformation", "Informacoes do console", "Console-informatie"
};
static const char *s_serial_emu[LANG_MAX] = {
    "(emulator/dev unit)", "(emulateur/unite dev)", "(emulatore/unita dev)", "(emulador/unidad dev)",
    "(Emulator/Entwicklung)", "(emulador/unidade dev)", "(emulator/dev-eenheid)"
};
static const char *s_nickname[LANG_MAX] = {
    "Nickname", "Surnom", "Soprannome", "Apodo",
    "Spitzname", "Apelido", "Bijnaam"
};
static const char *s_sd_info_speed[LANG_MAX] = {
    "SD Card Info & Speed Test", "Infos carte SD & test vitesse", "Info SD e test velocita", "Informacion SD y prueba velocidad",
    "SD-Karteninfo & Geschw.-Test", "Info cartao SD e teste velocidade", "SD-kaartinfo & snelheidstest"
};
static const char *s_total_capacity[LANG_MAX] = {
    "Total Capacity", "Capacite totale", "Capacita totale", "Capacidad total",
    "Gesamtkapazitaet", "Capacidade total", "Totale capaciteit"
};
static const char *s_free_space[LANG_MAX] = {
    "Free Space", "Espace libre", "Spazio libero", "Espacio libre",
    "Freier Speicher", "Espaco livre", "Vrije ruimte"
};
static const char *s_pct_used_fmt[LANG_MAX] = {
    "%.1f%% used", "%.1f%% utilise", "%.1f%% usato", "%.1f%% usado",
    "%.1f%% belegt", "%.1f%% usado", "%.1f%% gebruikt"
};
static const char *s_read_speed_btn[LANG_MAX] = {
    "[Touch] Test Read Speed", "[Touch] Test vitesse lecture", "[Touch] Test velocita lettura", "[Touch] Probar velocidad lectura",
    "[Touch] Lese-Geschw. testen", "[Touch] Testar velocidade leitura", "[Touch] Lees snelheid testen"
};
static const char *s_testing[LANG_MAX] = {
    "Testing...", "Test...", "Test...", "Probando...",
    "Test...", "Testando...", "Testen..."
};
static const char *s_error[LANG_MAX] = {
    "Error!", "Erreur!", "Errore!", "Error!",
    "Fehler!", "Erro!", "Fout!"
};
static const char *s_not_tested[LANG_MAX] = {
    "Not tested", "Non teste", "Non testato", "No probado",
    "Nicht getestet", "Nao testado", "Niet getest"
};
static const char *s_sd_info_na[LANG_MAX] = {
    "SD card info unavailable", "Infos carte SD indisponibles", "Info SD non disponibile", "Info SD no disponible",
    "SD-Karteninfo nicht verfuegbar", "Info cartao SD indisponivel", "SD-kaartinfo niet beschikbaar"
};

// --- Page 7: About ---
static const char *s_about_title[LANG_MAX] = {
    "About Switch Info NX", "A propos de Switch Info NX", "Informazioni su Switch Info NX", "Acerca de Switch Info NX",
    "Ueber Switch Info NX", "Sobre Switch Info NX", "Over Switch Info NX"
};
static const char *s_created_by[LANG_MAX] = {
    "v0.0.2  |  Created by dodosi", "v0.0.2  |  Cree par dodosi", "v0.0.2  |  Creato da dodosi", "v0.0.2  |  Creado por dodosi",
    "v0.0.2  |  Erstellt von dodosi", "v0.0.2  |  Criado por dodosi", "v0.0.2  |  Gemaakt door dodosi"
};
static const char *s_desc_line1[LANG_MAX] = {
    "A premium system information and hardware diagnostic utility", "Un utilitaire d'information systeme et de diagnostic materiel", "Un utility di informazioni di sistema e diagnostica hardware", "Una utilidad de informacion del sistema y diagnostico de hardware",
    "Ein Premium-Systeminformations- und Hardware-Diagnose-Tool", "Um utilitario de informacoes do sistema e diagnostico de hardware", "Een premium systeeminformatie- en hardwarediagnosetool"
};
static const char *s_desc_line2[LANG_MAX] = {
    "for Nintendo Switch homebrew custom firmware.", "pour Nintendo Switch homebrew custom firmware.", "per Nintendo Switch homebrew custom firmware.", "para Nintendo Switch homebrew custom firmware.",
    "fuer Nintendo Switch Homebrew Custom Firmware.", "para Nintendo Switch homebrew custom firmware.", "voor Nintendo Switch homebrew custom firmware."
};

// --- Dead Pixel Test ---
static const char *s_dead_pixel_pages[LANG_MAX][6] = {
    {"","Red","Green","Blue","White","Black"},
    {"","Rouge","Vert","Bleu","Blanc","Noir"},
    {"","Rosso","Verde","Blu","Bianco","Nero"},
    {"","Rojo","Verde","Azul","Blanco","Negro"},
    {"","Rot","Gruen","Blau","Weiss","Schwarz"},
    {"","Vermelho","Verde","Azul","Branco","Preto"},
    {"","Rood","Groen","Blauw","Wit","Zwart"}
};
static const char *s_dead_pixel_fmt[LANG_MAX] = {
    "Dead-Pixel: %s  |  [A] Next  [B] Exit",
    "Pixel mort: %s  |  [A] Suivant  [B] Quitter",
    "Pixel morto: %s  |  [A] Successivo  [B] Esci",
    "Pixel muerto: %s  |  [A] Siguiente  [B] Salir",
    "Dead-Pixel: %s  |  [A] Weiter  [B] Beenden",
    "Pixel morto: %s  |  [A] Proximo  [B] Sair",
    "Dode pixel: %s  |  [A] Volgende  [B] Afsluiten"
};

// --- Footer hints ---
static const char *s_footer_default[LANG_MAX] = {
    "[L/R] Tabs    [Y] Refresh    Touch",
    "[L/G] Onglets    [Y] Rafraichir    Toucher",
    "[L/S] Schede    [Y] Aggiorna    Tocca",
    "[L/I] Pestanhas    [Y] Actualizar    Tocar",
    "[L/R] Tabs    [Y] Aktualisieren    Beruehren",
    "[L/E] Abas    [Y] Atualizar    Tocar",
    "[L/R] Tabbladen    [Y] Verversen    Raak"
};
static const char *s_footer_transfer[LANG_MAX] = {
    "[L/R] Tabs    [A] WiFi    [X] MTP    [Y] Ref    Touch all",
    "[L/G] Onglets    [A] WiFi    [X] MTP    [Y] Rafr.    Toucher",
    "[L/S] Schede    [A] WiFi    [X] MTP    [Y] Agg.    Tocca",
    "[L/I] Pest.    [A] WiFi    [X] MTP    [Y] Act.    Tocar",
    "[L/R] Tabs    [A] WLAN    [X] MTP    [Y] Akt.    Beruehren",
    "[L/E] Abas    [A] WiFi    [X] MTP    [Y] Atual.    Tocar",
    "[L/R] Tabbl.    [A] WiFi    [X] MTP    [Y] Verv.    Raak"
};
static const char *s_footer_tools[LANG_MAX] = {
    "[L/R] Mode    [X] Rumble L    [Y] Rumble R    [B] Export    [ZL/ZR] Fan    [DPad] Scroll",
    "[L/G] Mode    [X] Rumble G    [Y] Rumble D    [B] Export    [ZL/ZR] Vent.    [DPad] Defil.",
    "[L/S] Mod.    [X] Rumble S    [Y] Rumble D    [B] Esp.    [ZL/ZR] Vent.    [DPad] Scorr.",
    "[L/I] Modo    [X] Vib. I    [Y] Vib. D    [B] Export    [ZL/ZR] Vent.    [DPad] Despl.",
    "[L/R] Modus    [X] Rumble L    [Y] Rumble R    [B] Export    [ZL/ZR] Lueft.    [DPad] Rollen",
    "[L/E] Modo    [X] Rumble E    [Y] Rumble D    [B] Export    [ZL/ZR] Vent.    [DPad] Rolagem",
    "[L/R] Modus    [X] Rumble L    [Y] Rumble R    [B] Export.    [ZL/ZR] Vent.    [DPad] Scroll."
};
static const char *s_footer_about[LANG_MAX] = {
    "[L/R] Tabs    [DPad] Scroll    Touch scroll    [Y] Ref",
    "[L/G] Onglets    [DPad] Defil.    Toucher defil.    [Y] Rafr.",
    "[L/S] Schede    [DPad] Scorr.    Tocca scorr.    [Y] Agg.",
    "[L/I] Pest.    [DPad] Despl.    Tocar despl.    [Y] Act.",
    "[L/R] Tabs    [DPad] Rollen    Beruehren scroll.    [Y] Akt.",
    "[L/E] Abas    [DPad] Rolagem    Tocar rolag.    [Y] Atual.",
    "[L/R] Tabbl.    [DPad] Scroll.    Raak scroll.    [Y] Verv."
};
static const char *s_footer_settings[LANG_MAX] = {
    "[B] Back    [DPad] Select    [Left/Right] Change    [Y] Ref",
    "[B] Retour    [DPad] Select.    [Gauche/Droite] Chang.    [Y] Rafr.",
    "[B] Indietro    [DPad] Selez.    [Sinistra/Destra] Cambia    [Y] Agg.",
    "[B] Volver    [DPad] Selecc.    [Izquierda/Derecha] Camb.    [Y] Act.",
    "[B] Zurueck    [DPad] Ausw.    [Links/Rechts] Aend.    [Y] Akt.",
    "[B] Voltar    [DPad] Selecion.    [Esquerda/Direita] Mud.    [Y] Atual.",
    "[B] Terug    [DPad] Select.    [Links/Rechts] Wijz.    [Y] Verv."
};
static const char *s_footer_scroll[LANG_MAX] = {
    "[L/R] Tabs    [DPad] Scroll    [Y] Ref",
    "[L/G] Onglets    [DPad] Defil.    [Y] Rafr.",
    "[L/S] Schede    [DPad] Scorr.    [Y] Agg.",
    "[L/I] Pest.    [DPad] Despl.    [Y] Act.",
    "[L/R] Tabs    [DPad] Rollen    [Y] Akt.",
    "[L/E] Abas    [DPad] Rolagem    [Y] Atual.",
    "[L/R] Tabbl.    [DPad] Scroll.    [Y] Verv."
};
static const char *s_exit_hint[LANG_MAX] = {
    "[+] Exit", "[+] Quitter", "[+] Esci", "[+] Salir",
    "[+] Beenden", "[+] Sair", "[+] Afsluiten"
};

// --- Header ---
static const char *s_no_network[LANG_MAX] = {
    "No Network", "Pas de reseau", "Nessuna rete", "Sin red",
    "Kein Netzwerk", "Sem rede", "Geen netwerk"
};

// --- Export ---
static const char *s_export_error[LANG_MAX] = {
    "Error: Cannot write to SD card", "Erreur: Impossible d'ecrire sur la carte SD", "Errore: Impossibile scrivere su SD", "Error: No se puede escribir en la tarjeta SD",
    "Fehler: Kann nicht auf SD-Karte schreiben", "Erro: Nao e possivel escrever no cartao SD", "Fout: Kan niet naar SD-kaart schrijven"
};
static const char *s_export_saved[LANG_MAX] = {
    "Report saved to sdmc:/switch/SwitchInfoNX/system_report.txt", "Rapport sauvegarde dans sdmc:/switch/SwitchInfoNX/system_report.txt", "Report salvato in sdmc:/switch/SwitchInfoNX/system_report.txt", "Informe guardado en sdmc:/switch/SwitchInfoNX/system_report.txt",
    "Bericht gespeichert unter sdmc:/switch/SwitchInfoNX/system_report.txt", "Relatorio salvo em sdmc:/switch/SwitchInfoNX/system_report.txt", "Rapport opgeslagen naar sdmc:/switch/SwitchInfoNX/system_report.txt"
};

// --- File Browser ---
static const char *s_file_browser[LANG_MAX] = {
    "File Browser", "Explorateur", "Esplora file", "Explorador",
    "Dateibrowser", "Explorador", "Bestandsverkenner"
};
static const char *s_name_col[LANG_MAX] = {
    "Name", "Nom", "Nome", "Nombre",
    "Name", "Nome", "Naam"
};
static const char *s_size_col[LANG_MAX] = {
    "Size", "Taille", "Dimensione", "Tamahho",
    "Groesse", "Tamanho", "Grootte"
};
static const char *s_dir_tag[LANG_MAX] = {
    "<DIR>", "<DOS>", "<CART>", "<DIR>",
    "<VERZ>", "<DIR>", "<MAP>"
};
static const char *s_fb_delete_q[LANG_MAX] = {
    "Delete selected item?", "Supprimer l'element selectionne?", "Eliminare l'elemento selezionato?", "Eliminar elemento seleccionado?",
    "Ausgewaehltes Element loeschen?", "Excluir item selecionado?", "Geselecteerd item verwijderen?"
};
static const char *s_fb_confirm_del[LANG_MAX] = {
    "[A] Confirm Delete  [B] Cancel", "[A] Confirmer suppr.  [B] Annuler", "[A] Conferma elim.  [B] Annulla", "[A] Conf. eliminar  [B] Cancelar",
    "[A] Loeschen best.  [B] Abbrechen", "[A] Conf. exclusao  [B] Cancelar", "[A] Bevestig verwij.  [B] Annuleren"
};
static const char *s_fb_browse_hint[LANG_MAX] = {
    "[A] Enter  [B] Back  [X] Delete  [Y] Rename",
    "[A] Entrer  [B] Retour  [X] Suppr.  [Y] Renommer",
    "[A] Entra  [B] Indietro  [X] Elim.  [Y] Rinomina",
    "[A] Entrar  [B] Volver  [X] Elim.  [Y] Renombrar",
    "[A] Oeffnen  [B] Zuru.  [X] Loesch.  [Y] Umben.",
    "[A] Entrar  [B] Voltar  [X] Excluir  [Y] Renomear",
    "[A] Open  [B] Terug  [X] Verwijd.  [Y] Hernoem"
};
static const char *s_fb_paste[LANG_MAX] = {
    "[L] Paste", "[L] Coller", "[L] Incolla", "[L] Pegar",
    "[L] Einfuegen", "[L] Colar", "[L] Plakken"
};
static const char *s_fb_other_hint[LANG_MAX] = {
    "[R] Copy  [ZL] Cut  [-] Home", "[R] Copier  [ZL] Couper  [-] Accueil", "[R] Copia  [ZL] Taglia  [-] Home", "[R] Copiar  [ZL] Cortar  [-] Inicio",
    "[R] Kopieren  [ZL] Aussch.  [-] Start", "[R] Copiar  [ZL] Recortar  [-] Inicio", "[R] Kopieer  [ZL] Knip  [-] Home"
};
static const char *s_fb_rename_hint[LANG_MAX] = {
    "[Up/Dn] cycle char  [L/R] cursor  [ZL] del char  [A] Confirm  [B] Cancel",
    "[Haut/Bas] car. suiv./prec.  [G/D] curseur  [ZL] suppr. car.  [A] Conf.  [B] Annul.",
    "[Su/Giu] ciclo char  [S/D] cursore  [ZL] elim. char  [A] Conf.  [B] Annul.",
    "[Arr/Ab] caract. sig./ant.  [I/D] cursor  [ZL] elim. car.  [A] Conf.  [B] Canc.",
    "[Hoch/Runt] Zeichen  [L/R] Cursor  [ZL] loesch.  [A] Best.  [B] Abbr.",
    "[Cima/Baixo] ciclo char  [E/D] cursor  [ZL] elim. char  [A] Conf.  [B] Canc.",
    "[Omh/Oml] teken cyc.  [L/R] cursor  [ZL] del teken  [A] Bevest.  [B] Annul."
};
static const char *s_fb_rename_empty[LANG_MAX] = {
    "(empty)", "(vide)", "(vuoto)", "(vacio)",
    "(leer)", "(vazio)", "(leeg)"
};

// --- Popup dismiss ---
static const char *s_popup_dismiss[LANG_MAX] = {
    "Tap anywhere to dismiss", "Touchez pour fermer", "Tocca per chiudere", "Toca para cerrar",
    "Zum Schliessen tippen", "Toque para fechar", "Tik om te sluiten"
};

// --- Settings info (replaces original multi-line) ---
static const char *s_settings_info2[LANG_MAX] = {
    "[DPad] Switch item  |  [L/R] Change value  |  [B] Back",
    "[DPad] Changer item  |  [L/G] Changer valeur  |  [B] Retour",
    "[DPad] Cambia item  |  [L/S] Cambia valore  |  [B] Indietro",
    "[DPad] Cambiar item  |  [I/D] Cambiar valor  |  [B] Volver",
    "[DPad] Wechseln  |  [L/R] Wert aendern  |  [B] Zurueck",
    "[DPad] Mudar item  |  [L/E] Mudar valor  |  [B] Voltar",
    "[DPad] Wissele  |  [L/R] Waarde wijz.  |  [B] Terug"
};

// --- Settings refresh names ---
static const char *s_refresh_names[LANG_MAX][5] = {
    {"Off", "1s", "3s", "5s", "10s"},
    {"Arret", "1s", "3s", "5s", "10s"},
    {"Spento", "1s", "3s", "5s", "10s"},
    {"Apag.", "1s", "3s", "5s", "10s"},
    {"Aus", "1s", "3s", "5s", "10s"},
    {"Desl.", "1s", "3s", "5s", "10s"},
    {"Uit", "1s", "3s", "5s", "10s"}
};

// --- Custom Theme Editor ---
static const char *s_cte_title[LANG_MAX] = {
    "Custom Theme Editor", "Editeur de theme", "Editor tema personalizzato", "Editor de tema",
    "Benutzerdefinierter Theme-Editor", "Editor de tema personalizado", "Aangepaste thema-editor"
};
static const char *s_cte_names[10] __attribute__((unused)) = {"Background","Card bg","Border","Accent Cyan","Accent Green","Text","Grey","Dark Grey","Header bg2","Tab bg3"};
static const char *s_cte_edit_hint[LANG_MAX] = {
    "Selected Color - [L/R] Channel  [Up/Down/ZL/ZR] Value  [Touch] Drag",
    "Couleur - [L/G] Canal  [Haut/Bas/ZL/ZR] Valeur  [Touch] Glisser",
    "Colore - [L/S] Canale  [Su/Giu/ZL/ZR] Valore  [Touch] Trascina",
    "Color - [I/D] Canal  [Arr/Ab/ZL/ZR] Valor  [Touch] Arrastrar",
    "Farbe - [L/R] Kanal  [Hoch/Runt/ZL/ZR] Wert  [Touch] Ziehen",
    "Cor - [L/E] Canal  [Cima/Baixo/ZL/ZR] Valor  [Touch] Arrastar",
    "Kleur - [L/R] Kanaal  [Omh/Oml/ZL/ZR] Waarde  [Touch] Sleep"
};
static const char *s_cte_save[LANG_MAX] = {
    "[A] Save & Apply", "[A] Sauver & Appliquer", "[A] Salva & Applica", "[A] Guardar & Aplicar",
    "[A] Speichern & Anwenden", "[A] Salvar & Aplicar", "[A] Opslaan & Toepassen"
};
static const char *s_cte_cancel[LANG_MAX] = {
    "[B] Cancel", "[B] Annuler", "[B] Annulla", "[B] Cancelar",
    "[B] Abbrechen", "[B] Cancelar", "[B] Annuleren"
};
static const char *s_cte_touch_hint[LANG_MAX] = {
    "Touch color slot to select | Touch RGB bar to set value | A=Save  B=Cancel",
    "Toucher couleur pour selectionner | Toucher barre RVB | A=Sauv.  B=Annul.",
    "Tocca colore per selezionare | Tocca barra RGB per valore | A=Salva  B=Annulla",
    "Tocar color para seleccionar | Tocar barra RGB | A=Guardar  B=Cancelar",
    "Farbe antippen zum Ausw. | RGB-Balken antippen | A=Speich.  B=Abbr.",
    "Tocar cor para selecionar | Tocar barra RGB | A=Salvar  B=Cancelar",
    "Raak kleur aan om te selecteren | Raak RGB-balk | A=Opslaan  B=Annul."
};

// --- Existing i18n strings ---
static const char *s_dev_fan[LANG_MAX] = {
    "Fan control: Under development",
    "Controle du ventilateur: En developpement",
    "Controllo ventola: In sviluppo",
    "Control ventilador: En desarrollo",
    "Lueftersteuerung: In Entwicklung",
    "Controle ventoinha: Em desenvolvimento",
    "Ventilatorbediening: In ontwikkeling"
};
static const char *s_scroll_up[LANG_MAX] = {
    "^ Scroll up", "^ Haut", "^ Su", "^ Arriba", "^ Hoch", "^ Cima", "^ Omhoog"
};
static const char *s_scroll_down[LANG_MAX] = {
    "v Scroll down", "v Bas", "v Giu", "v Abajo", "v Runter", "v Baixo", "v Omlaag"
};

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
#define THEME_CUSTOM 11
#define THEME_MAX 12
static int cur_theme = THEME_DARK;
static int auto_refresh_interval = 3; // seconds, 0 = off

static const char *theme_names[THEME_MAX] = {
    "Dark", "Light", "Blue", "Green", "Purple", "Red", "Pink", "Orange", "Amber", "Teal", "Cyan", "Custom"
};

static int settings_sel = 0; // 0=language, 1=theme, 2=auto-refresh

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
static const SDL_Color theme_bg[THEME_MAX]       = { {18,18,20,255}, {235,235,240,255}, {10,18,35,255}, {10,28,12,255}, {28,10,35,255}, {35,10,10,255}, {40,15,25,255}, {30,18,8,255}, {30,24,8,255}, {8,30,28,255}, {8,25,35,255}, {18,18,20,255} };
static const SDL_Color theme_card[THEME_MAX]     = { {28,28,32,255}, {215,215,222,255}, {18,30,52,255}, {18,40,22,255}, {40,18,52,255}, {52,18,18,255}, {55,20,30,255}, {45,28,14,255}, {45,34,14,255}, {14,42,38,255}, {14,35,50,255}, {28,28,32,255} };
static const SDL_Color theme_border[THEME_MAX]   = { {42,42,48,255}, {190,190,200,255}, {30,45,70,255}, {30,55,35,255}, {55,30,70,255}, {70,30,30,255}, {75,35,45,255}, {65,42,24,255}, {65,48,24,255}, {24,55,50,255}, {24,48,65,255}, {42,42,48,255} };
static const SDL_Color theme_cyan[THEME_MAX]     = { {0,210,255,255}, {0,120,180,255}, {0,200,255,255}, {0,220,200,255}, {100,200,255,255}, {255,100,100,255}, {255,120,180,255}, {255,180,80,255}, {255,210,80,255}, {0,220,200,255}, {0,210,255,255}, {0,210,255,255} };
static const SDL_Color theme_green[THEME_MAX]    = { {0,255,136,255}, {0,180,80,255}, {0,240,140,255}, {0,255,136,255}, {100,255,180,255}, {255,100,100,255}, {255,120,180,255}, {255,180,80,255}, {255,210,80,255}, {0,255,200,255}, {100,255,220,255}, {0,255,136,255} };
static const SDL_Color theme_white[THEME_MAX]    = { {255,255,255,255}, {20,20,25,255}, {220,230,255,255}, {200,255,210,255}, {230,210,255,255}, {255,210,210,255}, {255,210,220,255}, {255,230,200,255}, {255,240,200,255}, {200,240,235,255}, {200,230,240,255}, {255,255,255,255} };
static const SDL_Color theme_grey[THEME_MAX]     = { {150,150,160,255}, {100,100,110,255}, {130,140,160,255}, {130,160,140,255}, {160,140,170,255}, {170,130,130,255}, {170,130,140,255}, {170,150,130,255}, {170,160,130,255}, {130,160,155,255}, {130,150,160,255}, {150,150,160,255} };
static const SDL_Color theme_dark_grey[THEME_MAX] = { {55,55,62,255}, {170,170,178,255}, {45,55,75,255}, {45,65,50,255}, {65,45,75,255}, {75,45,45,255}, {75,48,55,255}, {70,55,40,255}, {70,60,40,255}, {40,65,60,255}, {40,55,70,255}, {55,55,62,255} };
static const SDL_Color theme_bg2[THEME_MAX]      = { {22,22,26,255}, {225,225,232,255}, {14,22,39,255}, {14,32,16,255}, {32,14,39,255}, {39,14,14,255}, {44,18,28,255}, {34,22,12,255}, {34,28,12,255}, {12,34,32,255}, {12,28,39,255}, {22,22,26,255} };
static const SDL_Color theme_bg3[THEME_MAX]      = { {34,34,42,255}, {195,195,202,255}, {22,34,56,255}, {22,44,26,255}, {44,22,56,255}, {56,22,22,255}, {60,25,35,255}, {50,32,18,255}, {50,38,18,255}, {18,48,44,255}, {18,40,56,255}, {34,34,42,255} };

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

// Config save/load
#define CONFIG_PATH "sdmc:/switch/SwitchInfoNX/config.txt"
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
    }
    fclose(f);
    apply_theme();
}

// forward declaration
static void add_alert(const char *msg);

// Export current performance metrics snapshot to CSV
static void save_perf_metrics(void) {
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    const char *path = "sdmc:/switch/SwitchInfoNX/perf_metrics.csv";
    FILE *f = fopen(path, "a+");
    if (!f) {
        add_alert("Error: cannot open metrics file");
        return;
    }
    // If empty, write header
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz == 0) {
        fprintf(f, "timestamp,local_time,fps,cpu_mhz,gpu_mhz,mem_mhz,skin_mC,mem_used_bytes,mem_total_bytes\n");
    }

    // collect metrics
    time_t now = time(NULL);
    char timestr[64];
    struct tm *lt = localtime(&now);
    if (lt) strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", lt);

    // FPS
    int fps = current_fps;

    // clocks
    u32 cpu=0,gpu=0,memclk=0;
    ClkrstSession cc,cg,cm;
    if (R_SUCCEEDED(clkrstOpenSession(&cc,(PcvModuleId)PcvModule_CpuBus,3)) &&
        R_SUCCEEDED(clkrstOpenSession(&cg,(PcvModuleId)PcvModule_GPU,3)) &&
        R_SUCCEEDED(clkrstOpenSession(&cm,(PcvModuleId)PcvModule_EMC,3))) {
        clkrstGetClockRate(&cc,&cpu);
        clkrstGetClockRate(&cg,&gpu);
        clkrstGetClockRate(&cm,&memclk);
        clkrstCloseSession(&cc);
        clkrstCloseSession(&cg);
        clkrstCloseSession(&cm);
    }

    // skin temp
    s32 skin = 0;
    if (R_SUCCEEDED(tcInitialize())) { tcGetSkinTemperatureMilliC(&skin); tcExit(); }

    // memory usage
    u64 mem_total = 0, mem_used = 0;
    if (!(R_SUCCEEDED(svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
          R_SUCCEEDED(svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)))) {
        mem_total = 0; mem_used = 0;
    }

    fprintf(f, "%llu,%s,%d,%u,%u,%u,%d,%llu,%llu\n",
        (unsigned long long)now,
        timestr,
        fps,
        (unsigned)cpu/1000000u,
        (unsigned)gpu/1000000u,
        (unsigned)memclk/1000000u,
        (int)skin,
        (unsigned long long)mem_used,
        (unsigned long long)mem_total);
    fclose(f);
    add_alert("Metrics exported");
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
    return sn[0] ? "Switch OLED (HEG-001)" : "Unknown Model";
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

// ─── Drawing helpers ──────────────────────────────────────

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *text, int x, int y, SDL_Color color, int align) {
    if (!text || !text[0]) return;
    SDL_Surface *surface = TTF_RenderText_Blended(font, text, color);
    if (!surface) return;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(r, surface);
    if (texture) {
        SDL_Rect rect;
        rect.y = y;
        rect.w = surface->w;
        rect.h = surface->h;
        if (align == 0) {
            rect.x = x;
        } else if (align == 1) {
            rect.x = x - surface->w / 2;
        } else {
            rect.x = x - surface->w;
        }
        SDL_RenderCopy(r, texture, NULL, &rect);
        SDL_DestroyTexture(texture);
    }
    SDL_FreeSurface(surface);
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
        draw_text(r, font_md, title, x + 20, y + 15, color_cyan, 0);
        // Accent line under title
        thickLineRGBA(r, x + 20, y + 48, x + w - 20, y + 48, 2, color_cyan.r, color_cyan.g, color_cyan.b, 120);
    }
}

static void draw_key_value(SDL_Renderer *r, const char *key, const char *val, int x, int y, SDL_Color val_color) {
    draw_text(r, font_sm, key, x, y, color_grey, 0);
    draw_text(r, font_sm, val, x + 200, y, val_color, 0);
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
    int bar_w = w / 4 - 2;
    for (int i = 0; i < 4; i++) {
        int bar_h = (h * (i + 1)) / 4;
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

// ─── FTP Server ───────────────────────────────────────────
#define FTP_PORT 5000
#define FTP_MAXCL 4
#define FTP_BUF 4096
#define FTP_LOG_MAX 8

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static volatile int ftp_on = 0;
static int ftp_srv = -1, ftp_data = -1;
static volatile int ftp_cli_fd = -1;
static volatile int ftp_xfer_fd = -1;
static Thread ftp_thr;
static char ftp_cwd[256] = "/";
static u32 ftp_ip = 0;
static u32 ftp_xfer_count = 0;
static u64 ftp_bytes_total = 0;
static u32 ftp_upload_count = 0;
static u32 ftp_download_count = 0;

// Activity log
static char ftp_log[FTP_LOG_MAX][120];
static unsigned int ftp_log_idx = 0;
static Mutex ftp_log_mtx;

static int ftp_mode = 0; // 0=FTP, 1=FTPD
static int ftp_get_port(void) { return ftp_mode ? 5001 : 5000; }

static char ftp_rnfr[260] = {0};
static u64 ftp_rest_offset = 0;

static void ftp_addlog(const char *msg) {
    mutexLock(&ftp_log_mtx);
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt)
        snprintf(ftp_log[ftp_log_idx % FTP_LOG_MAX], 120, "%02d:%02d:%02d  %s",
            lt->tm_hour, lt->tm_min, lt->tm_sec, msg);
    else
        snprintf(ftp_log[ftp_log_idx % FTP_LOG_MAX], 120, "%s", msg);
    ftp_log_idx++;
    mutexUnlock(&ftp_log_mtx);
}

static void add_alert(const char *msg) {
    snprintf(export_msg, sizeof(export_msg), "%s", msg);
    export_msg_tick = armGetSystemTick();
}

static int fsend(int fd, const char *m) { return send(fd, m, strlen(m), MSG_NOSIGNAL); }

static void get_real_path(char *dst, size_t dst_size, const char *virtual_path) {
    if (virtual_path[0] == '/') {
        snprintf(dst, dst_size, "sdmc:%s", virtual_path);
    } else {
        snprintf(dst, dst_size, "sdmc:/%s", virtual_path);
    }
}

static void fclean(char *d, const char *b, const char *r) {
    char t[512];
    if (r[0] == '/') {
        snprintf(t, sizeof(t), "%s", r);
    } else if (strcmp(r, "..") == 0) {
        snprintf(t, sizeof(t), "%s", b);
        int len = (int)strlen(t);
        while (len > 1 && t[len-1] == '/') t[--len] = 0;
        char *slash = strrchr(t, '/');
        if (slash && slash != t) *slash = 0;
        else snprintf(t, sizeof(t), "/");
    } else {
        int blen = (int)strlen(b);
        if (blen > 1)
            snprintf(t, sizeof(t), "%s/%s", b, r);
        else
            snprintf(t, sizeof(t), "/%s", r);
    }
    int len = (int)strlen(t);
    while (len > 1 && t[len-1] == '/') t[--len] = 0;
    snprintf(d, 256, "%s", t);
}

static void fcli(int fd, u32 ip) {
    ftp_cli_fd = fd;
    char buf[FTP_BUF];
    const char *mode_name = ftp_mode ? "FTPD" : "FTP";
    int port = ftp_get_port();
    char banner[128];
    snprintf(banner, sizeof(banner), "220 SwitchInfoNX %s v0.0.2 (port %d) - sdmc:/ root access ready\r\n", mode_name, port);
    fsend(fd, banner);
    ftp_data = -1;
    ftp_rest_offset = 0;
    ftp_addlog("Client connected");
    add_alert("FTP: Client connected");

    while (ftp_on) {
        memset(buf, 0, sizeof(buf));
        int n = recv(fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        for (int i = 0; buf[i]; i++)
            if (buf[i]=='\r'||buf[i]=='\n') { buf[i]=0; break; }

        char cmd[32]={0}, arg[256]={0};
        sscanf(buf, "%31s %255[^\r\n]", cmd, arg);

        if (strcmp(cmd,"USER")==0) fsend(fd,"230 Login OK (anonymous)\r\n");
        else if (strcmp(cmd,"PASS")==0) fsend(fd,"230 Login OK\r\n");
        else if (strcmp(cmd,"SYST")==0) fsend(fd,"215 UNIX Type: L8\r\n");
        else if (strcmp(cmd,"FEAT")==0) {
            fsend(fd,"211-Features:\r\n SIZE\r\n PASV\r\n UTF8\r\n MDTM\r\n REST STREAM\r\n211 End\r\n");
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
            char new_dir[256];
            fclean(new_dir, ftp_cwd, arg);
            // Verify directory exists
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
        else if (strcmp(cmd,"PASV")==0) {
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
            char r[128]; snprintf(r,sizeof(r),"227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\r\n",
                (ip>>0)&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF,(dp>>8)&0xFF,dp&0xFF);
            fsend(fd,r);
        }
        else if (strcmp(cmd,"LIST")==0 || strcmp(cmd,"NLST")==0 || strcmp(cmd,"MLSD")==0) {
            char dp[256];
            if (arg[0]) fclean(dp,ftp_cwd,arg); else snprintf(dp, sizeof(dp), "%s", ftp_cwd);
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
            fsend(fd,"150 Listing\r\n");
            struct dirent *e;
            while ((e=readdir(d))!=NULL) {
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
                // Format modification time
                char datestr[32] = "Jan  1 00:00";
                if (res == 0) {
                    struct tm *mt = localtime(&st.st_mtime);
                    if (mt) {
                        static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
                        snprintf(datestr, sizeof(datestr), "%s %2d %02d:%02d",
                            months[mt->tm_mon], mt->tm_mday, mt->tm_hour, mt->tm_min);
                    }
                }
                snprintf(l,sizeof(l),"%s 1 switch switch %12s %s %s\r\n",
                    id?"drwxr-xr-x":"-rw-r--r--",szb,datestr,e->d_name);
                send(df,l,strlen(l),MSG_NOSIGNAL);
            }
            closedir(d); close(df); ftp_xfer_fd = -1; fsend(fd,"226 Done\r\n");
        }
        else if (strcmp(cmd,"RETR")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            FILE *f = fopen(rp,"rb");
            if (!f) { fsend(fd,"550 File not found\r\n"); continue; }
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
                ftp_rest_offset = 0;
            }
            fsend(fd,"150 Sending\r\n");
            char db[8192]; int r;
            u64 sent = 0;
            while (ftp_on && (r=fread(db,1,sizeof(db),f))>0) {
                int s = send(df,db,r,MSG_NOSIGNAL);
                if (s <= 0) break;
                sent += s;
            }
            fclose(f); close(df); ftp_xfer_fd = -1; fsend(fd,"226 Transfer complete\r\n");
            ftp_xfer_count++;
            ftp_download_count++;
            ftp_bytes_total += sent;
            char log[80]; snprintf(log,sizeof(log),"RETR %s (%llu B)", arg, (unsigned long long)sent);
            ftp_addlog(log);
        }
        else if (strcmp(cmd,"STOR")==0 || strcmp(cmd,"APPE")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            const char *mode = (strcmp(cmd,"APPE")==0) ? "ab" : "wb";
            FILE *f = fopen(rp, mode);
            if (!f) { fsend(fd,"550 Cannot create file\r\n"); continue; }
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
            fsend(fd,"150 Receiving\r\n");
            char db[8192]; int r;
            u64 rcvd = 0;
            while (ftp_on && (r=recv(df,db,sizeof(db),0))>0) {
                fwrite(db,1,r,f);
                rcvd += r;
            }
            fclose(f); close(df); ftp_xfer_fd = -1; fsend(fd,"226 Transfer complete\r\n");
            ftp_xfer_count++;
            ftp_upload_count++;
            ftp_bytes_total += rcvd;
            char log[80]; snprintf(log,sizeof(log),"%s %s (%llu B)", cmd, arg, (unsigned long long)rcvd);
            ftp_addlog(log);
        }
        else if (ftp_mode && (strcmp(cmd,"STOR")==0||strcmp(cmd,"APPE")==0||strcmp(cmd,"MKD")==0||strcmp(cmd,"XMKD")==0||strcmp(cmd,"RMD")==0||strcmp(cmd,"XRMD")==0||strcmp(cmd,"DELE")==0||strcmp(cmd,"RNFR")==0||strcmp(cmd,"RNTO")==0)) {
            fsend(fd, "550 FTPD: Read-only mode\r\n");
        }
        else if (strcmp(cmd,"MKD")==0 || strcmp(cmd,"XMKD")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char rp[540]; get_real_path(rp, sizeof(rp), p);
            if (mkdir(rp, 0755)==0) {
                char r[512]; snprintf(r,sizeof(r),"257 \"%s\" created\r\n",p);
                fsend(fd,r);
                char log[80]; snprintf(log,sizeof(log),"MKD %s", arg);
                ftp_addlog(log);
            } else fsend(fd,"550 Cannot create directory\r\n");
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
    if (ftp_srv<0) { ftp_on=0; return; }
    int o=1; setsockopt(ftp_srv,SOL_SOCKET,SO_REUSEADDR,&o,sizeof(o));
    a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(ftp_get_port());
    if (bind(ftp_srv,(struct sockaddr*)&a,sizeof(a))<0||listen(ftp_srv,FTP_MAXCL)<0)
        { close(ftp_srv); ftp_on=0; return; }
    char logmsg[64];
    snprintf(logmsg, sizeof(logmsg), "%s listening on port %d", ftp_mode ? "FTPD" : "FTP", ftp_get_port());
    ftp_addlog(logmsg);
    while (ftp_on) {
        struct sockaddr_in c;
        socklen_t cl=sizeof(c);
        int fd = accept(ftp_srv,(struct sockaddr*)&c,&cl);
        if (fd<0) { if (!ftp_on) break; continue; }
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
    mutexInit(&ftp_log_mtx);
    ftp_on = 1;
    threadCreate(&ftp_thr, fserve, NULL, NULL, 32768, 0x2B, -2);
    threadStart(&ftp_thr);
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
static char mtp_log[4][120];
static unsigned int mtp_log_idx = 0;
static Mutex mtp_log_mtx;
static u64 mtp_xfer_start_tick = 0;

static void mtp_addlog(const char *msg) {
    mutexLock(&mtp_log_mtx);
    snprintf(mtp_log[mtp_log_idx % 4], 120, "%s", msg);
    mtp_log_idx++;
    mutexUnlock(&mtp_log_mtx);
}

static void mtp_start_local(void) {
    if (mtp_on || mtp_server_is_running()) return;
    mtp_xfer_count = 0; mtp_bytes_total = 0; mtp_log_idx = 0;
    mutexInit(&mtp_log_mtx);
    Result rc = mtp_server_start();
    if (R_FAILED(rc)) { mtp_addlog("MTP init failed"); return; }
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
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    FILE *f = fopen("sdmc:/switch/SwitchInfoNX/system_report.txt", "w");
    if (!f) {
        snprintf(export_msg, sizeof(export_msg), "%s", s_export_error[cur_lang]);
        export_msg_tick = armGetSystemTick();
        return;
    }

    fprintf(f, "=== SwitchInfoNX System Report ===\n");
    fprintf(f, "Generated by SwitchInfoNX v0.0.2 by dodosi\n\n");

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
    if (R_SUCCEEDED(clkrstOpenSession(&cc,(PcvModuleId)PcvModule_CpuBus,3))) {
        clkrstGetClockRate(&cc,&cpu);
        fprintf(f, "CPU: %u MHz\n", cpu/1000000);
        clkrstCloseSession(&cc);
    }
    if (R_SUCCEEDED(clkrstOpenSession(&cg,(PcvModuleId)PcvModule_GPU,3))) {
        clkrstGetClockRate(&cg,&gpu);
        fprintf(f, "GPU: %u MHz\n", gpu/1000000);
        clkrstCloseSession(&cg);
    }
    if (R_SUCCEEDED(clkrstOpenSession(&cm,(PcvModuleId)PcvModule_EMC,3))) {
        clkrstGetClockRate(&cm,&mem);
        fprintf(f, "Memory: %u MHz\n", mem/1000000);
        clkrstCloseSession(&cm);
    }

    // Temps
    if (R_SUCCEEDED(tcInitialize())) {
        s32 skin = 0;
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin)))
            fprintf(f, "Temperature: %d.%d C\n", skin/1000, (skin%1000)/100);
        tcExit();
    }
    fprintf(f, "\n");

    fprintf(f, "=== End of Report ===\n");
    fclose(f);

    snprintf(export_msg, sizeof(export_msg), "%s", s_export_saved[cur_lang]);
    export_msg_tick = armGetSystemTick();
}

static void sd_run_speed_test(void) {
    FILE *f = fopen("sdmc:/switch/SwitchInfoNX/speed_test.tmp", "wb");
    if (!f) { sd_speed_result = -1; return; }
    // Write 16MB of test data
    u8 buf[65536];
    memset(buf, 0xFF, sizeof(buf));
    for (int i = 0; i < 256; i++) {
        size_t w = fwrite(buf, 1, sizeof(buf), f);
        if (w != sizeof(buf)) break;
    }
    fclose(f);
    // Now read it back
    f = fopen("sdmc:/switch/SwitchInfoNX/speed_test.tmp", "rb");
    if (!f) { sd_speed_result = -1; return; }
    u64 total_read = 0;
    u64 read_start = armGetSystemTick();
    while (1) {
        size_t r = fread(buf, 1, sizeof(buf), f);
        total_read += r;
        if (r != sizeof(buf)) break;
    }
    u64 read_end = armGetSystemTick();
    fclose(f);
    remove("sdmc:/switch/SwitchInfoNX/speed_test.tmp");
    // Calculate read speed in MB/s
    u64 read_ticks = read_end - read_start;
    if (read_ticks > 0) {
        double sec = (double)read_ticks / armGetSystemTickFreq();
        sd_speed_result = (float)(total_read / 1048576.0 / sec);
    } else {
        sd_speed_result = 0;
    }
}

// ─── Header bar ───────────────────────────────────────────

static void draw_header(SDL_Renderer *r) {
    // Gradient background
    SDL_Rect bg = {0, 0, W, 50};
    SDL_SetRenderDrawColor(r, color_bg2.r, color_bg2.g, color_bg2.b, 255);
    SDL_RenderFillRect(r, &bg);
    thickLineRGBA(r, 0, 50, W, 50, 2, color_card_border.r, color_card_border.g, color_card_border.b, 255);

    // Cyan accent dot + title
    filledCircleRGBA(r, 14, 25, 5, color_cyan.r, color_cyan.g, color_cyan.b, 255);
    draw_text(r, font_md, "Switch Info NX", 28, 11, color_white, 0);
    draw_text(r, font_sm, "v0.0.2", 220, 16, color_cyan, 0);

    // Refresh count badge
    char ref_str[16];
    snprintf(ref_str, sizeof(ref_str), "[%u]", refresh_count);
    draw_text(r, font_sm, ref_str, 290, 16, color_grey, 0);

    // Time
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) {
        char ts[16];
        snprintf(ts, sizeof(ts), "%02d:%02d", lt->tm_hour, lt->tm_min);
        draw_text(r, font_md, ts, W - 80, 11, color_white, 0);
    }

    // IP
    u32 ip = 0;
    nifmGetCurrentIpAddress(&ip);
    char ips[32] = {0};
    if (ip) {
        snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);
        draw_text(r, font_sm, ips, W - 320, 16, color_cyan, 0);
    } else {
        draw_text(r, font_sm, s_no_network[cur_lang], W - 320, 16, color_red, 0);
    }

    // Uptime
    u64 uptime_ticks = armGetSystemTick() - start_tick;
    u64 uptime_sec = uptime_ticks / armGetSystemTickFreq();
    int days = (int)(uptime_sec / 86400);
    int hours = (int)((uptime_sec % 86400) / 3600);
    int mins = (int)((uptime_sec % 3600) / 60);
    if (days > 0)
        snprintf(ips, sizeof(ips), "%dd %02dh", days, hours);
    else if (hours > 0)
        snprintf(ips, sizeof(ips), "%dh %02dm", hours, mins);
    else
        snprintf(ips, sizeof(ips), "%dm", mins);
    draw_text(r, font_xs, ips, 340, 18, color_grey, 0);

    // Battery
    u32 batt = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    psmGetBatteryChargePercentage(&batt);
    psmGetChargerType(&ch);
    draw_battery_icon(r, W - 160, 14, 45, 22, batt, (ch != PsmChargerType_Unconnected));
}

// ─── Tab bar ──────────────────────────────────────────────

static void draw_tabs(SDL_Renderer *r, int cur) {
    int tab_w = W / PGS;
    SDL_Rect bg = {0, 51, W, 60};
    SDL_SetRenderDrawColor(r, color_bg2.r, color_bg2.g, color_bg2.b, 255);
    SDL_RenderFillRect(r, &bg);

    for (int i = 0; i < PGS; i++) {
        SDL_Rect btn = {i * tab_w, 51, tab_w, 60};
        if (i == cur) {
            // Selected tab background
            SDL_SetRenderDrawColor(r, color_bg3.r, color_bg3.g, color_bg3.b, 255);
            SDL_RenderFillRect(r, &btn);
            
            // Accent line at bottom
            SDL_Rect line = {i * tab_w + 10, 107, tab_w - 20, 4};
            SDL_SetRenderDrawColor(r, color_cyan.r, color_cyan.g, color_cyan.b, 255);
            SDL_RenderFillRect(r, &line);
            
            draw_text(r, font_sm, tab_names[cur_lang][i], i * tab_w + tab_w / 2, 68, color_white, 1);
        } else {
            draw_text(r, font_sm, tab_names[cur_lang][i], i * tab_w + tab_w / 2, 68, color_grey, 1);
        }
        
        // Tab separator
        if (i > 0) {
            SDL_SetRenderDrawColor(r, color_card_border.r, color_card_border.g, color_card_border.b, 255);
            SDL_RenderDrawLine(r, i * tab_w, 56, i * tab_w, 106);
        }
    }

    SDL_SetRenderDrawColor(r, color_card_border.r, color_card_border.g, color_card_border.b, 255);
    SDL_RenderDrawLine(r, 0, 111, W, 111);
}

// ─── Footer ───────────────────────────────────────────────

static void draw_footer(SDL_Renderer *r, int cur_page) {
    SDL_Rect bg = {0, 660, W, 60};
    SDL_SetRenderDrawColor(r, color_bg2.r, color_bg2.g, color_bg2.b, 255);
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, color_card_border.r, color_card_border.g, color_card_border.b, 255);
    SDL_RenderDrawLine(r, 0, 660, W, 660);

    const char *hint = s_footer_default[cur_lang];
    if (cur_page == 3)
        hint = s_footer_transfer[cur_lang];
    else if (cur_page == 6)
        hint = s_footer_tools[cur_lang];
    else if (cur_page == 7)
        hint = s_footer_about[cur_lang];
    else if (cur_page == 8)
        hint = s_footer_settings[cur_lang];
    else if (cur_page == 0 || cur_page == 1 || cur_page == 4 || cur_page == 8)
        hint = s_footer_scroll[cur_lang];

    draw_text(r, font_sm, hint, 20, 680, color_grey, 0);
    draw_text(r, font_sm, s_exit_hint[cur_lang], W - 100, 680, color_red, 0);
}

// ─── Pages ────────────────────────────────────────────────

// Scroll state for pages
static int about_scroll = 0;
static int about_scroll_max = 0;
static int tools_scroll = 0;
static int tools_scroll_max = 0;
static int perf_scroll = 0;
static int perf_scroll_max = 0;
static int storage_scroll = 0;
static int storage_scroll_max = 0;
static int net_scroll = 0;
static int net_scroll_max = 0;
static int ctrl_scroll = 0;
static int ctrl_scroll_max = 0;
static int system_scroll = 0;
static int system_scroll_max = 0;
static int settings_scroll = 0;
static int settings_scroll_max = 0;

// System Info
static void draw_pg0(SDL_Renderer *r) {
    char t[256];

    int content_bottom = 920;
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_h = content_bottom - view_top;
    system_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (system_scroll > system_scroll_max) system_scroll = system_scroll_max;
    if (system_scroll < 0) system_scroll = 0;

    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = system_scroll;

    draw_card(r, 40, 140 - sc, 580, 310, s_fw_hw[cur_lang]);
    SetSysFirmwareVersion fw = {0};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        snprintf(t, sizeof(t), "%d.%d.%d", fw.major, fw.minor, fw.micro);
        draw_key_value(r, s_firmware[cur_lang], t, 70, 210 - sc, color_white);
    }
    SetSysSerialNumber sn = {0};
    if (R_SUCCEEDED(setsysGetSerialNumber(&sn))) {
        draw_key_value(r, s_serial[cur_lang], sn.number, 70, 240 - sc, color_white);
        draw_key_value_wide(r, s_hardware[cur_lang], detect_hardware_type(sn.number), 70, 270 - sc, 140, color_purple);
    }
    int docked = appletGetOperationMode();
    draw_key_value(r, s_mode[cur_lang], docked ? s_docked[cur_lang] : s_handheld[cur_lang], 70, 300 - sc, docked ? color_green : color_cyan);
    draw_key_value(r, s_arch[cur_lang], s_arch_val[cur_lang], 70, 330 - sc, color_white);
    
    SetSysDeviceNickName nick = {0};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0]) {
        draw_key_value(r, s_dev_name[cur_lang], nick.nickname, 70, 360 - sc, color_yellow);
    }

    SetRegion region;
    if (R_SUCCEEDED(setGetRegionCode(&region))) {
        if ((int)region >= 0 && (int)region <= 5)
            draw_key_value(r, s_region[cur_lang], s_country_names[cur_lang][(int)region], 70, 390 - sc, color_white);
    }

    u64 lang = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&lang))) {
        SetLanguage langCode;
        if (R_SUCCEEDED(setMakeLanguage(lang, &langCode))) {
            if ((int)langCode >= 0 && (int)langCode < 18)
                draw_key_value(r, s_sys_lang[cur_lang], s_lang_names_18[cur_lang][(int)langCode], 70, 420 - sc, color_white);
        }
    }

    draw_card(r, 660, 140 - sc, 580, 280, s_batt_power[cur_lang]);
    u32 batt = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batt))) {
        psmGetChargerType(&ch);
        snprintf(t, sizeof(t), "%u%%", batt);
        draw_key_value(r, s_batt_level[cur_lang], t, 690, 210 - sc, get_usage_color(100 - batt));
        draw_key_value(r, s_charging_label[cur_lang], ch != PsmChargerType_Unconnected ? s_charging[cur_lang] : s_discharging[cur_lang], 690, 240 - sc, ch != PsmChargerType_Unconnected ? color_green : color_yellow);
        
        const char *chType = s_none[cur_lang];
        if (ch == PsmChargerType_EnoughPower) chType = s_ac_adapter[cur_lang];
        else if (ch == PsmChargerType_LowPower) chType = s_usb_slow[cur_lang];
        draw_key_value(r, s_charger[cur_lang], chType, 690, 270 - sc, color_white);

        draw_text(r, font_sm, s_capacity[cur_lang], 690, 310 - sc, color_grey, 0);
        draw_progress_bar(r, 820, 312 - sc, 380, 16, batt / 100.f, get_usage_color(100 - batt), color_dark_grey);
    }

    HidPowerInfo jc_left = {0}, jc_right = {0};
    hidGetNpadPowerInfoSplit(HidNpadIdType_No1, &jc_left, &jc_right);
    
    snprintf(t, sizeof(t), "%s", get_joycon_battery_str(jc_left.battery_level));
    draw_key_value(r, s_jc_l[cur_lang], t, 690, 350 - sc, get_joycon_battery_color(jc_left.battery_level));
    
    snprintf(t, sizeof(t), "%s", get_joycon_battery_str(jc_right.battery_level));
    draw_key_value(r, s_jc_r[cur_lang], t, 690, 380 - sc, get_joycon_battery_color(jc_right.battery_level));

    draw_card(r, 40, 460 - sc, 1200, 200, s_thermals[cur_lang]);
    
    s32 skin = 0;
    if (R_SUCCEEDED(tcInitialize())) {
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin))) {
            snprintf(t, sizeof(t), "%d.%d C", skin/1000, (skin%1000)/100);
            draw_key_value(r, s_skin_temp[cur_lang], t, 70, 530 - sc, get_temp_color(skin));
            
            u32 tpct = 0;
            if (skin < 25000) tpct = 0;
            else if (skin > 70000) tpct = 100;
            else tpct = (u32)((skin - 25000) * 100 / 45000);
            
            draw_progress_bar(r, 270, 532 - sc, 300, 16, tpct / 100.f, get_temp_color(skin), color_dark_grey);

            const char *th_state = s_normal[cur_lang];
            SDL_Color th_col = color_green;
            if (skin >= 55000) { th_state = s_hot[cur_lang]; th_col = color_red; }
            else if (skin >= 40000) { th_state = s_warm[cur_lang]; th_col = color_yellow; }
            draw_key_value(r, s_thermal_state[cur_lang], th_state, 70, 570 - sc, th_col);
        }
        tcExit();
    }

    float br = 0;
    if (R_SUCCEEDED(brightness_read(&br))) {
        snprintf(t, sizeof(t), "%s: %.0f%%   ", s_brightness[cur_lang], br*100);
        draw_text(r, font_sm, t, 690, 530 - sc, color_yellow, 0);
        draw_progress_bar(r, 920, 532 - sc, 280, 16, br, color_yellow, color_dark_grey);
    } else if (lbl_emulator) {
        draw_key_value(r, s_brightness[cur_lang], s_na_emu[cur_lang], 690, 530 - sc, color_grey);
    }

    draw_key_value(r, s_resolution[cur_lang], s_res_val[cur_lang], 690, 570 - sc, color_cyan);
    snprintf(t, sizeof(t), "%.0f Hz", 60.0f);
    draw_key_value(r, s_refresh_rate[cur_lang], t, 690, 600 - sc, color_white);

    draw_card(r, 40, 700 - sc, 580, 160, s_sys_uptime[cur_lang]);
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
    draw_key_value(r, s_app_uptime[cur_lang], t, 70, 770 - sc, color_cyan);

    // Scroll indicators
    if (system_scroll_max > 0) {
        if (system_scroll > 0)
            draw_text(r, font_sm, s_scroll_up[cur_lang], W - 190, 144, color_cyan, 0);
        if (system_scroll < system_scroll_max)
            draw_text(r, font_sm, s_scroll_down[cur_lang], W - 210, 624, color_cyan, 0);
    }
    SDL_RenderSetClipRect(r, NULL);
}

// File browser state (Storage page)
#define FB_MAX_ENTRIES 512
static int fb_active = 0;
static int fb_scroll = 0;
static int fb_count = 0;
static char fb_path[1024];
static char fb_entries[FB_MAX_ENTRIES][260];
static u32 fb_sizes[FB_MAX_ENTRIES];
static int fb_is_dir[FB_MAX_ENTRIES];
static int fb_selected = 0;

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
        u32 sz = 0;
        if (stat(fp, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            sz = (u32)(st.st_size);
        }
        snprintf(fb_entries[fb_count], sizeof(fb_entries[0]), "%s%s", e->d_name, is_dir ? "/" : "");
        fb_sizes[fb_count] = sz;
        fb_is_dir[fb_count] = is_dir;
        fb_count++;
    }
    closedir(d);

    // Sort: directories first (A-Z), then files (A-Z)
    if (fb_count > 1) {
        int *idx = malloc(fb_count * sizeof(int));
        if (idx) {
            for (int i = 0; i < fb_count; i++) idx[i] = i;
            // Simple bubble sort
            for (int i = 0; i < fb_count-1; i++) {
                for (int j = 0; j < fb_count-1-i; j++) {
                    int a = idx[j], b = idx[j+1];
                    int swap = 0;
                    if (fb_is_dir[a] && !fb_is_dir[b]) swap = 0;
                    else if (!fb_is_dir[a] && fb_is_dir[b]) swap = 1;
                    else if (strcasecmp(fb_entries[a], fb_entries[b]) > 0) swap = 1;
                    if (swap) {
                        int tmp = idx[j]; idx[j] = idx[j+1]; idx[j+1] = tmp;
                    }
                }
            }
            // Reorder arrays
            char tmp_entries[FB_MAX_ENTRIES][260];
            u32 tmp_sizes[FB_MAX_ENTRIES];
            int tmp_is_dir[FB_MAX_ENTRIES];
            for (int i = 0; i < fb_count; i++) {
                strncpy(tmp_entries[i], fb_entries[idx[i]], sizeof(tmp_entries[i])-1);
                tmp_sizes[i] = fb_sizes[idx[i]];
                tmp_is_dir[i] = fb_is_dir[idx[i]];
            }
            memcpy(fb_entries, tmp_entries, sizeof(fb_entries[0]) * fb_count);
            memcpy(fb_sizes, tmp_sizes, sizeof(fb_sizes[0]) * fb_count);
            memcpy(fb_is_dir, tmp_is_dir, sizeof(fb_is_dir[0]) * fb_count);
            free(idx);
        }
    }

    fb_active = 1;
}

// Six-axis sensor handles for gyro/accel
static HidSixAxisSensorHandle sixaxis_handles[2];
static bool sixaxis_init_ok = false;

// MAC address
static u8 mac_addr[6] = {0};
static bool mac_addr_valid = false;

// Fan control
static int fan_speed_pct = 50;

// Overlay/sysmodule mode
static int app_mode = 0; // 0=normal, 1=overlay, 2=sysmodule

// Storage button Y positions (dynamic, updated on draw)
static int storage_test_btn_y = 0;
static int storage_fb_btn_y = 0;

// Popup for "En developpement" messages
static bool popup_active = false;
static char popup_text[128] = {0};
static u64 popup_start_tick = 0;

// Storage (cleaner reorganized layout)
static void draw_pg1(SDL_Renderer *r) {
    char t[256];

    // Card 3 bottom: nand_y max is ~470, file breakdown card ends at 710 -> need more content_bottom
    int content_bottom = 960;
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_h = content_bottom - view_top;
    storage_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (storage_scroll > storage_scroll_max) storage_scroll = storage_scroll_max;
    if (storage_scroll < 0) storage_scroll = 0;
    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = storage_scroll;

    draw_card(r, 40, 140 - sc, 580, 360, s_sd_card[cur_lang]);
    FsFileSystem sd;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&sd, "/", &f)) && R_SUCCEEDED(fsFsGetTotalSpace(&sd, "/", &tot)) && tot > 0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            SDL_Color clr = get_usage_color(pct);

            snprintf(t, sizeof(t), "%.2f GB", tot/1.0e9);
            draw_key_value(r, s_total[cur_lang], t, 70, 210 - sc, color_white);
            snprintf(t, sizeof(t), "%.2f GB (%u%%)", used/1.0e9, pct);
            draw_key_value(r, s_used[cur_lang], t, 70, 240 - sc, clr);
            snprintf(t, sizeof(t), "%.2f GB", f/1.0e9);
            draw_key_value(r, s_free[cur_lang], t, 70, 270 - sc, color_green);

            draw_text(r, font_sm, s_storage_usage[cur_lang], 70, 310 - sc, color_grey, 0);
            draw_progress_bar(r, 250, 312 - sc, 340, 20, (float)used / tot, clr, color_dark_grey);

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
                draw_text(r, font_sm, t, 70, 350 - sc, color_grey, 0);
            }
        } else {
            draw_text(r, font_sm, s_sd_fail[cur_lang], 70, 210 - sc, color_red, 0);
        }
        if (sd_speed_result > 0) {
            snprintf(t, sizeof(t), s_read_speed_fmt[cur_lang], sd_speed_result);
            draw_text(r, font_sm, t, 70, 380 - sc, color_green, 0);
        } else if (sd_speed_result == -2) {
            draw_text(r, font_sm, s_read_testing[cur_lang], 70, 380 - sc, color_yellow, 0);
        } else if (sd_speed_result < 0) {
            draw_text(r, font_sm, s_read_error[cur_lang], 70, 380 - sc, color_red, 0);
        } else {
            draw_text(r, font_sm, s_read_untested[cur_lang], 70, 380 - sc, color_grey, 0);
        }
        fsFsClose(&sd);
    } else {
        draw_text(r, font_sm, s_sd_no_mount[cur_lang], 70, 210 - sc, color_red, 0);
    }

    draw_card(r, 660, 140 - sc, 580, 360, s_nand_parts[cur_lang]);
    int nand_y = 210;
    FsFileSystem ns;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&ns, FsBisPartitionId_System, ""))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&ns, "/", &f)) && R_SUCCEEDED(fsFsGetTotalSpace(&ns, "/", &tot)) && tot > 0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            SDL_Color clr = get_usage_color(pct);
            draw_text(r, font_sm, s_sys_part[cur_lang], 690, nand_y - sc, color_cyan, 0);
            nand_y += 24;
            snprintf(t, sizeof(t), "%.2f / %.2f GB  (%u%%)", used/1.0e9, tot/1.0e9, pct);
            draw_text(r, font_sm, t, 690, nand_y - sc, color_white, 0);
            nand_y += 22;
            draw_progress_bar(r, 690, nand_y - sc, 510, 12, (float)used / tot, clr, color_dark_grey);
            nand_y += 30;
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
            draw_text(r, font_sm, s_user_part[cur_lang], 690, nand_y - sc, color_cyan, 0);
            nand_y += 24;
            snprintf(t, sizeof(t), "%.2f / %.2f GB  (%u%%)", used/1.0e9, tot/1.0e9, pct);
            draw_text(r, font_sm, t, 690, nand_y - sc, color_white, 0);
            nand_y += 22;
            draw_progress_bar(r, 690, nand_y - sc, 510, 12, (float)used / tot, clr, color_dark_grey);
            nand_y += 30;
        }
        fsFsClose(&nu);
    }
    storage_test_btn_y = nand_y;
    draw_rounded_box(r, 690, nand_y - sc, 240, 40, 6, color_card_border);
    draw_text(r, font_sm, s_touch_test[cur_lang], 810, nand_y + 8 - sc, color_cyan, 1);
    nand_y += 56;
    storage_fb_btn_y = nand_y;
    draw_rounded_box(r, 690, nand_y - sc, 260, 40, 6, color_card_border);
    draw_text(r, font_sm, s_open_fb[cur_lang], 820, nand_y + 8 - sc, color_cyan, 1);
    nand_y += 56;

    draw_card(r, 40, 530 - sc, 1200, 180, s_sd_breakdown[cur_lang]);
    DIR *sd_root = opendir("sdmc:/");
    if (sd_root) {
        int dirs = 0, pics = 0, vids = 0, music = 0, docs = 0, zips = 0, nros = 0, roms = 0, other = 0;
        struct dirent *e;
        while ((e = readdir(sd_root)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char fp[300]; snprintf(fp, sizeof(fp), "sdmc:/%s", e->d_name);
            struct stat st;
            if (stat(fp, &st) == 0 && S_ISDIR(st.st_mode)) { dirs++; continue; }
            const char *ext = strrchr(e->d_name, '.');
            if (!ext) { other++; continue; }
            ext++;
            if (strcmp(ext, "jpg")==0||strcmp(ext, "jpeg")==0||strcmp(ext, "png")==0||strcmp(ext, "bmp")==0) pics++;
            else if (strcmp(ext, "mp4")==0||strcmp(ext, "mov")==0||strcmp(ext, "avi")==0||strcmp(ext, "mkv")==0) vids++;
            else if (strcmp(ext, "mp3")==0||strcmp(ext, "flac")==0||strcmp(ext, "wav")==0) music++;
            else if (strcmp(ext, "txt")==0||strcmp(ext, "pdf")==0||strcmp(ext, "md")==0) docs++;
            else if (strcmp(ext, "zip")==0||strcmp(ext, "rar")==0||strcmp(ext, "7z")==0) zips++;
            else if (strcmp(ext, "nro")==0) nros++;
            else if (strcmp(ext, "xci")==0||strcmp(ext, "nsp")==0||strcmp(ext, "nsz")==0) roms++;
            else other++;
        }
        closedir(sd_root);

        int bx = 70;
        int by = 590 - sc;
        snprintf(t, sizeof(t), s_folders[cur_lang], dirs);
        draw_text(r, font_sm, t, bx, by, color_yellow, 0); bx += 180;
        snprintf(t, sizeof(t), s_nro_count[cur_lang], nros);
        draw_text(r, font_sm, t, bx, by, color_cyan, 0); bx += 150;
        snprintf(t, sizeof(t), s_images[cur_lang], pics);
        draw_text(r, font_sm, t, bx, by, color_white, 0); bx += 160;
        snprintf(t, sizeof(t), s_videos[cur_lang], vids);
        draw_text(r, font_sm, t, bx, by, color_white, 0); bx += 160;

        bx = 70; by = 620 - sc;
        snprintf(t, sizeof(t), s_music[cur_lang], music);
        draw_text(r, font_sm, t, bx, by, color_white, 0); bx += 160;
        snprintf(t, sizeof(t), s_games[cur_lang], roms);
        draw_text(r, font_sm, t, bx, by, color_purple, 0); bx += 160;
        snprintf(t, sizeof(t), s_docs[cur_lang], docs);
        draw_text(r, font_sm, t, bx, by, color_white, 0); bx += 150;
        snprintf(t, sizeof(t), s_other[cur_lang], other);
        draw_text(r, font_sm, t, bx, by, color_grey, 0); bx += 150;

        DIR *sw_dir = opendir("sdmc:/switch");
        int sw_items = 0;
        if (sw_dir) {
            struct dirent *e2;
            while ((e2 = readdir(sw_dir)) != NULL) {
                if (strcmp(e2->d_name, ".")==0||strcmp(e2->d_name, "..")==0) continue;
                sw_items++;
            }
            closedir(sw_dir);
        }
        snprintf(t, sizeof(t), s_homebrew_count[cur_lang], sw_items);
        draw_text(r, font_sm, t, 70, 660 - sc, color_cyan, 0);
    }

    if (storage_scroll_max > 0) {
        if (storage_scroll > 0)
            draw_text(r, font_sm, s_scroll_up[cur_lang], W - 190, 144, color_cyan, 0);
        if (storage_scroll < storage_scroll_max)
            draw_text(r, font_sm, s_scroll_down[cur_lang], W - 210, 624, color_cyan, 0);
    }
    SDL_RenderSetClipRect(r, NULL);
}

// Connection quality history
#define WIFI_HIST_SIZE 60
static int wifi_hist[WIFI_HIST_SIZE] = {0};
static int wifi_hist_pos = 0;
static int wifi_hist_count = 0;

// Network
static void draw_pg2(SDL_Renderer *r) {
    char t[128];

    int content_bottom = 740;
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_h = content_bottom - view_top;
    net_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (net_scroll > net_scroll_max) net_scroll = net_scroll_max;
    if (net_scroll < 0) net_scroll = 0;
    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = net_scroll;

    draw_card(r, 40, 140 - sc, 580, 520, s_ip_config[cur_lang]);
    u32 ip=0, msk=0, gw=0, d1=0, d2=0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip,&msk,&gw,&d1,&d2)) && ip) {
        snprintf(t,sizeof(t),"%u.%u.%u.%u",ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
        draw_key_value(r, s_ip_addr[cur_lang], t, 70, 210 - sc, color_green);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",msk&0xFF,(msk>>8)&0xFF,(msk>>16)&0xFF,(msk>>24)&0xFF);
        draw_key_value(r, s_subnet[cur_lang], t, 70, 240 - sc, color_white);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",gw&0xFF,(gw>>8)&0xFF,(gw>>16)&0xFF,(gw>>24)&0xFF);
        draw_key_value(r, s_gateway[cur_lang], t, 70, 270 - sc, color_white);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",d1&0xFF,(d1>>8)&0xFF,(d1>>16)&0xFF,(d1>>24)&0xFF);
        draw_key_value(r, s_primary_dns[cur_lang], t, 70, 300 - sc, color_white);
        if (d2) {
            snprintf(t,sizeof(t),"%u.%u.%u.%u",d2&0xFF,(d2>>8)&0xFF,(d2>>16)&0xFF,(d2>>24)&0xFF);
            draw_key_value(r, s_secondary_dns[cur_lang], t, 70, 330 - sc, color_white);
        } else {
            draw_key_value(r, s_secondary_dns[cur_lang], s_none[cur_lang], 70, 330 - sc, color_grey);
        }

        char hostname[64] = {0};
        if (gethostname(hostname, sizeof(hostname)) == 0 && hostname[0])
            draw_key_value(r, s_hostname[cur_lang], hostname, 70, 410 - sc, color_white);

        if (mac_addr_valid) {
            snprintf(t, sizeof(t), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
            draw_key_value(r, s_mac_addr[cur_lang], t, 70, 440 - sc, color_yellow);
        }

        draw_key_value(r, s_adapter[cur_lang], "Broadcom BCM4356", 70, 470 - sc, color_white);
        draw_text(r, font_sm, "Dual-band 802.11ac Wi-Fi + Bluetooth 4.1", 70, 500 - sc, color_grey, 0);

        if (ftp_on) {
            snprintf(t, sizeof(t), "%s://%u.%u.%u.%u:%d", ftp_mode ? "ftpd" : "ftp", ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF, ftp_get_port());
            draw_key_value(r, "FTP Access", t, 70, 520 - sc, color_yellow);
        }
    } else {
        draw_text(r, font_sm, s_not_connected[cur_lang], 70, 210 - sc, color_red, 0);
        draw_text(r, font_sm, s_go_to_settings[cur_lang], 70, 240 - sc, color_grey, 0);
        draw_text(r, font_sm, s_supports_both[cur_lang], 70, 270 - sc, color_grey, 0);
    }

    draw_card(r, 660, 140 - sc, 580, 310, s_conn_details[cur_lang]);
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
            snprintf(t, sizeof(t), "%d/3", ws);
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
            draw_key_value(r, s_connected_for[cur_lang], t, 690, 330 - sc, color_white);
        }
        if (ct == 1) {
            draw_key_value(r, s_band[cur_lang], ws >= 2 ? "5 GHz" : "2.4 GHz", 690, 360 - sc, ws >= 2 ? color_green : color_yellow);
        }
        if (cs == 4)
            draw_key_value(r, s_status[cur_lang], s_full_internet[cur_lang], 690, 390 - sc, color_green);
        else if (cs == 3)
            draw_key_value(r, s_status[cur_lang], s_local_only[cur_lang], 690, 390 - sc, color_yellow);
        else
            draw_key_value(r, s_status[cur_lang], s_no_conn[cur_lang], 690, 390 - sc, color_red);
    }

    draw_card(r, 660, 470 - sc, 580, 230, s_wifi_diag[cur_lang]);
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

                wifi_hist[wifi_hist_pos] = qual;
                wifi_hist_pos = (wifi_hist_pos + 1) % WIFI_HIST_SIZE;
                if (wifi_hist_count < WIFI_HIST_SIZE) wifi_hist_count++;
                draw_text(r, font_xs, s_signal_history[cur_lang], 690, 625 - sc, color_grey, 0);
                int chart_x = 870, chart_y = 622 - sc, chart_w = 330, chart_h = 30;
                draw_rounded_rect(r, chart_x, chart_y, chart_w, chart_h, 3, color_dark_grey);
                int hc = wifi_hist_count;
                if (hc > WIFI_HIST_SIZE) hc = WIFI_HIST_SIZE;
                for (int si = 0; si < hc - 1; si++) {
                    int x1 = chart_x + chart_w - (si+1) * chart_w / hc;
                    int x2 = chart_x + chart_w - (si+2) * chart_w / hc;
                    int y1 = chart_y + chart_h - wifi_hist[(wifi_hist_pos - si - 1 + WIFI_HIST_SIZE) % WIFI_HIST_SIZE] * chart_h / 100;
                    int y2 = chart_y + chart_h - wifi_hist[(wifi_hist_pos - si - 2 + WIFI_HIST_SIZE) % WIFI_HIST_SIZE] * chart_h / 100;
                    lineRGBA(r, x1, y1, x2, y2, color_cyan.r, color_cyan.g, color_cyan.b, 200);
                }
            }
        } else {
            draw_text(r, font_sm, s_wifi_not_active[cur_lang], 690, 540 - sc, color_grey, 0);
        }
        wlaninfExit();
    } else {
        draw_text(r, font_sm, s_wlan_na[cur_lang], 690, 540 - sc, color_grey, 0);
    }

    SDL_RenderSetClipRect(r, NULL);

    if (net_scroll_max > 0) {
        if (net_scroll > 0)
            draw_text(r, font_sm, s_scroll_up[cur_lang], W - 190, 144, color_cyan, 0);
        if (net_scroll < net_scroll_max)
            draw_text(r, font_sm, s_scroll_down[cur_lang], W - 210, 624, color_cyan, 0);
    }
}

// FTP Server
static void draw_pg3(SDL_Renderer *r) {
    char t[1024];
    char ipstr[32] = "0.0.0.0";
    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip)
        snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);

    const char *mode_name = ftp_mode ? "FTPD" : "FTP";
    char card1_title[64];
    snprintf(card1_title, sizeof(card1_title), s_wifi_transfer[cur_lang], mode_name);
    draw_card(r, 40, 140, 580, 310, card1_title);
    
    if (ftp_on) {
        draw_rounded_box(r, 70, 210, 120, 32, 4, color_green);
        draw_text(r, font_sm, s_running[cur_lang], 130, 216, color_bg, 1);
    } else {
        draw_rounded_box(r, 70, 210, 120, 32, 4, color_red);
        draw_text(r, font_sm, s_stopped[cur_lang], 130, 216, color_white, 1);
    }

    snprintf(t, sizeof(t), "%s://%s:%d", ftp_mode ? "ftpd" : "ftp", ipstr, ftp_get_port());
    draw_key_value(r, s_address[cur_lang], t, 70, 260, color_cyan);
    draw_key_value(r, s_current_dir[cur_lang], ftp_cwd, 70, 290, color_yellow);
    snprintf(t, sizeof(t), "%u total (%u up, %u down)", ftp_xfer_count, ftp_upload_count, ftp_download_count);
    draw_key_value(r, s_transfers[cur_lang], t, 70, 320, color_white);
    if (ftp_bytes_total > 0) {
        if (ftp_bytes_total > 1073741824ULL)
            snprintf(t, sizeof(t), "%.2f GB", ftp_bytes_total / 1.0e9);
        else if (ftp_bytes_total > 1048576ULL)
            snprintf(t, sizeof(t), "%.2f MB", ftp_bytes_total / 1.0e6);
        else
            snprintf(t, sizeof(t), "%llu KB", (unsigned long long)(ftp_bytes_total / 1024));
        draw_key_value(r, s_data_total[cur_lang], t, 70, 350, color_white);
    }
    int mode_sel_x = 70, mode_sel_y = 390;
    SDL_Color col_ftp = ftp_mode ? color_grey : color_white;
    SDL_Color col_ftpd = ftp_mode ? color_white : color_grey;
    SDL_Color col_bg_ftp = ftp_mode ? color_card_border : color_cyan;
    SDL_Color col_bg_ftpd = ftp_mode ? color_cyan : color_card_border;
    draw_rounded_box(r, mode_sel_x, mode_sel_y, 46, 24, 4, col_bg_ftp);
    draw_rounded_rect(r, mode_sel_x, mode_sel_y, 46, 24, 4, color_card_border);
    draw_text(r, font_sm, "FTP", mode_sel_x + 23, mode_sel_y + 2, col_ftp, 1);
    draw_rounded_box(r, mode_sel_x + 50, mode_sel_y, 52, 24, 4, col_bg_ftpd);
    draw_rounded_rect(r, mode_sel_x + 50, mode_sel_y, 52, 24, 4, color_card_border);
    draw_text(r, font_sm, "FTPD", mode_sel_x + 76, mode_sel_y + 2, col_ftpd, 1);
    draw_text(r, font_sm, s_toggle_wifi[cur_lang], 70, 420, color_grey, 0);

    draw_card(r, 660, 140, 580, 310, s_mtp_title[cur_lang]);
    if (mtp_on) {
        draw_rounded_box(r, 690, 210, 120, 32, 4, color_green);
        draw_text(r, font_sm, s_active[cur_lang], 750, 216, color_bg, 1);
    } else {
        draw_rounded_box(r, 690, 210, 120, 32, 4, color_red);
        draw_text(r, font_sm, s_stopped[cur_lang], 750, 216, color_white, 1);
    }
    draw_key_value(r, s_protocol[cur_lang], s_mtp_protocol[cur_lang], 690, 260, color_cyan);
    snprintf(t, sizeof(t), s_mtp_xfers_fmt[cur_lang], (unsigned long long)mtp_xfer_count);
    draw_key_value(r, s_transfers[cur_lang], t, 690, 290, color_white);
    if (mtp_bytes_total > 0) {
        if (mtp_bytes_total > 1073741824ULL)
            snprintf(t, sizeof(t), "%.2f GB", mtp_bytes_total / 1.0e9);
        else if (mtp_bytes_total > 1048576ULL)
            snprintf(t, sizeof(t), "%.2f MB", mtp_bytes_total / 1.0e6);
        else
            snprintf(t, sizeof(t), "%llu KB", (unsigned long long)(mtp_bytes_total / 1024));
        draw_key_value(r, s_data_total[cur_lang], t, 690, 320, color_white);
    }
    draw_text(r, font_sm, s_mtp_desc[cur_lang], 690, 365, color_grey, 0);

    if (mtp_xfer_active) {
        if (!mtp_xfer_start_tick) mtp_xfer_start_tick = armGetSystemTick();

        draw_rounded_box(r, 690, 375, 500, 10, 3, color_dark_grey);
        u64 done = mtp_xfer_done;
        u64 total = mtp_xfer_total;
        float pct = (total > 0) ? (float)done / total : 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        if (pct > 0.0f)
            draw_rounded_box(r, 690, 375, (int)(pct * 500), 10, 3, color_green);

        const char *unit = "B";
        double ddone = (double)done, dtotal = (double)total;
        if (dtotal > 1073741824.0) { ddone /= 1.073741824e9; dtotal /= 1.073741824e9; unit = "GB"; }
        else if (dtotal > 1048576.0) { ddone /= 1.048576e6; dtotal /= 1.048576e6; unit = "MB"; }
        else if (dtotal > 1024.0) { ddone /= 1024.0; dtotal /= 1024.0; unit = "KB"; }
        snprintf(t, sizeof(t), "%s  %.1f/%.1f %s (%d%%)", (const char*)mtp_xfer_filename,
            ddone, dtotal, unit, (int)(pct * 100));
        draw_text(r, font_xs, t, 690, 390, color_white, 0);
    } else {
        mtp_xfer_start_tick = 0;
    }

    draw_text(r, font_sm, s_toggle_mtp[cur_lang], 690, 425, color_grey, 0);

    draw_card(r, 40, 470, 1200, 160, s_activity_log[cur_lang]);
    mutexLock(&ftp_log_mtx);
    mutexLock(&mtp_log_mtx);
    int py = 530, shown = 0;
    for (unsigned int i = (mtp_log_idx > 4 ? mtp_log_idx - 4 : 0); i < mtp_log_idx && shown < 3; i++) {
        draw_text(r, font_sm, "[MTP]", 70, py, color_purple, 0);
        draw_text(r, font_sm, mtp_log[i % 4], 120, py, color_white, 0);
        py += 22; shown++;
    }
    int ftp_start = ftp_log_idx > FTP_LOG_MAX ? ftp_log_idx - FTP_LOG_MAX : 0;
    for (int i = ftp_start; i < (int)ftp_log_idx && shown < 6; i++) {
        draw_text(r, font_sm, "[FTP]", 70, py, color_cyan, 0);
        draw_text(r, font_sm, ftp_log[i % FTP_LOG_MAX], 120, py, color_white, 0);
        py += 22; shown++;
    }
    mutexUnlock(&mtp_log_mtx);
    mutexUnlock(&ftp_log_mtx);
    if (!shown)
        draw_text(r, font_sm, s_no_activity[cur_lang], 70, 530, color_grey, 0);
}

// Performance Clocks - Redesigned layout
static void draw_pg4(SDL_Renderer *r) {
    char t[256];

    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_bottom = 700;
    int content_h = content_bottom - view_top;
    perf_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (perf_scroll > perf_scroll_max) perf_scroll = perf_scroll_max;
    if (perf_scroll < 0) perf_scroll = 0;

    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = perf_scroll;

    // Collect hardware data
    ClkrstSession cc, cg, cm;
    u32 cpu=0, gpu=0, mem=0;
    s32 skin = 0;
    bool clk_ok = false;
    if (R_SUCCEEDED(clkrstOpenSession(&cc,(PcvModuleId)PcvModule_CpuBus,3)) &&
        R_SUCCEEDED(clkrstOpenSession(&cg,(PcvModuleId)PcvModule_GPU,3)) &&
        R_SUCCEEDED(clkrstOpenSession(&cm,(PcvModuleId)PcvModule_EMC,3))) {
        clkrstGetClockRate(&cc,&cpu);
        clkrstGetClockRate(&cg,&gpu);
        clkrstGetClockRate(&cm,&mem);
        clk_ok = true;
        clkrstCloseSession(&cc);
        clkrstCloseSession(&cg);
        clkrstCloseSession(&cm);
    }
    if (R_SUCCEEDED(tcInitialize())) {
        tcGetSkinTemperatureMilliC(&skin);
        tcExit();
    }

    u32 cpu_mhz = cpu/1000000, gpu_mhz = gpu/1000000, mem_mhz = mem/1000000;

    // Update history buffers
    {
        int cpu_pct = clk_ok ? (int)((float)cpu_mhz / 1785.0f * 100.0f) : 0;
        if (cpu_pct < 0) cpu_pct = 0;
        if (cpu_pct > 100) cpu_pct = 100;
        cpu_hist[cpu_hist_pos] = cpu_pct;
        cpu_hist_pos = (cpu_hist_pos + 1) % PERF_HIST_SIZE;
        if (cpu_hist_count < PERF_HIST_SIZE) cpu_hist_count++;
    }
    {
        u32 tpct = 0;
        if (skin < 25000) tpct = 0;
        else if (skin > 70000) tpct = 100;
        else tpct = (skin - 25000) * 100 / 45000;
        temp_hist[temp_hist_pos] = (int)tpct;
        temp_hist_pos = (temp_hist_pos + 1) % PERF_HIST_SIZE;
        if (temp_hist_count < PERF_HIST_SIZE) temp_hist_count++;
    }
    fps_hist[fps_hist_pos] = current_fps;
    fps_hist_pos = (fps_hist_pos + 1) % FPS_HIST_SIZE;
    if (fps_hist_count < FPS_HIST_SIZE) fps_hist_count++;

    u64 mem_total = 0, mem_used = 0;
    bool mem_ok = R_SUCCEEDED(svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
                  R_SUCCEEDED(svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0));
    float mem_pct_f = mem_ok && mem_total > 0 ? (float)mem_used / mem_total : 0;
    {
        int mem_pct = (int)(mem_pct_f * 100.0f);
        if (mem_pct < 0) mem_pct = 0;
        if (mem_pct > 100) mem_pct = 100;
        mem_hist[mem_hist_pos] = mem_pct;
        mem_hist_pos = (mem_hist_pos + 1) % PERF_HIST_SIZE;
        if (mem_hist_count < PERF_HIST_SIZE) mem_hist_count++;
    }
    // Alerts
    if (skin >= temp_alert_millic) { if (!temp_alert_notified) { add_alert("ALERT: High Temp!"); temp_alert_notified = true; } }
    else if (skin < temp_alert_millic - 5000) { temp_alert_notified = false; }
    if (mem_pct_f > mem_alert_frac) { if (!mem_alert_notified) { add_alert("ALERT: High Mem Usage!"); mem_alert_notified = true; } }
    else if (mem_pct_f < mem_alert_frac - 0.05f) { mem_alert_notified = false; }

    // ═══════════════════════════════════════════════════════
    // ROW 1: Live Clock Dashboard (3 gauges side by side)
    // ═══════════════════════════════════════════════════════
    draw_card(r, 40, 140 - sc, 1200, 115, s_live_clocks[cur_lang]);

    if (clk_ok) {
        draw_rounded_box(r, 70, 175 - sc, 370, 68, 6, color_bg3);
        draw_text(r, font_md, s_cpu_clock[cur_lang], 85, 178 - sc, color_cyan, 0);
        snprintf(t, sizeof(t), "%u / 1785 MHz", cpu_mhz);
        draw_text(r, font_sm, t, 85, 205 - sc, color_white, 0);
        draw_progress_bar(r, 85, 228 - sc, 340, 10, (float)cpu_mhz / 1785.0f, color_cyan, color_dark_grey);

        draw_rounded_box(r, 455, 175 - sc, 370, 68, 6, color_bg3);
        draw_text(r, font_md, s_gpu_clock[cur_lang], 470, 178 - sc, color_yellow, 0);
        snprintf(t, sizeof(t), "%u / 921 MHz", gpu_mhz);
        draw_text(r, font_sm, t, 470, 205 - sc, color_white, 0);
        draw_progress_bar(r, 470, 228 - sc, 340, 10, (float)gpu_mhz / 921.0f, color_yellow, color_dark_grey);

        draw_rounded_box(r, 840, 175 - sc, 370, 68, 6, color_bg3);
        draw_text(r, font_md, s_mem_bus[cur_lang], 855, 178 - sc, color_green, 0);
        snprintf(t, sizeof(t), "%u / 1600 MHz", mem_mhz);
        draw_text(r, font_sm, t, 855, 205 - sc, color_white, 0);
        draw_progress_bar(r, 855, 228 - sc, 340, 10, (float)mem_mhz / 1600.0f, color_green, color_dark_grey);
    } else {
        draw_text(r, font_sm, s_failed_read_clocks[cur_lang], 70, 190 - sc, color_red, 0);
    }

    // ═══════════════════════════════════════════════════════
    // ROW 2: FPS line chart + load indicators
    // ═══════════════════════════════════════════════════════
    int chart_y = 270;
    int chart_h = 175;

    // ── FPS History (connected line + fill) ──
    draw_card(r, 40, chart_y - sc, 780, chart_h, s_fps_history[cur_lang]);
    int hc = fps_hist_count > 60 ? 60 : fps_hist_count;
    int fps_cx = 70, fps_cy = chart_y + 28 - sc, fps_cw = 720, fps_ch = 90;
    draw_rounded_rect(r, fps_cx, fps_cy, fps_cw, fps_ch, 3, color_dark_grey);

    for (int g = 0; g <= 3; g++) {
        int gy = fps_cy + (fps_ch * g / 3);
        lineRGBA(r, fps_cx, gy, fps_cx + fps_cw, gy, 100, 100, 100, 50);
    }
    int ref30y = fps_cy + fps_ch * 30 / 60;
    lineRGBA(r, fps_cx, ref30y, fps_cx + fps_cw, ref30y, 255, 100, 100, 120);
    draw_text(r, font_xs, "30", fps_cx + 3, ref30y - 8, color_red, 0);

    if (hc >= 2) {
        Sint16 xpts[FPS_HIST_SIZE+2], ypts[FPS_HIST_SIZE+2];
        int npts = hc;
        for (int i = 0; i < npts; i++) {
            int fv = fps_hist[(fps_hist_pos - npts + i + FPS_HIST_SIZE) % FPS_HIST_SIZE];
            if (fv > 60) fv = 60;
            if (fv < 0) fv = 0;
            xpts[i] = fps_cx + i * fps_cw / npts;
            ypts[i] = fps_cy + fps_ch - (fv * fps_ch / 60);
        }
        Sint16 fx[FPS_HIST_SIZE+4], fy[FPS_HIST_SIZE+4];
        for (int i = 0; i < npts; i++) { fx[i] = xpts[i]; fy[i] = ypts[i]; }
        fx[npts] = xpts[npts-1]; fy[npts] = fps_cy + fps_ch;
        fx[npts+1] = xpts[0]; fy[npts+1] = fps_cy + fps_ch;
        filledPolygonRGBA(r, fx, fy, npts+2, 0, 210, 255, 40);
        for (int i = 0; i < npts - 1; i++) {
            int fv1 = fps_hist[(fps_hist_pos - npts + i + FPS_HIST_SIZE) % FPS_HIST_SIZE];
            int fv2 = fps_hist[(fps_hist_pos - npts + i + 1 + FPS_HIST_SIZE) % FPS_HIST_SIZE];
            SDL_Color lc = (fv1 >= 55 && fv2 >= 55) ? color_green : ((fv1 >= 30 || fv2 >= 30) ? color_yellow : color_red);
            thickLineRGBA(r, xpts[i], ypts[i], xpts[i+1], ypts[i+1], 3, lc.r, lc.g, lc.b, 220);
        }
    }
    snprintf(t, sizeof(t), s_current_fps_fmt[cur_lang], current_fps);
    draw_text(r, font_md, t, 70, chart_y + chart_h - 45 - sc, current_fps >= 55 ? color_green : color_yellow, 0);

    // ── Load indicators (right) ──
    draw_card(r, 840, chart_y - sc, 400, chart_h, s_system_load[cur_lang]);
    if (clk_ok) {
        u32 cpu_pct = (u32)((float)cpu_mhz / 1785.0f * 100.0f);
        if (cpu_pct > 100) cpu_pct = 100;
        u32 gpu_pct = (u32)((float)gpu_mhz / 921.0f * 100.0f);
        if (gpu_pct > 100) gpu_pct = 100;
        u32 mem_pct_disp = (u32)(mem_pct_f * 100);
        if (mem_pct_disp > 100) mem_pct_disp = 100;

        int ly = chart_y + 55 - sc;
        draw_text(r, font_sm, "CPU", 870, ly, color_cyan, 0);
        draw_progress_bar(r, 930, ly + 2, 280, 14, cpu_pct / 100.f, color_cyan, color_dark_grey);
        snprintf(t, sizeof(t), "%u%%", cpu_pct);
        draw_text(r, font_sm, t, 1220, ly, color_cyan, 2);

        ly += 28;
        draw_text(r, font_sm, "GPU", 870, ly, color_yellow, 0);
        draw_progress_bar(r, 930, ly + 2, 280, 14, gpu_pct / 100.f, color_yellow, color_dark_grey);
        snprintf(t, sizeof(t), "%u%%", gpu_pct);
        draw_text(r, font_sm, t, 1220, ly, color_yellow, 2);

        ly += 28;
        draw_text(r, font_sm, "MEM", 870, ly, color_green, 0);
        draw_progress_bar(r, 930, ly + 2, 280, 14, mem_pct_disp / 100.f, color_green, color_dark_grey);
        snprintf(t, sizeof(t), "%u%%", mem_pct_disp);
        draw_text(r, font_sm, t, 1220, ly, color_green, 2);

        ly += 28;
        u32 score = (cpu_mhz * 10 + gpu_mhz * 5 + mem_mhz * 2) / 20;
        snprintf(t, sizeof(t), "Score: %u", score);
        SDL_Color scol = score > 1300 ? color_green : (score > 800 ? color_yellow : color_grey);
        draw_text(r, font_sm, t, 870, ly, scol, 0);
    } else {
        draw_text(r, font_sm, "No clock data", 870, chart_y + 55 - sc, color_grey, 0);
    }

    // ═══════════════════════════════════════════════════════
    // ROW 3: System Health (3 columns, lowered thermal text)
    // ═══════════════════════════════════════════════════════
    int row3_y = 460 - sc;
    int r3w = 385;
    int r3h = 165;

    // ── Thermal Status (lowered text) ──
    draw_card(r, 40, row3_y, r3w, r3h, s_thermal_status[cur_lang]);
    if (skin > 0) {
        snprintf(t, sizeof(t), "%d.%d C", skin/1000, (skin%1000)/100);
        draw_text(r, font_md, t, 70, row3_y + 52, get_temp_color(skin), 0);
        u32 tpct = skin < 25000 ? 0 : (skin > 70000 ? 100 : (skin - 25000) * 100 / 45000);
        draw_progress_bar(r, 210, row3_y + 56, 185, 16, tpct / 100.f, get_temp_color(skin), color_dark_grey);
        snprintf(t, sizeof(t), "%u%%", tpct);
        draw_text(r, font_sm, t, 395, row3_y + 56, color_grey, 2);
        const char *label = skin >= 55000 ? s_critical_limit[cur_lang] : (skin >= 45000 ? s_warm_hot[cur_lang] : s_cool[cur_lang]);
        draw_text(r, font_sm, label, 70, row3_y + 92, get_temp_color(skin), 0);
    } else {
        draw_text(r, font_sm, "N/A", 70, row3_y + 54, color_grey, 0);
    }

    // ── RAM Usage (enlarged card, lowered text/bar/pct) ──
    draw_card(r, 440, row3_y, r3w + 15, r3h, s_ram_usage[cur_lang]);
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
        draw_text(r, font_sm, t, 885, row3_y + 52, pcol, 0);
        int dock = appletGetOperationMode();
        draw_text(r, font_sm, dock ? s_docked_high[cur_lang] : s_handheld_throttled[cur_lang], 885, row3_y + 79, dock ? color_green : color_cyan, 0);
        u32 score = (cpu_mhz * 10 + gpu_mhz * 5 + mem_mhz * 2) / 20;
        snprintf(t, sizeof(t), s_perf_score_fmt[cur_lang], score);
        SDL_Color scol = score > 1300 ? color_green : (score > 800 ? color_yellow : color_grey);
        draw_text(r, font_sm, t, 885, row3_y + 106, scol, 0);
    }
    draw_rounded_box(r, 885, row3_y + 136, 350, 28, 6, color_card_border);
    draw_text(r, font_sm, s_export_perf_hint[cur_lang], 1060, row3_y + 140, color_orange, 1);

    SDL_RenderSetClipRect(r, NULL);

    // Scroll indicators
    if (perf_scroll_max > 0) {
        if (perf_scroll > 0)
            draw_text(r, font_sm, s_scroll_up[cur_lang], W - 190, 144, color_cyan, 0);
        if (perf_scroll < perf_scroll_max)
            draw_text(r, font_sm, s_scroll_down[cur_lang], W - 210, 624, color_cyan, 0);
    }
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

    int content_bottom = 920;
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

    // Left Joycon body
    int jcl_x = 340, jcl_y = 230 - sc;
    draw_rounded_box(r, jcl_x, jcl_y, 180, 360, 24, (SDL_Color){30, 120, 255, 255});
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
    draw_rounded_box(r, jcr_x, jcr_y, 180, 360, 24, (SDL_Color){255, 60, 80, 255});
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

    snprintf(t, sizeof(t), "L: X=%d Y=%d", stick_l.x, stick_l.y);
    draw_text(r, font_sm, t, 550, trig_y, color_cyan, 0);
    // align R meter to right card edge
    snprintf(t, sizeof(t), "R: X=%d Y=%d", stick_r.x, stick_r.y);
    draw_text(r, font_sm, t, 830, trig_y, color_cyan, 0);

    // Joy-Con battery levels (positioned to avoid overlap)
    HidPowerInfo jc_left = {0}, jc_right = {0};
    hidGetNpadPowerInfoSplit(HidNpadIdType_No1, &jc_left, &jc_right);
    draw_battery_icon(r, 75, 192 - sc, 30, 13, jc_left.battery_level * 25, false);
    draw_text(r, font_sm, get_joycon_battery_str(jc_left.battery_level), 112, 190 - sc, get_joycon_battery_color(jc_left.battery_level), 0);
    draw_battery_icon(r, 420, 192 - sc, 30, 13, jc_right.battery_level * 25, false);
    draw_text(r, font_sm, get_joycon_battery_str(jc_right.battery_level), 457, 190 - sc, get_joycon_battery_color(jc_right.battery_level), 0);

    // Connection type (moved right, compact)
    u64 style = padGetStyleSet(pad);
    const char *ctype = "Unknown";
    if (style & HidNpadStyleTag_NpadHandheld) ctype = "Handheld";
    else if (style & HidNpadStyleTag_NpadFullKey) ctype = "Pro Controller";
    else if (style & HidNpadStyleTag_NpadJoyDual) ctype = "Dual Joy-Con";
    else if (style & HidNpadStyleTag_NpadJoyLeft) ctype = "Left JC Only";
    else if (style & HidNpadStyleTag_NpadJoyRight) ctype = "Right JC Only";
    snprintf(t, sizeof(t), "%s: %s", s_controller_type[cur_lang], ctype);
    draw_text(r, font_sm, t, 820, 190 - sc, color_cyan, 0);

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

    SDL_RenderSetClipRect(r, NULL);

    if (ctrl_scroll_max > 0) {
        if (ctrl_scroll > 0)
            draw_text(r, font_sm, s_scroll_up[cur_lang], W - 190, 144, color_cyan, 0);
        if (ctrl_scroll < ctrl_scroll_max)
            draw_text(r, font_sm, s_scroll_down[cur_lang], W - 210, 624, color_cyan, 0);
    }
}
#undef draw_btn
#undef draw_btn_g
#undef draw_btn_y
#undef draw_btn_r

// Tools
static void draw_pg6(SDL_Renderer *r) {
    char t[256];

    // Content extends past visible area → clear scrolling needed
    int content_bottom = 1360;
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
        draw_text(r, font_sm, auto_br_enabled ? s_auto_on_off[cur_lang] : s_auto_off_on[cur_lang], 600, 260 - sc, auto_br_enabled ? color_green : color_grey, 0);
        draw_text(r, font_sm, s_br_adjust[cur_lang], 70, 275 - sc, color_grey, 0);
    } else {
        draw_text(r, font_sm, s_br_na[cur_lang], 70, 200 - sc, color_grey, 0);
    }

    draw_card(r, 40, 370 - sc, 580, 200, s_haptic_test[cur_lang]);
    draw_text(r, font_sm, s_test_haptic[cur_lang], 70, 430 - sc, color_grey, 0);
    draw_rounded_box(r, 70, 460 - sc, 220, 40, 6, color_card_border);
    draw_text(r, font_sm, s_left_rumble[cur_lang], 180, 470 - sc, color_cyan, 1);
    draw_rounded_box(r, 310, 460 - sc, 220, 40, 6, color_card_border);
    draw_text(r, font_sm, s_right_rumble[cur_lang], 420, 470 - sc, color_cyan, 1);

    draw_card(r, 660, 370 - sc, 580, 200, s_report_export[cur_lang]);
    draw_text(r, font_sm, s_export_desc[cur_lang], 690, 430 - sc, color_grey, 0);
    draw_rounded_box(r, 690, 460 - sc, 240, 40, 6, color_card_border);
    draw_text(r, font_sm, s_export_btn[cur_lang], 810, 470 - sc, color_orange, 1);
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

    draw_card(r, 40, 610 - sc, 1200, 140, s_runtime_env[cur_lang]);
    draw_key_value(r, s_app_status[cur_lang], s_running_ok[cur_lang], 70, 670 - sc, color_green);
    draw_key_value_wide(r, s_render[cur_lang], "SDL2 @ 60 FPS", 390, 670 - sc, 160, color_white);
    int dock = appletGetOperationMode();
    draw_key_value_wide(r, s_mode[cur_lang], dock ? s_docked[cur_lang] : s_handheld[cur_lang], 660, 670 - sc, 110, color_white);
    draw_key_value_wide(r, s_env_label[cur_lang], lbl_emulator ? s_emulator[cur_lang] : s_retail[cur_lang], 880, 670 - sc, 140, lbl_emulator ? color_yellow : color_green);

    draw_rounded_box(r, 70, 710 - sc, 220, 32, 6, color_card_border);
    draw_text(r, font_sm, s_dead_pixel[cur_lang], 180, 716 - sc, color_red, 1);

    {
        int fs_cx = 40, fs_cy = 790 - sc, fs_cw = 580, fs_ch = 220;
        draw_card(r, fs_cx, fs_cy, fs_cw, fs_ch, s_fan_ctrl[cur_lang]);
        SDL_Color fs_text_col = color_grey;
        SDL_Color fs_label_col = {100, 100, 110, 255};
        SDL_Color fs_track_col = {45, 45, 50, 255};

        draw_text(r, font_sm, s_fan_speed[cur_lang], 70, fs_cy + 45, fs_text_col, 0);
        snprintf(t, sizeof(t), "%d%%", fan_speed_pct);
        draw_text(r, font_sm, t, 520, fs_cy + 45, fs_label_col, 0);

        int fs_x = 70, fs_y = fs_cy + 75, fs_w = 510, fs_h = 12;
        draw_rounded_box(r, fs_x, fs_y, fs_w, fs_h, 6, fs_track_col);
        int fhx = fs_x + (int)(fan_speed_pct * fs_w / 100);
        draw_rounded_box(r, fs_x, fs_y, fhx - fs_x, fs_h, 6, fs_label_col);
        filledCircleRGBA(r, fhx, fs_y + fs_h/2, 14, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
        circleRGBA(r, fhx, fs_y + fs_h/2, 14, fs_label_col.r, fs_label_col.g, fs_label_col.b, 255);

        draw_text(r, font_xs, s_fan_adjust[cur_lang], 70, fs_cy + 105, fs_label_col, 0);
        draw_text(r, font_xs, s_fan_dev[cur_lang], 70, fs_cy + 135, color_yellow, 0);
        draw_text(r, font_xs, s_fan_req[cur_lang], 70, fs_cy + 160, fs_label_col, 0);
    }

    draw_card(r, 660, 790 - sc, 580, 180, s_app_mode[cur_lang]);
    {
        int mod_y = 850 - sc;
        int cur_mode = app_mode;
        for (int i = 0; i < 3; i++) {
            int bx = 690 + i * 180;
            SDL_Color mod_col = (i == cur_mode) ? color_cyan : color_grey;
            SDL_Color mod_bg = (i == cur_mode) ? color_dark_grey : color_card_border;
            draw_rounded_box(r, bx, mod_y, 170, 30, 6, mod_bg);
            draw_text(r, font_sm, s_modes[cur_lang][i], bx + 85, mod_y + 4, mod_col, 1);
        }
        draw_text(r, font_sm, s_mode_switch[cur_lang], 690, mod_y + 40, color_grey, 0);
        draw_text(r, font_sm, s_overlay_desc[cur_lang], 690, mod_y + 65, color_grey, 0);
        draw_text(r, font_sm, s_sysmodule_desc[cur_lang], 690, mod_y + 88, color_grey, 0);
    }

    draw_card(r, 40, 1010 - sc, 580, 200, s_console_info[cur_lang]);
    {
        SetSysSerialNumber sn;
        if (R_SUCCEEDED(setsysGetSerialNumber(&sn)) && serial_looks_retail(sn.number))
            draw_key_value(r, s_serial[cur_lang], sn.number, 70, 1070 - sc, color_cyan);
        else
            draw_key_value(r, s_serial[cur_lang], s_serial_emu[cur_lang], 70, 1070 - sc, color_grey);

        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            snprintf(t, sizeof(t), "%u.%u.%u", fw.major, fw.minor, fw.micro);
            draw_key_value(r, s_firmware[cur_lang], t, 70, 1100 - sc, color_yellow);
        }

        SetSysDeviceNickName nick;
        if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0])
            draw_key_value(r, s_nickname[cur_lang], nick.nickname, 70, 1130 - sc, color_white);
    }

    draw_card(r, 660, 1010 - sc, 580, 280, s_sd_info_speed[cur_lang]);
    {
        FsFileSystem sd;
        if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
            s64 free = 0, total = 0;
            if (R_SUCCEEDED(fsFsGetFreeSpace(&sd, "/", &free)) &&
                R_SUCCEEDED(fsFsGetTotalSpace(&sd, "/", &total)) && total > 0) {
                float used_pct = (float)(total - free) / total;
                snprintf(t, sizeof(t), "%.1f GB", total / 1.0e9);
                draw_key_value(r, s_total_capacity[cur_lang], t, 690, 1070 - sc, color_white);
                snprintf(t, sizeof(t), "%.1f GB", free / 1.0e9);
                draw_key_value(r, s_free_space[cur_lang], t, 690, 1100 - sc, color_green);
                draw_progress_bar(r, 690, 1130 - sc, 510, 16, used_pct, color_cyan, color_dark_grey);
                snprintf(t, sizeof(t), s_pct_used_fmt[cur_lang], used_pct * 100);
                draw_text(r, font_sm, t, 690, 1160 - sc, color_grey, 0);
            }
            draw_rounded_box(r, 690, 1190 - sc, 240, 36, 6, color_card_border);
            draw_text(r, font_sm, s_read_speed_btn[cur_lang], 810, 1198 - sc, color_cyan, 1);
            if (sd_speed_result > 0) {
                snprintf(t, sizeof(t), s_read_speed_fmt[cur_lang], sd_speed_result);
                draw_text(r, font_sm, t, 960, 1198 - sc, color_green, 0);
            } else if (sd_speed_result < 0) {
                if (sd_speed_result == -2)
                    draw_text(r, font_sm, s_testing[cur_lang], 960, 1198 - sc, color_yellow, 0);
                else
                    draw_text(r, font_sm, s_error[cur_lang], 960, 1198 - sc, color_red, 0);
            } else {
                draw_text(r, font_sm, s_not_tested[cur_lang], 960, 1198 - sc, color_grey, 0);
            }
            fsFsClose(&sd);
        } else {
            draw_text(r, font_sm, s_sd_info_na[cur_lang], 690, 1080 - sc, color_grey, 0);
        }
    }

    SDL_RenderSetClipRect(r, NULL);

    // Scroll indicators
    if (tools_scroll_max > 0) {
        if (tools_scroll > 0)
            draw_text(r, font_sm, s_scroll_up[cur_lang], W - 190, 144, color_cyan, 0);
        if (tools_scroll < tools_scroll_max)
            draw_text(r, font_sm, s_scroll_down[cur_lang], W - 210, 624, color_cyan, 0);
    }
}

// About
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
    content_h += 20 * 24; // changelog items
    content_h += 14;   // gap
    content_h += 28;   // roadmap header
    content_h += 5 * 24; // roadmap items
    content_h += 20;   // bottom pad

    int view_h = 540 - 48;
    about_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (about_scroll > about_scroll_max) about_scroll = about_scroll_max;
    if (about_scroll < 0) about_scroll = 0;

    // Clip to card body
    SDL_Rect clip = {40, 188, 1200, 510 - 48};
    SDL_RenderSetClipRect(r, &clip);

    int y = 200 - about_scroll;

    draw_text(r, font_lg, "Switch Info NX", 70, y, color_cyan, 0);
    y += 45;
    draw_text(r, font_sm, s_created_by[cur_lang], 70, y, color_green, 0);
    y += 33;

    draw_text(r, font_sm, s_desc_line1[cur_lang], 70, y, color_white, 0);
    y += 22;
    draw_text(r, font_sm, s_desc_line2[cur_lang], 70, y, color_white, 0);
    y += 30;

    const char *features[] = {
        "System  -  Firmware, serial, hardware model, region, device name",
        "Storage  -  SD partitions + Full file browser (rename/copy/delete)",
        "Network  -  Full IP config, Wi-Fi RSSI, MAC address",
        "Transfer  -  WiFi FTP server + MTP USB file transfer",
        "Performance  -  Live CPU/GPU/EMC clocks, thermal, profiles, RAM",
        "Controller  -  Joy-Con test + Gyroscope & Accelerometer",
        "Tools  -  Brightness, haptic, fan control, overlay/sysmodule mode",
    };
    for (int i = 0; i < 7; i++) {
        draw_text(r, font_sm, features[i], 90, y, color_grey, 0);
        y += 25;
    }

    y += 12;
    draw_text(r, font_sm, "Libraries: libnx | devkitA64 | SDL2 | SDL2_ttf | SDL2_gfx | FreeType", 70, y, color_cyan, 0);
    y += 22;
    draw_text(r, font_sm, "Thanks to SwitchBrew, devkitPro, and the homebrew community.", 70, y, color_grey, 0);
    y += 30;

    // Changelog
    draw_text(r, font_md, "v0.0.2 Changelog", 70, y, color_purple, 0);
    y += 30;
    const char *changelog[] = {
        "System page: scrollable with thermals, SoC/PCB temps, display info",
        "Perf page: enlarged RAM card with heap, pressure, FPS display",
        "Storage page: redesigned cleaner layout with 3 cards",
        "Network page: MAC address display added",
        "Controller page: Gyroscope + Accelerometer (6-axis sensors)",
        "Tools page: Custom fan speed control slider (0-100%)",
        "Tools page: Overlay / Sysmodule mode selector",
        "Custom Theme Editor: proper full-screen window with button",
        "Settings page: Language (7) + Theme (12 colors)",
        "Auto-refresh interval (Off/1s/3s/5s/10s) in Settings",
        "SD card read speed benchmark (Tools page)",
        "MTP support: Switch appears as MTP device on PC",
        "Full-screen file browser with delete/rename/copy/paste",
        "Touch support for Settings (tap language/theme/refresh)",
        "Theme colors applied to entire UI (header, tabs, footer)",
        "System uptime displayed in header bar",
        "Config persisted to sdmc:/switch/SwitchInfoNX/config.txt",
        "Translations for tab names and settings labels",
        "Fixed all hardcoded colors to follow theme",
        "Custom Theme Editor: 10 color slots, Save/Cancel",
    };
    for (int i = 0; i < 20; i++) {
        draw_text(r, font_sm, changelog[i], 90, y, color_grey, 0);
        y += 24;
    }
    y += 14;
    draw_text(r, font_md, "Planned for later", 70, y, color_purple, 0);
    y += 28;
    const char *roadmap[] = {
        "USB 3.0 speed for MTP transfer",
        "Game cart info reader",
        "Battery charge/discharge rate",
        "Network speed test (download/upload)",
        "CPU/GPU frequency scaling control",
    };
    for (int i = 0; i < 5; i++) {
        draw_text(r, font_sm, roadmap[i], 90, y, color_grey, 0);
        y += 24;
    }
    y += 20;

    SDL_RenderSetClipRect(r, NULL);

    // Scroll indicators
    if (about_scroll_max > 0) {
        if (about_scroll > 0)
            draw_text(r, font_sm, s_scroll_up[cur_lang], W - 190, 192, color_cyan, 0);
        if (about_scroll < about_scroll_max)
            draw_text(r, font_sm, s_scroll_down[cur_lang], W - 200, 600, color_cyan, 0);
    }
}

// ─── Settings page ─────────────────────────────────────────
static void draw_pg8(SDL_Renderer *r) {
    int view_top = 140, view_bottom = 650;
    int view_h = view_bottom - view_top;
    int content_bottom = 820;
    int content_h = content_bottom - view_top;
    settings_scroll_max = content_h > view_h ? content_h - view_h : 0;
    if (settings_scroll > settings_scroll_max) settings_scroll = settings_scroll_max;
    if (settings_scroll < 0) settings_scroll = 0;

    SDL_Rect clip = {0, view_top, W, view_h};
    SDL_RenderSetClipRect(r, &clip);
    int sc = settings_scroll;

    draw_card(r, 40, 140 - sc, 1200, 900, settings_title[cur_lang]);

    int y = 210 - sc;
    // Language
    draw_text(r, font_md, settings_lang_label[cur_lang], 70, y, color_cyan, 0);
    y += 40;
    for (int i = 0; i < LANG_MAX; i++) {
        int bx = 70 + i * 115;
        SDL_Color col = (settings_sel == 0 && i == cur_lang) ? color_green : color_grey;
        draw_rounded_box(r, bx, y - 4, 108, 36, 6, (settings_sel == 0 && i == cur_lang) ? color_dark_grey : color_card_border);
        draw_text(r, font_sm, lang_names[i], bx + 54, y + 4, (i == cur_lang) ? color_cyan : col, 1);
    }
    y += 56;

    // Theme (two rows of 6)
    draw_text(r, font_md, settings_theme_label[cur_lang], 70, y, color_cyan, 0);
    y += 40;
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 6; col++) {
            int i = row * 6 + col;
            if (i >= THEME_MAX) break;
            int bx = 70 + col * 105;
            SDL_Color tc = theme_white[i];
            SDL_Color bc = theme_bg[i];
            draw_rounded_box(r, bx, y - 4, 99, 30, 6, (settings_sel == 1 && i == cur_theme) ? color_dark_grey : color_card_border);
            draw_rounded_box(r, bx + 3, y - 1, 16, 24, 4, bc);
            draw_rounded_rect(r, bx + 3, y - 1, 16, 24, 4, tc);
            draw_text(r, font_sm, theme_names[i], bx + 24, y + 2, (i == cur_theme) ? color_cyan : color_grey, 0);
        }
        y += 38;
    }
    y += 16;
    if (cur_theme == THEME_CUSTOM) {
        draw_rounded_box(r, 70, y, 200, 32, 6, color_card_border);
        draw_text(r, font_sm, "[Touch] Custom Theme Editor", 170, y + 4, color_cyan, 1);
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
        int bx = 70 + i * 110;
        draw_rounded_box(r, bx, y - 4, 104, 36, 6, (settings_sel == 2 && i == cur_rsel) ? color_dark_grey : color_card_border);
        draw_text(r, font_sm, s_refresh_names[cur_lang][i], bx + 52, y + 4, (settings_sel == 2 && i == cur_rsel) ? color_cyan : color_grey, 1);
    }
    y += 60;

    // Temp alert threshold
    draw_text(r, font_md, settings_temp_alert_label[cur_lang], 70, y, color_cyan, 0);
    y += 36;
    char vtemp[64]; snprintf(vtemp, sizeof(vtemp), "%d.%d C", temp_alert_millic/1000, (temp_alert_millic%1000)/100);
    draw_rounded_box(r, 70, y, 200, 36, 6, (settings_sel == 3) ? color_dark_grey : color_card_border);
    draw_text(r, font_sm, vtemp, 170, y + 6, (settings_sel == 3) ? color_cyan : color_grey, 1);
    y += 52;

    // Memory alert threshold
    draw_text(r, font_md, settings_mem_alert_label[cur_lang], 70, y, color_cyan, 0);
    y += 36;
    char vmem[64]; snprintf(vmem, sizeof(vmem), "%d%%", (int)(mem_alert_frac * 100.0f));
    draw_rounded_box(r, 70, y, 200, 36, 6, (settings_sel == 4) ? color_dark_grey : color_card_border);
    draw_text(r, font_sm, vmem, 170, y + 6, (settings_sel == 4) ? color_cyan : color_grey, 1);
    y += 52;

    // Info
    draw_text(r, font_xs, settings_info[cur_lang], 70, y, color_grey, 0);
    draw_text(r, font_sm, s_settings_info2[cur_lang], 70, y + 40, color_grey, 0);

    SDL_RenderSetClipRect(r, NULL);

    // Scroll indicators
    if (settings_scroll_max > 0) {
        if (settings_scroll > 0)
            draw_text(r, font_sm, s_scroll_up[cur_lang], W - 190, 144, color_cyan, 0);
        if (settings_scroll < settings_scroll_max)
            draw_text(r, font_sm, s_scroll_down[cur_lang], W - 210, 624, color_cyan, 0);
    }

    // ── Custom Theme Editor (full-screen window) ──
    if (custom_theme_editing) {
        char cte_t[128];
        // Full background overlay
        SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
        SDL_Rect full = {0, 0, W, H};
        SDL_RenderFillRect(r, &full);

        // Editor window card
        draw_card_shadow(r, 80, 130, 1120, 540);
        draw_rounded_box(r, 80, 130, 1120, 540, 12, color_bg2);
        draw_rounded_rect(r, 80, 130, 1120, 540, 12, color_cyan);

        // Header with close button
        draw_text(r, font_lg, s_cte_title[cur_lang], 640, 148, color_cyan, 1);
        thickLineRGBA(r, 100, 185, 1180, 185, 2, color_card_border.r, color_card_border.g, color_card_border.b, 255);

        // Color slot grid (5 rows x 2 cols)
        const char *cte_names[10] = {"Background","Card bg","Border","Accent Cyan","Accent Green","Text","Grey","Dark Grey","Header bg2","Tab bg3"};
        SDL_Color *cte_colors[] = {&custom_bg,&custom_card,&custom_border,&custom_cyan,&custom_green,&custom_white,&custom_grey,&custom_dark_grey,&custom_bg2,&custom_bg3};
        int cte_count = sizeof(cte_names)/sizeof(cte_names[0]);

        int cx = 110, cy = 200;
        for (int i = 0; i < cte_count; i++) {
            int row = i / 2;
            int col = i % 2;
            int px = cx + col * 420;
            int py = cy + row * 46;
            int sel = (cte_sel == i);
            draw_rounded_box(r, px, py, 400, 36, 6, sel ? (SDL_Color){50,80,160,255} : color_card_border);
            draw_rounded_box(r, px + 4, py + 4, 28, 28, 4, *cte_colors[i]);
            draw_rounded_rect(r, px + 4, py + 4, 28, 28, 4, sel ? color_white : color_grey);
            draw_text(r, font_sm, cte_names[i], px + 40, py + 6, sel ? color_white : color_grey, 0);
            snprintf(cte_t, sizeof(cte_t), "R:%03d G:%03d B:%03d", cte_colors[i]->r, cte_colors[i]->g, cte_colors[i]->b);
            draw_text(r, font_xs, cte_t, px + 200, py + 10, sel ? color_cyan : color_grey, 0);
        }

        // Channel edit area
        SDL_Color *sc = cte_colors[cte_sel];
        int edit_y = cy + cte_count/2 * 46 + 10;
        draw_text(r, font_sm, s_cte_edit_hint[cur_lang], 640, edit_y, color_cyan, 1);

        const char *chan_names[] = {"R", "G", "B"};
        u8 *chan_vals[] = {&sc->r, &sc->g, &sc->b};
        for (int ch = 0; ch < 3; ch++) {
            int by = edit_y + 28 + ch * 36;
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
        int btn_y = edit_y + 28 + 3 * 36 + 16;
        // Save button
        draw_rounded_box(r, 340, btn_y, 200, 44, 8, color_green);
        draw_rounded_rect(r, 340, btn_y, 200, 44, 8, (SDL_Color){0,180,100,255});
        draw_text(r, font_sm, s_cte_save[cur_lang], 440, btn_y + 10, color_bg, 1);
        // Cancel button
        draw_rounded_box(r, 600, btn_y, 200, 44, 8, (SDL_Color){180,30,30,255});
        draw_rounded_rect(r, 600, btn_y, 200, 44, 8, color_red);
        draw_text(r, font_sm, s_cte_cancel[cur_lang], 700, btn_y + 10, color_white, 1);

        // Touch hints
        draw_text(r, font_xs, s_cte_touch_hint[cur_lang], 640, btn_y + 60, color_grey, 1);
    }
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
    draw_text(r, font_xs, s_size_col[cur_lang], list_x + 820, 202, color_grey, 0);
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
            if (fb_sizes[idx] > 1073741824)
                snprintf(t, sizeof(t), "%.2f GB", fb_sizes[idx] / 1.0e9);
            else if (fb_sizes[idx] > 1048576)
                snprintf(t, sizeof(t), "%.1f MB", fb_sizes[idx] / 1.0e6);
            else if (fb_sizes[idx] > 1024)
                snprintf(t, sizeof(t), "%.0f KB", fb_sizes[idx] / 1.0e3);
            else
                snprintf(t, sizeof(t), "%u B", fb_sizes[idx]);
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
    } else if (fb_mode == FB_MODE_CONFIRM_DELETE) {
        draw_text(r, font_sm, s_fb_delete_q[cur_lang], 20, 678, color_red, 0);
        draw_text(r, font_sm, s_fb_confirm_del[cur_lang], 500, 678, color_grey, 0);
    } else {
        draw_text(r, font_sm, s_fb_browse_hint[cur_lang], 20, 672, color_grey, 0);
        if (fb_clipboard_path[0])
            draw_text(r, font_sm, s_fb_paste[cur_lang], 660, 672, color_cyan, 0);
        draw_text(r, font_sm, s_fb_other_hint[cur_lang], 20, 694, color_grey, 0);
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
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Renamed to: %s", new_name);
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

static void fb_paste(void) {
    if (!fb_clipboard_path[0]) return;
    const char *name = strrchr(fb_clipboard_path, '/');
    if (!name) return;
    name++;
    char dst[2048];
    snprintf(dst, sizeof(dst), "%s/%s", fb_path, name);

    // If same path, append _copy
    struct stat st;
    if (stat(dst, &st) == 0) {
        char base[256], ext[64];
        const char *dot = strrchr(name, '.');
        if (dot && !fb_is_dir[fb_selected]) {
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
        FILE *src_f = fopen(fb_clipboard_path, "rb");
        if (src_f) {
            FILE *dst_f = fopen(dst, "wb");
            if (dst_f) {
                char buf[8192];
                int n;
                while ((n = fread(buf, 1, sizeof(buf), src_f)) > 0)
                    fwrite(buf, 1, n, dst_f);
                fclose(dst_f);
                ok = 1;
            }
            fclose(src_f);
        }
    }

    if (ok) {
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Pasted: %s", name);
        fb_open(fb_path);
    } else {
        snprintf(fb_status_msg, sizeof(fb_status_msg), "Error pasting: %s", strerror(errno));
    }
    fb_status_tick = armGetSystemTick();
}

// ─── Main loop ─────────────────────────────────────────────

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    // Initialize Switch services
    setsysInitialize();
    setInitialize();
    psmInitialize();
    nifmInitialize(NifmServiceType_User);
    clkrstInitialize();
    plInitialize(PlServiceType_User);
    socketInitializeDefault();

    // Pad state config
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    start_tick = armGetSystemTick();
    ctrl_brightness = -1.0f;
    lbl_detect_environment();

    // Load settings config
    load_config();

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

    // Initialize Joycon Haptic Rumble handles
    Result vib_rc = hidInitializeVibrationDevices(vibe_handles, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    if (R_SUCCEEDED(vib_rc)) {
        vibe_init_ok = true;
    }

    // Initialize SDL2
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        goto exit_app;
    }

    if (TTF_Init() < 0) {
        SDL_Quit();
        goto exit_app;
    }

    SDL_Window *window = SDL_CreateWindow("Switch Info NX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, 0);
    if (!window) {
        TTF_Quit();
        SDL_Quit();
        goto exit_app;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        goto exit_app;
    }

    // Load standard Switch system fonts from PL service
    PlFontData shared_font;
    if (R_SUCCEEDED(plGetSharedFontByType(&shared_font, PlSharedFontType_Standard))) {
        SDL_RWops *rw_xs = SDL_RWFromMem(shared_font.address, shared_font.size);
        SDL_RWops *rw_sm = SDL_RWFromMem(shared_font.address, shared_font.size);
        SDL_RWops *rw_md = SDL_RWFromMem(shared_font.address, shared_font.size);
        SDL_RWops *rw_lg = SDL_RWFromMem(shared_font.address, shared_font.size);
        font_xs = TTF_OpenFontRW(rw_xs, 1, 14);
        font_sm = TTF_OpenFontRW(rw_sm, 1, 18);
        font_md = TTF_OpenFontRW(rw_md, 1, 24);
        font_lg = TTF_OpenFontRW(rw_lg, 1, 36);
        if (!font_xs || !font_sm || !font_md || !font_lg) {
            add_alert("Error: failed to load fonts");
            goto exit_app;
        }
    } else {
        // No font available
        goto exit_app;
    }

    int cur = 0;
    bool quit = false;
    
    void (*pages[PGS])(SDL_Renderer *) = { 
        draw_pg0, draw_pg1, draw_pg2, draw_pg3, draw_pg4, NULL, draw_pg6, draw_pg7, draw_pg8
    };

    // Auto-refresh timer
    u64 last_refresh = armGetSystemTick();

    while (appletMainLoop() && !quit) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

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
                    hidStartSixAxisSensor(sixaxis_handles[0]);
                    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&sixaxis_handles[1], 1, hid_id, style)))
                        hidStartSixAxisSensor(sixaxis_handles[1]);
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
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) quit = true;

            int mx = -1, my = -1;
            int ev_down = 0, ev_move = 0;

            if (event.type == SDL_MOUSEBUTTONDOWN) {
                mx = event.button.x; my = event.button.y; ev_down = 1;
            } else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK)) {
                mx = event.motion.x; my = event.motion.y; ev_move = 1;
            } else if (event.type == SDL_FINGERDOWN) {
                mx = (int)(event.tfinger.x * W); my = (int)(event.tfinger.y * H); ev_down = 1;
            } else if (event.type == SDL_FINGERMOTION) {
                mx = (int)(event.tfinger.x * W); my = (int)(event.tfinger.y * H); ev_move = 1;
            }

            if (!ev_down && !ev_move) continue;

            // Dismiss popup on any tap
            if (popup_active && ev_down) {
                popup_active = false;
                continue;
            }

            // Tab navigation (blocked when custom theme editor is open)
            if (ev_down && my >= 51 && my <= 111 && !(cur == 8 && custom_theme_editing)) {
                int tab_w = W / PGS;
                int t = mx / tab_w;
                if (t >= 0 && t < PGS) cur = t;
            }

            // Transfer page: badge + mode toggle
            if (cur == 3 && ev_down) {
                if (mx >= 70 && mx <= 190 && my >= 210 && my <= 242) {
                    if (ftp_on) ftp_stop(); else ftp_start();
                }
                if (mx >= 690 && mx <= 810 && my >= 210 && my <= 242) {
                    if (mtp_on) mtp_stop_local(); else mtp_start_local();
                }
                if (my >= 390 && my <= 414) {
                    if ((mx >= 70 && mx <= 116) || (mx >= 120 && mx <= 172)) {
                        ftp_mode = !ftp_mode;
                        if (ftp_on) { ftp_stop(); ftp_start(); }
                    }
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
            if (cur == 6 && ev_down) {
                int bt_y = 460 - tools_scroll;
                int exp_y = 460 - tools_scroll;
                int dp_y = 710 - tools_scroll;
                if (mx >= 70 && mx <= 290 && my >= bt_y && my <= bt_y + 40 && vibe_init_ok) {
                    HidVibrationValue v = {160.0f, 0.6f, 320.0f, 0.6f};
                    hidSendVibrationValue(vibe_handles[0], &v);
                    svcSleepThread(150000000);
                    v.amp_low = 0.0f; v.amp_high = 0.0f;
                    hidSendVibrationValue(vibe_handles[0], &v);
                }
                if (mx >= 310 && mx <= 530 && my >= bt_y && my <= bt_y + 40 && vibe_init_ok) {
                    HidVibrationValue v = {160.0f, 0.6f, 320.0f, 0.6f};
                    hidSendVibrationValue(vibe_handles[1], &v);
                    svcSleepThread(150000000);
                    v.amp_low = 0.0f; v.amp_high = 0.0f;
                    hidSendVibrationValue(vibe_handles[1], &v);
                }
                if (mx >= 690 && mx <= 930 && my >= exp_y && my <= exp_y + 40)
                    export_system_info();
                if (mx >= 70 && mx <= 290 && my >= dp_y && my <= dp_y + 32)
                    screen_test_active = 1;

                // App mode selector touch
                int mod_y = 850 - tools_scroll;
                for (int i = 0; i < 3; i++) {
                    int bx = 690 + i * 180;
                    if (mx >= bx && mx <= bx + 170 && my >= mod_y && my <= mod_y + 30) {
                        app_mode = i;
                    }
                }

                // Fan speed card tap -> popup "En developpement"
                int fs_card_y = 790 - tools_scroll;
                if (mx >= 40 && mx <= 620 && my >= fs_card_y && my <= fs_card_y + 200) {
                    popup_active = true;
                    snprintf(popup_text, sizeof(popup_text), s_dev_fan[cur_lang]);
                    popup_start_tick = armGetSystemTick();
                }

                // SD Speed Test button: y=1190-sc
                int sd_btn_y = 1190 - tools_scroll;
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

            // Tools page: fan speed card tap -> popup (disabled feature)
            if (cur == 6 && (ev_down || ev_move)) {
                int fs_sy = 790 - tools_scroll;
                if (mx >= 40 && mx <= 620 && my >= fs_sy && my <= fs_sy + 200) {
                    popup_active = true;
                    snprintf(popup_text, sizeof(popup_text), s_dev_fan[cur_lang]);
                    popup_start_tick = armGetSystemTick();
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
            }
            // Storage page: tap file browser entry (full-screen)
            if (cur == 1 && fb_active && ev_down && my >= 222 && my <= 640 && mx >= 40 && mx <= 1240) {
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
            int scroll_max = (cur == 0) ? system_scroll_max : (cur == 2) ? net_scroll_max : (cur == 4) ? perf_scroll_max : (cur == 5) ? ctrl_scroll_max : (cur == 7) ? about_scroll_max : (cur == 6) ? tools_scroll_max : (cur == 1) ? storage_scroll_max : (cur == 8) ? settings_scroll_max : 0;
            int *scroll_var = (cur == 0) ? &system_scroll : (cur == 2) ? &net_scroll : (cur == 4) ? &perf_scroll : (cur == 5) ? &ctrl_scroll : (cur == 7) ? &about_scroll : (cur == 6) ? &tools_scroll : (cur == 1) ? &storage_scroll : (cur == 8) ? &settings_scroll : NULL;
            if (scroll_var && scroll_max > 0) {
                if (ev_down)
                    touch_start_y = my;
                else if (ev_move) {
                    int dy = touch_start_y - my;
                    if (dy > 4 || dy < -4) {
                        *scroll_var += dy;
                        if (*scroll_var < 0) *scroll_var = 0;
                        if (*scroll_var > scroll_max) *scroll_var = scroll_max;
                        touch_start_y = my;
                    }
                }
            }

            // Settings page: touch for language/theme/refresh selection
            if (cur == 8 && ev_down) {
                // Language buttons: y 246-282
                if (my >= 246 && my <= 282) {
                    for (int i = 0; i < LANG_MAX; i++) {
                        int bx = 70 + i * 115;
                        if (mx >= bx && mx <= bx + 108) {
                            cur_lang = i;
                            save_config();
                            break;
                        }
                    }
                }
                // Theme buttons: two rows
                if (my >= 342 && my <= 372) {
                    for (int i = 0; i < 6 && i < THEME_MAX; i++) {
                        int bx = 70 + i * 105;
                        if (mx >= bx && mx <= bx + 99) {
                            cur_theme = i;
                            apply_theme(); save_config(); break;
                        }
                    }
                }
                if (my >= 380 && my <= 410) {
                    for (int i = 6; i < 12 && i < THEME_MAX; i++) {
                        int bx = 70 + (i - 6) * 105;
                        if (mx >= bx && mx <= bx + 99) {
                            cur_theme = i;
                            apply_theme(); save_config(); break;
                        }
                    }
                }
                // Refresh buttons
                int ref_y_start = (cur_theme == THEME_MAX - 1) ? 514 : 466;
                if (my >= ref_y_start && my <= ref_y_start + 40) {
                    static const int rv[] = {0,1,3,5,10};
                    for (int i = 0; i < 5; i++) {
                        int bx = 70 + i * 110;
                        if (mx >= bx && mx <= bx + 104) {
                            auto_refresh_interval = rv[i];
                            save_config();
                            break;
                        }
                    }
                }
                // Custom theme button: y 438-470 (if THEME_CUSTOM selected)
                if (cur_theme == THEME_MAX - 1 && my >= 438 && my <= 470) {
                    custom_theme_editing = 1;
                    cte_backup[0]=custom_bg; cte_backup[1]=custom_card; cte_backup[2]=custom_border;
                    cte_backup[3]=custom_cyan; cte_backup[4]=custom_green; cte_backup[5]=custom_white;
                    cte_backup[6]=custom_grey; cte_backup[7]=custom_dark_grey; cte_backup[8]=custom_bg2;
                    cte_backup[9]=custom_bg3;
                }
                // Custom theme editor touch (new layout coordinates)
                if (custom_theme_editing) {
                    int cte_cy = 200;
                    for (int i = 0; i < 10; i++) {
                        int row = i / 2;
                        int col = i % 2;
                        int px = 110 + col * 420;
                        int py = cte_cy + row * 46;
                        if (mx >= px && mx <= px + 400 && my >= py && my <= py + 36) {
                            cte_sel = i;
                        }
                    }
                    // Tap channel R/G/B sliders
                    int cte_edit_y = cte_cy + 5 * 46 + 10;
                    SDL_Color *cte_colors[] = {&custom_bg,&custom_card,&custom_border,&custom_cyan,&custom_green,&custom_white,&custom_grey,&custom_dark_grey,&custom_bg2,&custom_bg3};
                    for (int ch = 0; ch < 3; ch++) {
                        int by = cte_edit_y + 28 + ch * 36;
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
                    int cte_btn_y = cte_edit_y + 28 + 3 * 36 + 16;
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
                if (screen_test_active >= SCREEN_TEST_PHASES) screen_test_active = 0;
            }
            if (down & HidNpadButton_B) screen_test_active = 0;
        }

        // Transfer page: A toggles WiFi FTP, X toggles USB, Y toggles FTP/FTPD mode
        if (cur == 3) {
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
            // Fan speed control (disabled - shows popup)
            if (down & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                popup_active = true;
                snprintf(popup_text, sizeof(popup_text), s_dev_fan[cur_lang]);
                popup_start_tick = armGetSystemTick();
            }
            // L+R together -> popup
            if ((down & HidNpadButton_L) && (down & HidNpadButton_R)) {
                popup_active = true;
                snprintf(popup_text, sizeof(popup_text), s_dev_fan[cur_lang]);
                popup_start_tick = armGetSystemTick();
            }
            // App mode switching (only if L or R alone)
            if ((down & HidNpadButton_L) && !(down & HidNpadButton_R)) {
                app_mode = (app_mode - 1 + 3) % 3;
            }
            if ((down & HidNpadButton_R) && !(down & HidNpadButton_L)) {
                app_mode = (app_mode + 1) % 3;
            }
        }

        // Settings page: navigate options with Up/Down, change with Left/Right
        if (cur == 8 && !custom_theme_editing) {
            if (down & HidNpadButton_Down) { settings_sel = (settings_sel + 1) % 5; }
            if (down & HidNpadButton_Up) { settings_sel = (settings_sel - 1 + 5) % 5; }
            if (down & HidNpadButton_Left) {
                if (settings_sel == 0) cur_lang = (cur_lang - 1 + LANG_MAX) % LANG_MAX;
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
                if (settings_sel == 0) cur_lang = (cur_lang + 1) % LANG_MAX;
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

        // Tab navigation (after per-page handlers so pages can override L/R)
        if (cur != 8) {
            if (down & HidNpadButton_L) cur = (cur - 1 + PGS) % PGS;
            if (down & HidNpadButton_R) cur = (cur + 1) % PGS;
        }

        // Haptic rumble in Tools tab (X and Y)
        if (cur == 6 && vibe_init_ok) {
            if (down & HidNpadButton_X) {
                HidVibrationValue v = {160.0f, 0.6f, 320.0f, 0.6f};
                hidSendVibrationValue(vibe_handles[0], &v);
                svcSleepThread(150000000);
                v.amp_low = 0.0f; v.amp_high = 0.0f;
                hidSendVibrationValue(vibe_handles[0], &v);
            }
            if (down & HidNpadButton_Y) {
                HidVibrationValue v = {160.0f, 0.6f, 320.0f, 0.6f};
                hidSendVibrationValue(vibe_handles[1], &v);
                svcSleepThread(150000000);
                v.amp_low = 0.0f; v.amp_high = 0.0f;
                hidSendVibrationValue(vibe_handles[1], &v);
            }
        }

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
        if (cur == 5 && ctrl_scroll_max > 0) {
            if (down & (HidNpadButton_Up | HidNpadButton_Left)) {
                ctrl_scroll -= 24;
                if (ctrl_scroll < 0) ctrl_scroll = 0;
            }
            if (down & (HidNpadButton_Down | HidNpadButton_Right)) {
                ctrl_scroll += 24;
                if (ctrl_scroll > ctrl_scroll_max) ctrl_scroll = ctrl_scroll_max;
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
        if (cur != 4) perf_scroll = 0;
        if (cur != 5) ctrl_scroll = 0;
        if (cur != 7) about_scroll = 0;
        if (cur != 6) tools_scroll = 0;
        if (cur != 1) storage_scroll = 0;
        if (cur != 8) settings_scroll = 0;

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

            // Popup overlay (En developpement)
            if (popup_active) {
                u64 elapsed_ticks = armGetSystemTick() - popup_start_tick;
                u64 elapsed_sec = elapsed_ticks / armGetSystemTickFreq();
                if (elapsed_sec < 3) {
                    // Semi-transparent overlay
                    SDL_Rect overlay = {0, 0, W, H};
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
                    SDL_RenderFillRect(renderer, &overlay);
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

                    // Popup box (enlarged)
                    int pw = 700, ph = 160, px = (W - pw) / 2, py = (H - ph) / 2;
                    draw_rounded_box(renderer, px, py, pw, ph, 12, color_card);
                    draw_rounded_rect(renderer, px, py, pw, ph, 12, color_cyan);
                    draw_text(renderer, font_md, popup_text, px + pw/2, py + 40, color_white, 1);
                    draw_text(renderer, font_sm, s_popup_dismiss[cur_lang], px + pw/2, py + 110, color_grey, 1);
                } else {
                    popup_active = false;
                }
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

exit_app:
    if (ftp_on) ftp_stop();
    if (mtp_on) {
        mtp_server_stop();
        mtp_on = 0;
    }
    if (sixaxis_init_ok) {
        hidStopSixAxisSensor(sixaxis_handles[0]);
        hidStopSixAxisSensor(sixaxis_handles[1]);
    }

    if (font_xs) TTF_CloseFont(font_xs);
    if (font_sm) TTF_CloseFont(font_sm);
    if (font_md) TTF_CloseFont(font_md);
    if (font_lg) TTF_CloseFont(font_lg);
    
    TTF_Quit();
    SDL_Quit();

    socketExit();
    plExit();
    clkrstExit();
    nifmExit();
    psmExit();
    setExit();
    setsysExit();
    
    return 0;
}
