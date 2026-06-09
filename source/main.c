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

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#define W 1280
#define H 720
#define PGS 8

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
static SDL_Color color_purple = {180, 100, 255, 255};
static SDL_Color color_orange = {255, 150, 50, 255};

static const char *pgname[PGS] = {
    "System", "Storage", "Network", "FTP", "Perf", "Controller", "Tools", "About"
};

// Tick at app start for uptime
static u64 start_tick = 0;
// Refresh counter
static u32 refresh_count = 0;
// Brightness
static float ctrl_brightness = -1.0f;
static bool lbl_ready = false;
static bool lbl_emulator = true;

static TTF_Font *font_sm = NULL;
static TTF_Font *font_md = NULL;
static TTF_Font *font_lg = NULL;

static HidVibrationDeviceHandle vibe_handles[2];
static bool vibe_init_ok = false;

// Export status message
static char export_msg[128] = {0};
static u64 export_msg_tick = 0;

static bool serial_looks_retail(const char *sn) {
    static const char *prefixes[] = { "XAW", "XAJ", "XKW", "XKJ", "HDH", "HEG", "HAE", NULL };
    for (int i = 0; prefixes[i]; i++) {
        if (strncmp(sn, prefixes[i], 3) == 0) return true;
    }
    return false;
}

static const char *detect_hardware_type(const char *sn) {
    if (strncmp(sn, "XAW", 3) == 0 || strncmp(sn, "XAJ", 3) == 0)
        return "Switch V1 (Erista / HAC-001)";
    if (strncmp(sn, "XKW", 3) == 0 || strncmp(sn, "XKJ", 3) == 0)
        return "Switch V2 (Mariko / HAC-001-01)";
    if (strncmp(sn, "HDH", 3) == 0)
        return "Switch Lite (HDH-001)";
    if (strncmp(sn, "HEG", 3) == 0)
        return "Switch OLED (HEG-001)";
    if (strncmp(sn, "HAE", 3) == 0)
        return "Dev Unit (SDEV)";
    return "Unknown Model";
}

static bool lbl_safe_on_device(void) {
    SetSysSerialNumber sn = {0};
    if (!R_SUCCEEDED(setsysGetSerialNumber(&sn)) || !serial_looks_retail(sn.number))
        return false;

    u32 batt = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    psmGetBatteryChargePercentage(&batt);
    psmGetChargerType(&ch);

    if (batt == 100 && ch == PsmChargerType_EnoughPower)
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

static void draw_card(SDL_Renderer *r, int x, int y, int w, int h, const char *title) {
    draw_rounded_box(r, x, y, w, h, 8, color_card);
    draw_rounded_rect(r, x, y, w, h, 8, color_card_border);
    if (title && title[0]) {
        draw_text(r, font_md, title, x + 20, y + 15, color_cyan, 0);
        thickLineRGBA(r, x + 20, y + 48, x + w - 20, y + 48, 2, color_card_border.r, color_card_border.g, color_card_border.b, color_card_border.a);
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
        case 0: return "Empty";
        case 1: return "Critical (25%)";
        case 2: return "Low (50%)";
        case 3: return "Medium (75%)";
        case 4: return "Full (100%)";
        default: return "Unknown";
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
        else strcpy(t, "/");
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
    fsend(fd, "220 SwitchInfoNX FTP v0.0.1 - sdmc:/ root access ready\r\n");
    ftp_data = -1;
    ftp_rest_offset = 0;
    ftp_addlog("Client connected");

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
                strcpy(ftp_cwd, new_dir);
                if (!ftp_cwd[0]) strcpy(ftp_cwd,"/");
                char log[80]; snprintf(log,sizeof(log),"CWD %s", ftp_cwd);
                ftp_addlog(log);
                fsend(fd,"250 Directory changed\r\n");
            } else if (strcmp(new_dir, "/") == 0) {
                strcpy(ftp_cwd, "/");
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
            if (arg[0]) fclean(dp,ftp_cwd,arg); else strcpy(dp,ftp_cwd);
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
                char f[512];
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
    a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(FTP_PORT);
    if (bind(ftp_srv,(struct sockaddr*)&a,sizeof(a))<0||listen(ftp_srv,FTP_MAXCL)<0)
        { close(ftp_srv); ftp_on=0; return; }
    ftp_addlog("FTP server listening on port 5000");
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
    strcpy(ftp_cwd,"/");
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

// ─── System info export ───────────────────────────────────

static void export_system_info(void) {
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/SwitchInfoNX", 0755);
    FILE *f = fopen("sdmc:/switch/SwitchInfoNX/system_report.txt", "w");
    if (!f) {
        snprintf(export_msg, sizeof(export_msg), "Error: Cannot write to SD card");
        export_msg_tick = armGetSystemTick();
        return;
    }

    fprintf(f, "=== SwitchInfoNX System Report ===\n");
    fprintf(f, "Generated by SwitchInfoNX v0.0.1 by dodosi\n\n");

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

    snprintf(export_msg, sizeof(export_msg), "Report saved to sdmc:/switch/SwitchInfoNX/system_report.txt");
    export_msg_tick = armGetSystemTick();
}

// ─── Header bar ───────────────────────────────────────────

static void draw_header(SDL_Renderer *r) {
    // Top header background with subtle gradient
    SDL_Rect bg = {0, 0, W, 50};
    SDL_SetRenderDrawColor(r, 22, 22, 26, 255);
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 44, 44, 52, 255);
    SDL_RenderDrawLine(r, 0, 50, W, 50);

    // Title
    draw_text(r, font_md, "Switch Info NX", 20, 12, color_white, 0);
    draw_text(r, font_sm, "v0.0.1 by dodosi", 195, 17, color_grey, 0);

    // Time
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) {
        char ts[16];
        snprintf(ts, sizeof(ts), "%02d:%02d", lt->tm_hour, lt->tm_min);
        draw_text(r, font_md, ts, W - 80, 12, color_white, 0);
    }

    // IP Configuration
    u32 ip = 0;
    nifmGetCurrentIpAddress(&ip);
    char ips[32] = {0};
    if (ip) {
        snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);
        draw_text(r, font_sm, ips, W - 320, 16, color_cyan, 0);
    } else {
        draw_text(r, font_sm, "No Network", W - 320, 16, color_red, 0);
    }

    // Battery
    u32 batt = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    psmGetBatteryChargePercentage(&batt);
    psmGetChargerType(&ch);
    draw_battery_icon(r, W - 160, 15, 45, 20, batt, (ch != PsmChargerType_Unconnected));
}

// ─── Tab bar ──────────────────────────────────────────────

static void draw_tabs(SDL_Renderer *r, int cur) {
    int tab_w = W / PGS;
    SDL_Rect bg = {0, 51, W, 60};
    SDL_SetRenderDrawColor(r, 22, 22, 26, 255);
    SDL_RenderFillRect(r, &bg);

    for (int i = 0; i < PGS; i++) {
        SDL_Rect btn = {i * tab_w, 51, tab_w, 60};
        if (i == cur) {
            // Selected tab background
            SDL_SetRenderDrawColor(r, 34, 34, 42, 255);
            SDL_RenderFillRect(r, &btn);
            
            // Accent line at bottom
            SDL_Rect line = {i * tab_w + 10, 107, tab_w - 20, 4};
            SDL_SetRenderDrawColor(r, color_cyan.r, color_cyan.g, color_cyan.b, 255);
            SDL_RenderFillRect(r, &line);
            
            draw_text(r, font_md, pgname[i], i * tab_w + tab_w / 2, 68, color_white, 1);
        } else {
            draw_text(r, font_md, pgname[i], i * tab_w + tab_w / 2, 68, color_grey, 1);
        }
        
        // Tab separator
        if (i > 0) {
            SDL_SetRenderDrawColor(r, 44, 44, 52, 255);
            SDL_RenderDrawLine(r, i * tab_w, 56, i * tab_w, 106);
        }
    }

    SDL_SetRenderDrawColor(r, 44, 44, 52, 255);
    SDL_RenderDrawLine(r, 0, 111, W, 111);
}

// ─── Footer ───────────────────────────────────────────────

static void draw_footer(SDL_Renderer *r) {
    SDL_Rect bg = {0, 660, W, 60};
    SDL_SetRenderDrawColor(r, 22, 22, 26, 255);
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 44, 44, 52, 255);
    SDL_RenderDrawLine(r, 0, 660, W, 660);

    // Shortcuts
    draw_text(r, font_sm, "[L/R] Navigate    [A] Action    [Y] Refresh    [DPad] Brightness", 20, 680, color_grey, 0);
    draw_text(r, font_sm, "[+] Exit", W - 100, 680, color_red, 0);
}

// ─── Pages ────────────────────────────────────────────────

// System Info
static void draw_pg0(SDL_Renderer *r) {
    char t[256];

    // Card 1: Hardware
    draw_card(r, 40, 140, 580, 280, "Firmware & Hardware");
    SetSysFirmwareVersion fw = {0};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        snprintf(t, sizeof(t), "%d.%d.%d", fw.major, fw.minor, fw.micro);
        draw_key_value(r, "Firmware", t, 70, 210, color_white);
    }
    SetSysSerialNumber sn = {0};
    if (R_SUCCEEDED(setsysGetSerialNumber(&sn))) {
        draw_key_value(r, "Serial No.", sn.number, 70, 240, color_white);
        draw_key_value_wide(r, "Hardware", detect_hardware_type(sn.number), 70, 270, 140, color_purple);
    }
    int docked = appletGetOperationMode();
    draw_key_value(r, "Mode", docked ? "Docked (TV Output)" : "Handheld (Portable)", 70, 300, docked ? color_green : color_cyan);
    
    // Device nickname
    SetSysDeviceNickName nick = {0};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0]) {
        draw_key_value(r, "Device Name", nick.nickname, 70, 330, color_yellow);
    }

    // Region
    SetRegion region;
    if (R_SUCCEEDED(setGetRegionCode(&region))) {
        const char *rnames[] = {"Japan","Americas","Europe","Australia/NZ","HK/TW/KR","China"};
        if ((int)region >= 0 && (int)region <= 5)
            draw_key_value(r, "Region", rnames[(int)region], 70, 360, color_white);
    }

    // Language
    u64 lang = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&lang))) {
        SetLanguage langCode;
        if (R_SUCCEEDED(setMakeLanguage(lang, &langCode))) {
            const char *langNames[] = {
                "Japanese","English US","French","German","Italian",
                "Spanish","Chinese","Korean","Dutch","Portuguese",
                "Russian","Chinese TW","English UK","French CA",
                "Spanish LA","Chinese Hans","Chinese Hant","Brazilian PT"
            };
            if ((int)langCode >= 0 && (int)langCode < 18)
                draw_key_value(r, "Language", langNames[(int)langCode], 70, 390, color_white);
        }
    }

    // Card 2: Battery & Power
    draw_card(r, 660, 140, 580, 280, "Battery & Power");
    u32 batt = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batt))) {
        psmGetChargerType(&ch);
        snprintf(t, sizeof(t), "%u%%", batt);
        draw_key_value(r, "Battery Level", t, 690, 210, get_usage_color(100 - batt));
        draw_key_value(r, "Charging", ch != PsmChargerType_Unconnected ? "Charging" : "Discharging", 690, 240, ch != PsmChargerType_Unconnected ? color_green : color_yellow);
        
        const char *chType = "None";
        if (ch == PsmChargerType_EnoughPower) chType = "AC Adapter (USB-PD)";
        else if (ch == PsmChargerType_LowPower) chType = "USB Power (Slow)";
        draw_key_value(r, "Charger", chType, 690, 270, color_white);

        // Visual Battery progress bar
        draw_text(r, font_sm, "Capacity", 690, 310, color_grey, 0);
        draw_progress_bar(r, 820, 312, 380, 16, batt / 100.f, get_usage_color(100 - batt), color_dark_grey);
    }

    // Joy-Con battery levels
    HidPowerInfo jc_left = {0}, jc_right = {0};
    hidGetNpadPowerInfoSplit(HidNpadIdType_No1, &jc_left, &jc_right);
    
    snprintf(t, sizeof(t), "%s", get_joycon_battery_str(jc_left.battery_level));
    draw_key_value(r, "Joy-Con L", t, 690, 350, get_joycon_battery_color(jc_left.battery_level));
    
    snprintf(t, sizeof(t), "%s", get_joycon_battery_str(jc_right.battery_level));
    draw_key_value(r, "Joy-Con R", t, 690, 380, get_joycon_battery_color(jc_right.battery_level));

    // Card 3: Thermals & Display
    draw_card(r, 40, 440, 1200, 180, "Thermals & Display");
    
    // Temp
    s32 skin = 0;
    if (R_SUCCEEDED(tcInitialize())) {
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin))) {
            snprintf(t, sizeof(t), "%d.%d C", skin/1000, (skin%1000)/100);
            draw_key_value(r, "Skin Temp", t, 70, 510, get_temp_color(skin));
            
            u32 tpct = 0;
            if (skin < 25000) tpct = 0;
            else if (skin > 70000) tpct = 100;
            else tpct = (u32)((skin - 25000) * 100 / 45000);
            
            draw_progress_bar(r, 270, 512, 300, 16, tpct / 100.f, get_temp_color(skin), color_dark_grey);
        }
        tcExit();
    }

    // Brightness
    float br = 0;
    if (R_SUCCEEDED(brightness_read(&br))) {
        snprintf(t, sizeof(t), "%.0f%%", br*100);
        draw_key_value(r, "Brightness", t, 690, 510, color_yellow);
        draw_progress_bar(r, 920, 512, 280, 16, br, color_yellow, color_dark_grey);
    } else if (lbl_emulator) {
        draw_key_value(r, "Brightness", "N/A (emulator)", 690, 510, color_grey);
    }

    // Uptime
    u64 now_tick = armGetSystemTick();
    u64 elapsed = (now_tick - start_tick) / armGetSystemTickFreq();
    u32 hrs = (u32)(elapsed / 3600);
    u32 mins = (u32)((elapsed % 3600) / 60);
    u32 secs = (u32)(elapsed % 60);
    snprintf(t, sizeof(t), "%02u Hours, %02u Minutes, %02u Seconds", hrs, mins, secs);
    draw_key_value(r, "App Uptime", t, 70, 560, color_cyan);
}

// Storage
static void draw_pg1(SDL_Renderer *r) {
    char t[256];

    draw_card(r, 40, 140, 1200, 480, "Storage Volumes & Partitions");

    // SD Card
    FsFileSystem sd;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&sd, "/", &f)) && R_SUCCEEDED(fsFsGetTotalSpace(&sd, "/", &tot)) && tot > 0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            SDL_Color clr = get_usage_color(pct);
            
            draw_text(r, font_md, "SD Card (sdmc:/)", 70, 210, color_cyan, 0);
            snprintf(t, sizeof(t), "Used: %.2f GB / Total: %.2f GB (%u%%)  -  Free: %.2f GB", used/1.0e9, tot/1.0e9, pct, f/1.0e9);
            draw_text(r, font_sm, t, 70, 245, color_white, 0);
            draw_progress_bar(r, 70, 275, 1140, 16, (float)used / tot, clr, color_dark_grey);

            // Count root items
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
                snprintf(t, sizeof(t), "Root: %d folders, %d files", dir_count, file_count);
                draw_text(r, font_sm, t, 70, 300, color_grey, 0);
            }
        }
        fsFsClose(&sd);
    } else {
        draw_text(r, font_md, "SD Card (sdmc:/)", 70, 210, color_cyan, 0);
        draw_text(r, font_sm, "SD Card not inserted or failed to mount", 70, 245, color_red, 0);
    }

    // NAND System
    FsFileSystem ns;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&ns, FsBisPartitionId_System, ""))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&ns, "/", &f)) && R_SUCCEEDED(fsFsGetTotalSpace(&ns, "/", &tot)) && tot > 0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            SDL_Color clr = get_usage_color(pct);
            
            draw_text(r, font_md, "NAND System Partition", 70, 350, color_cyan, 0);
            snprintf(t, sizeof(t), "Used: %.2f GB / Total: %.2f GB (%u%%)  -  Free: %.2f GB", used/1.0e9, tot/1.0e9, pct, f/1.0e9);
            draw_text(r, font_sm, t, 70, 385, color_white, 0);
            draw_progress_bar(r, 70, 415, 1140, 16, (float)used / tot, clr, color_dark_grey);
        }
        fsFsClose(&ns);
    }

    // NAND User
    FsFileSystem nu;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&nu, FsBisPartitionId_User, ""))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&nu, "/", &f)) && R_SUCCEEDED(fsFsGetTotalSpace(&nu, "/", &tot)) && tot > 0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            SDL_Color clr = get_usage_color(pct);
            
            draw_text(r, font_md, "NAND User Partition", 70, 470, color_cyan, 0);
            snprintf(t, sizeof(t), "Used: %.2f GB / Total: %.2f GB (%u%%)  -  Free: %.2f GB", used/1.0e9, tot/1.0e9, pct, f/1.0e9);
            draw_text(r, font_sm, t, 70, 505, color_white, 0);
            draw_progress_bar(r, 70, 535, 1140, 16, (float)used / tot, clr, color_dark_grey);
        }
        fsFsClose(&nu);
    }
}

// Network
static void draw_pg2(SDL_Renderer *r) {
    char t[128];

    // Card 1: IP Config
    draw_card(r, 40, 140, 580, 480, "IP Configuration");
    u32 ip=0, msk=0, gw=0, d1=0, d2=0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip,&msk,&gw,&d1,&d2)) && ip) {
        snprintf(t,sizeof(t),"%u.%u.%u.%u",ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
        draw_key_value(r, "IP Address", t, 70, 210, color_green);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",msk&0xFF,(msk>>8)&0xFF,(msk>>16)&0xFF,(msk>>24)&0xFF);
        draw_key_value(r, "Subnet Mask", t, 70, 240, color_white);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",gw&0xFF,(gw>>8)&0xFF,(gw>>16)&0xFF,(gw>>24)&0xFF);
        draw_key_value(r, "Gateway", t, 70, 270, color_white);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",d1&0xFF,(d1>>8)&0xFF,(d1>>16)&0xFF,(d1>>24)&0xFF);
        draw_key_value(r, "Primary DNS", t, 70, 300, color_white);
        if (d2) {
            snprintf(t,sizeof(t),"%u.%u.%u.%u",d2&0xFF,(d2>>8)&0xFF,(d2>>16)&0xFF,(d2>>24)&0xFF);
            draw_key_value(r, "Secondary DNS", t, 70, 330, color_white);
        }

        // FTP quick info
        if (ftp_on) {
            snprintf(t, sizeof(t), "ftp://%u.%u.%u.%u:%d", ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF, FTP_PORT);
            draw_key_value(r, "FTP Access", t, 70, 380, color_yellow);
        }
    } else {
        draw_text(r, font_sm, "Not connected to any network.", 70, 210, color_red, 0);
        draw_text(r, font_sm, "Go to Switch System Settings to connect.", 70, 240, color_grey, 0);
        draw_text(r, font_sm, "Supports both Wi-Fi and USB Ethernet.", 70, 270, color_grey, 0);
    }

    // Card 2: Connection Status
    draw_card(r, 660, 140, 580, 230, "Connection Details");
    NifmInternetConnectionType ct; u32 ws=0; NifmInternetConnectionStatus cs;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&ct,&ws,&cs))) {
        const char *netType = "Unknown";
        if (ct == 1) netType = "Wi-Fi (Wireless)";
        else if (ct == 2) netType = "Ethernet (Wired)";
        draw_key_value(r, "Interface", netType, 690, 210, ct == 2 ? color_green : color_cyan);
        
        draw_key_value(r, "Internet", cs == 4 ? "Connected" : "Limited", 690, 240, cs == 4 ? color_green : color_yellow);

        if (ct == 1 && ws > 0) {
            draw_text(r, font_sm, "Signal Quality", 690, 290, color_grey, 0);
            draw_wifi_bars(r, 870, 290, 50, 20, ws);
            snprintf(t, sizeof(t), "%d/3", ws);
            draw_text(r, font_sm, t, 940, 290, color_cyan, 0);
        } else if (ct == 2) {
            draw_key_value(r, "Link", "Wired (Stable)", 690, 290, color_green);
        }
    }

    // Card 3: Wireless RSSI
    draw_card(r, 660, 390, 580, 230, "Wi-Fi Signal Diagnostics");
    if (R_SUCCEEDED(wlaninfInitialize())) {
        WlanInfState wst;
        if (R_SUCCEEDED(wlaninfGetState(&wst)) && wst == WlanInfState_Connected) {
            s32 rssi = 0;
            if (R_SUCCEEDED(wlaninfGetRSSI(&rssi))) {
                int qual = (rssi + 90) * 100 / 60;
                if (qual > 100) qual = 100;
                if (qual < 0) qual = 0;

                snprintf(t, sizeof(t), "%d dBm", rssi);
                draw_key_value(r, "Wi-Fi RSSI", t, 690, 460, color_white);
                
                snprintf(t, sizeof(t), "%d%%", qual);
                draw_key_value(r, "Link Quality", t, 690, 490, get_usage_color(100 - qual));
                
                draw_text(r, font_sm, "Signal Power", 690, 540, color_grey, 0);
                draw_progress_bar(r, 870, 542, 330, 16, qual / 100.f, get_usage_color(100 - qual), color_dark_grey);
            }
        } else {
            draw_text(r, font_sm, "Wi-Fi not active or not connected.", 690, 460, color_grey, 0);
        }
        wlaninfExit();
    } else {
        draw_text(r, font_sm, "WLAN diagnostics not available.", 690, 460, color_grey, 0);
    }
}

// FTP Server
static void draw_pg3(SDL_Renderer *r) {
    char t[128];
    char ipstr[32] = "0.0.0.0";
    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip)
        snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);

    // Card 1: Server Status
    draw_card(r, 40, 140, 580, 280, "FTP Server Status");
    
    if (ftp_on) {
        draw_rounded_box(r, 70, 210, 120, 32, 4, color_green);
        draw_text(r, font_sm, "RUNNING", 130, 216, color_bg, 1);
    } else {
        draw_rounded_box(r, 70, 210, 120, 32, 4, color_red);
        draw_text(r, font_sm, "STOPPED", 130, 216, color_white, 1);
    }

    snprintf(t, sizeof(t), "ftp://%s:%d", ipstr, FTP_PORT);
    draw_key_value(r, "Address", t, 70, 260, color_cyan);
    draw_key_value(r, "Root Mount", "/ -> sdmc:/ (Full SD Card)", 70, 290, color_white);
    draw_key_value(r, "Current Dir", ftp_cwd, 70, 320, color_yellow);
    
    snprintf(t, sizeof(t), "%u total (%u up, %u down)", ftp_xfer_count, ftp_upload_count, ftp_download_count);
    draw_key_value(r, "Transfers", t, 70, 350, color_white);

    if (ftp_bytes_total > 0) {
        if (ftp_bytes_total > 1073741824ULL)
            snprintf(t, sizeof(t), "%.2f GB", ftp_bytes_total / 1.0e9);
        else if (ftp_bytes_total > 1048576ULL)
            snprintf(t, sizeof(t), "%.2f MB", ftp_bytes_total / 1.0e6);
        else
            snprintf(t, sizeof(t), "%llu KB", (unsigned long long)(ftp_bytes_total / 1024));
        draw_key_value(r, "Data Total", t, 70, 380, color_white);
    }

    // Card 2: Instructions
    draw_card(r, 660, 140, 580, 280, "How to Connect");
    draw_text(r, font_sm, "Use any FTP client to connect:", 690, 210, color_grey, 0);
    draw_text(r, font_sm, "FileZilla, WinSCP, Windows Explorer", 690, 235, color_grey, 0);
    
    // Large URL
    char url[64];
    snprintf(url, sizeof(url), "ftp://%s:%d", ipstr, FTP_PORT);
    draw_text(r, font_md, url, 690, 275, color_yellow, 0);
    
    draw_text(r, font_sm, "User: anonymous  |  Pass: (anything)", 690, 315, color_white, 0);
    draw_text(r, font_sm, "Supports: PASV, STOR, RETR, REST, APPE", 690, 345, color_grey, 0);
    draw_text(r, font_sm, "Works over Wi-Fi and USB Ethernet", 690, 375, color_green, 0);

    // Card 3: Log console
    draw_card(r, 40, 440, 1200, 180, "Activity Log");
    
    mutexLock(&ftp_log_mtx);
    int start = ftp_log_idx > FTP_LOG_MAX ? ftp_log_idx - FTP_LOG_MAX : 0;
    int py = 500;
    int shown = 0;
    for (int i = start; i < (int)ftp_log_idx && i < start + FTP_LOG_MAX; i++) {
        if (ftp_log[i % FTP_LOG_MAX][0]) {
            draw_text(r, font_sm, ">", 70, py, color_cyan, 0);
            draw_text(r, font_sm, ftp_log[i % FTP_LOG_MAX], 95, py, color_white, 0);
            py += 22;
            shown++;
            if (shown >= 6) break;
        }
    }
    mutexUnlock(&ftp_log_mtx);
    if (!shown) {
        draw_text(r, font_sm, "No activity yet. Press [A] to start the server.", 70, 500, color_grey, 0);
    }
}

// Performance Clocks
static void draw_pg4(SDL_Renderer *r) {
    char t[128];

    // Clocks
    draw_card(r, 40, 140, 1200, 260, "Live Clock Frequencies");

    ClkrstSession cc, cg, cm;
    u32 cpu=0, gpu=0, mem=0;
    bool clk_ok = false;
    if (R_SUCCEEDED(clkrstOpenSession(&cc,(PcvModuleId)PcvModule_CpuBus,3)) &&
        R_SUCCEEDED(clkrstOpenSession(&cg,(PcvModuleId)PcvModule_GPU,3)) &&
        R_SUCCEEDED(clkrstOpenSession(&cm,(PcvModuleId)PcvModule_EMC,3))) {
        clkrstGetClockRate(&cc,&cpu);
        clkrstGetClockRate(&cg,&gpu);
        clkrstGetClockRate(&cm,&mem);
        clk_ok = true;

        // CPU
        u32 cpu_mhz = cpu/1000000;
        u32 cpu_max = 1785;
        snprintf(t, sizeof(t), "%u MHz / %u MHz Max", cpu_mhz, cpu_max);
        draw_key_value(r, "CPU Core Clock", t, 70, 210, color_cyan);
        draw_progress_bar(r, 370, 212, 840, 16, (float)cpu_mhz / cpu_max, color_cyan, color_dark_grey);

        // GPU
        u32 gpu_mhz = gpu/1000000;
        u32 gpu_max = 921;
        snprintf(t, sizeof(t), "%u MHz / %u MHz Max", gpu_mhz, gpu_max);
        draw_key_value(r, "GPU Core Clock", t, 70, 250, color_yellow);
        draw_progress_bar(r, 370, 252, 840, 16, (float)gpu_mhz / gpu_max, color_yellow, color_dark_grey);

        // EMC (Memory)
        u32 mem_mhz = mem/1000000;
        u32 mem_max = 1600;
        snprintf(t, sizeof(t), "%u MHz / %u MHz Max", mem_mhz, mem_max);
        draw_key_value(r, "Memory Bus Clock", t, 70, 290, color_green);
        draw_progress_bar(r, 370, 292, 840, 16, (float)mem_mhz / mem_max, color_green, color_dark_grey);

        clkrstCloseSession(&cc);
        clkrstCloseSession(&cg);
        clkrstCloseSession(&cm);
    } else {
        draw_text(r, font_sm, "Failed to read system clocks.", 70, 210, color_red, 0);
    }

    // Thermal diagnostic
    draw_card(r, 40, 420, 580, 200, "Thermal Status");
    s32 skin = 0;
    if (R_SUCCEEDED(tcInitialize())) {
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin))) {
            snprintf(t, sizeof(t), "%d.%d C", skin/1000, (skin%1000)/100);
            draw_key_value(r, "Core Skin Temp", t, 70, 490, get_temp_color(skin));
            
            const char *label = "Cool";
            if (skin >= 55000) label = "CRITICAL LIMIT";
            else if (skin >= 45000) label = "Warm / Hot";
            draw_key_value(r, "Thermal Status", label, 70, 520, get_temp_color(skin));

            draw_text(r, font_sm, "Heat Index", 70, 560, color_grey, 0);
            u32 tpct = 0;
            if (skin < 25000) tpct = 0;
            else if (skin > 70000) tpct = 100;
            else tpct = (u32)((skin - 25000) * 100 / 45000);
            draw_progress_bar(r, 270, 562, 320, 16, tpct / 100.f, get_temp_color(skin), color_dark_grey);
        }
        tcExit();
    }

    // Performance Profile
    draw_card(r, 660, 420, 580, 200, "Performance Profile");
    if (clk_ok) {
        u32 cpu_mhz = cpu/1000000;
        const char *prof = "Power Saving Mode";
        SDL_Color p_col = color_green;
        if (cpu_mhz >= 1500) { prof = "Horizon Boost Profile"; p_col = color_red; }
        else if (cpu_mhz >= 1020) { prof = "High Performance"; p_col = color_yellow; }
        draw_key_value(r, "Active Profile", prof, 690, 490, p_col);

        int dock = appletGetOperationMode();
        draw_key_value(r, "Dock Status", dock ? "Docked (High Speed)" : "Handheld (Throttled)", 690, 520, dock ? color_green : color_cyan);

        snprintf(t, sizeof(t), "CPU %uMHz / GPU %uMHz / MEM %uMHz", cpu/1000000, gpu/1000000, mem/1000000);
        draw_text(r, font_sm, t, 690, 560, color_grey, 0);
    }
}

// Page 5: Controller Button and Stick Test
static void draw_button_dot(SDL_Renderer *r, const char *label, int x, int y, bool pressed) {
    if (pressed) {
        filledCircleRGBA(r, x, y, 16, color_green.r, color_green.g, color_green.b, 255);
        draw_text(r, font_sm, label, x, y - 9, color_bg, 1);
    } else {
        filledCircleRGBA(r, x, y, 16, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
        draw_text(r, font_sm, label, x, y - 9, color_white, 1);
    }
}

static void draw_pg5(SDL_Renderer *r, PadState *pad) {
    char t[128];
    draw_card(r, 40, 140, 1200, 480, "Joy-Con Inputs & Analog Sticks Diagnostics");

    u64 held = padGetButtons(pad);
    
    // Joystick Stick position query
    HidAnalogStickState stick_l = padGetStickPos(pad, 0);
    HidAnalogStickState stick_r = padGetStickPos(pad, 1);

    // Draw Left Joycon (Blue)
    int jcl_x = 340;
    int jcl_y = 200;
    draw_rounded_box(r, jcl_x, jcl_y, 180, 360, 24, (SDL_Color){30, 120, 255, 255});
    draw_rounded_rect(r, jcl_x, jcl_y, 180, 360, 24, color_white);

    // Left analog stick
    int stick_cx = jcl_x + 90;
    int stick_cy = jcl_y + 100;
    filledCircleRGBA(r, stick_cx, stick_cy, 28, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
    // Draw offset stick position
    int off_x = (int)(stick_l.x * 20.f / 32768.f);
    int off_y = (int)(-stick_l.y * 20.f / 32768.f); // invert Y
    filledCircleRGBA(r, stick_cx + off_x, stick_cy + off_y, 16, color_cyan.r, color_cyan.g, color_cyan.b, 255);

    // D-Pad buttons
    draw_button_dot(r, "U", jcl_x + 90, jcl_y + 200, held & HidNpadButton_Up);
    draw_button_dot(r, "D", jcl_x + 90, jcl_y + 280, held & HidNpadButton_Down);
    draw_button_dot(r, "L", jcl_x + 50, jcl_y + 240, held & HidNpadButton_Left);
    draw_button_dot(r, "R", jcl_x + 130, jcl_y + 240, held & HidNpadButton_Right);

    // Minus & Stick L Click
    draw_button_dot(r, "-", jcl_x + 130, jcl_y + 40, held & HidNpadButton_Minus);
    draw_button_dot(r, "L3", jcl_x + 50, jcl_y + 320, held & HidNpadButton_StickL);

    // Draw Right Joycon (Red)
    int jcr_x = 760;
    int jcr_y = 200;
    draw_rounded_box(r, jcr_x, jcr_y, 180, 360, 24, (SDL_Color){255, 60, 80, 255});
    draw_rounded_rect(r, jcr_x, jcr_y, 180, 360, 24, color_white);

    // ABXY Buttons
    draw_button_dot(r, "X", jcr_x + 90, jcr_y + 100, held & HidNpadButton_X);
    draw_button_dot(r, "B", jcr_x + 90, jcr_y + 180, held & HidNpadButton_B);
    draw_button_dot(r, "Y", jcr_x + 50, jcr_y + 140, held & HidNpadButton_Y);
    draw_button_dot(r, "A", jcr_x + 130, jcr_y + 140, held & HidNpadButton_A);

    // Right analog stick
    int rstick_cx = jcr_x + 90;
    int rstick_cy = jcr_y + 240;
    filledCircleRGBA(r, rstick_cx, rstick_cy, 28, color_dark_grey.r, color_dark_grey.g, color_dark_grey.b, 255);
    int roff_x = (int)(stick_r.x * 20.f / 32768.f);
    int roff_y = (int)(-stick_r.y * 20.f / 32768.f);
    filledCircleRGBA(r, rstick_cx + roff_x, rstick_cy + roff_y, 16, color_cyan.r, color_cyan.g, color_cyan.b, 255);

    // Plus & Stick R Click
    draw_button_dot(r, "+", jcr_x + 50, jcr_y + 40, held & HidNpadButton_Plus);
    draw_button_dot(r, "R3", jcr_x + 130, jcr_y + 320, held & HidNpadButton_StickR);

    // Trigger states
    int trig_y = 585;
    draw_text(r, font_sm, "Triggers:", 70, trig_y, color_grey, 0);
    draw_button_dot(r, "L", 210, trig_y + 8, held & HidNpadButton_L);
    draw_button_dot(r, "R", 250, trig_y + 8, held & HidNpadButton_R);
    draw_button_dot(r, "ZL", 300, trig_y + 8, held & HidNpadButton_ZL);
    draw_button_dot(r, "ZR", 350, trig_y + 8, held & HidNpadButton_ZR);

    // Raw stick values
    snprintf(t, sizeof(t), "L Stick: X=%d  Y=%d", stick_l.x, stick_l.y);
    draw_text(r, font_sm, t, 550, trig_y, color_cyan, 0);
    snprintf(t, sizeof(t), "R Stick: X=%d  Y=%d", stick_r.x, stick_r.y);
    draw_text(r, font_sm, t, 870, trig_y, color_cyan, 0);
}

// Tools
static void draw_pg6(SDL_Renderer *r) {
    char t[64];

    // Card 1: Brightness Adjustment Slider
    draw_card(r, 40, 140, 1200, 160, "Display Brightness Control");
    float br = 0;
    if (R_SUCCEEDED(brightness_read(&br))) {
        if (ctrl_brightness < 0) ctrl_brightness = br;
        
        snprintf(t, sizeof(t), "Current: %.0f%%", ctrl_brightness * 100);
        draw_text(r, font_sm, t, 70, 200, color_yellow, 0);

        // Draw slider bar
        int sx = 70;
        int sy = 235;
        int sw = 1140;
        int sh = 10;
        draw_rounded_box(r, sx, sy, sw, sh, 5, color_dark_grey);
        
        int hx = sx + (int)(ctrl_brightness * sw);
        draw_rounded_box(r, sx, sy, hx - sx, sh, 5, color_yellow);
        filledCircleRGBA(r, hx, sy + sh/2, 16, color_white.r, color_white.g, color_white.b, 255);
        circleRGBA(r, hx, sy + sh/2, 16, color_yellow.r, color_yellow.g, color_yellow.b, 255);
        
        draw_text(r, font_sm, "D-Pad UP/DOWN to adjust", 70, 260, color_grey, 0);
    } else {
        draw_text(r, font_sm, "Brightness control not available on emulator.", 70, 200, color_grey, 0);
    }

    // Card 2: Haptic vibration test
    draw_card(r, 40, 320, 580, 180, "Haptic Vibration Test");
    draw_text(r, font_sm, "Test Joy-Con haptic motors:", 70, 380, color_grey, 0);
    
    draw_rounded_box(r, 70, 410, 220, 40, 6, color_card_border);
    draw_text(r, font_sm, "[X] Left Rumble", 180, 420, color_cyan, 1);

    draw_rounded_box(r, 310, 410, 220, 40, 6, color_card_border);
    draw_text(r, font_sm, "[Y] Right Rumble", 420, 420, color_cyan, 1);

    // Card 3: System report export
    draw_card(r, 660, 320, 580, 180, "System Report Export");
    draw_text(r, font_sm, "Export full system diagnostics to SD card:", 690, 380, color_grey, 0);
    
    draw_rounded_box(r, 690, 410, 240, 40, 6, color_card_border);
    draw_text(r, font_sm, "[B] Export Report", 810, 420, color_orange, 1);
    draw_text(r, font_sm, "Saves to sdmc:/switch/SwitchInfoNX/", 690, 465, color_grey, 0);

    // Export status message
    if (export_msg[0]) {
        u64 elapsed_ticks = armGetSystemTick() - export_msg_tick;
        u64 elapsed_sec = elapsed_ticks / armGetSystemTickFreq();
        if (elapsed_sec < 5) {
            draw_text(r, font_sm, export_msg, 690, 485, color_green, 0);
        } else {
            export_msg[0] = 0;
        }
    }

    // Card 4: Diagnostics & Environment
    draw_card(r, 40, 520, 1200, 110, "Runtime Environment");
    draw_key_value(r, "App Status", "Running Normally", 70, 580, color_green);
    draw_key_value(r, "Render", "SDL2 @ 60 FPS", 370, 580, color_white);
    
    int dock = appletGetOperationMode();
    draw_key_value(r, "Mode", dock ? "Docked" : "Handheld", 620, 580, color_white);
    draw_key_value(r, "Environment", lbl_emulator ? "Emulator" : "Retail Hardware", 850, 580, lbl_emulator ? color_yellow : color_green);
}

// About
static void draw_pg7(SDL_Renderer *r) {
    draw_card(r, 40, 140, 1200, 480, "About Switch Info NX");
    
    draw_text(r, font_lg, "Switch Info NX", 70, 200, color_cyan, 0);
    draw_text(r, font_sm, "Version 0.0.1  |  Created by dodosi", 70, 245, color_green, 0);
    
    draw_text(r, font_sm, "A premium system information and hardware diagnostic utility", 70, 285, color_white, 0);
    draw_text(r, font_sm, "for Nintendo Switch homebrew custom firmware.", 70, 310, color_white, 0);

    // Feature list
    int py = 355;
    const char *features[] = {
        "System  -  Firmware, serial number, hardware model, region, device name",
        "Storage  -  SD Card, NAND System/User partitions with usage bars",
        "Network  -  Full IP config, Wi-Fi RSSI, wired/wireless detection",
        "FTP  -  Built-in FTP server with full sdmc:/ root access (upload + download)",
        "Performance  -  Live CPU/GPU/EMC clocks, thermal monitoring, profiles",
        "Controller  -  Interactive Joy-Con button test with analog stick diagnostics",
        "Tools  -  Brightness slider, haptic rumble test, system report export",
    };
    for (int i = 0; i < 7; i++) {
        draw_text(r, font_sm, features[i], 90, py, color_grey, 0);
        py += 25;
    }

    py += 10;
    draw_text(r, font_sm, "Built with libnx, devkitA64, SDL2, SDL2_ttf, SDL2_gfx", 70, py, color_cyan, 0);
    draw_text(r, font_sm, "Thanks to the SwitchBrew and devkitPro communities.", 70, py + 25, color_grey, 0);
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
        SDL_RWops *rw_sm = SDL_RWFromMem(shared_font.address, shared_font.size);
        SDL_RWops *rw_md = SDL_RWFromMem(shared_font.address, shared_font.size);
        SDL_RWops *rw_lg = SDL_RWFromMem(shared_font.address, shared_font.size);
        font_sm = TTF_OpenFontRW(rw_sm, 1, 18);
        font_md = TTF_OpenFontRW(rw_md, 1, 24);
        font_lg = TTF_OpenFontRW(rw_lg, 1, 36);
    } else {
        // No font available
        goto exit_app;
    }

    int cur = 0;
    bool quit = false;
    
    void (*pages[PGS])(SDL_Renderer *) = { 
        draw_pg0, draw_pg1, draw_pg2, draw_pg3, draw_pg4, NULL, draw_pg6, draw_pg7 
    };

    // Auto-refresh timer
    u64 last_refresh = armGetSystemTick();
    u64 refresh_interval = 3ULL * armGetSystemTickFreq();

    while (appletMainLoop() && !quit) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) {
            quit = true;
        }

        // Navigate tabs with L/R bumpers
        if (down & HidNpadButton_L) {
            cur = (cur - 1 + PGS) % PGS;
        }
        if (down & HidNpadButton_R) {
            cur = (cur + 1) % PGS;
        }
        // Also navigate with DPad Left/Right
        if (down & HidNpadButton_Left) {
            cur = (cur - 1 + PGS) % PGS;
        }
        if (down & HidNpadButton_Right) {
            cur = (cur + 1) % PGS;
        }

        // Action button (A)
        if (down & HidNpadButton_A) {
            if (cur == 3) { // FTP tab
                if (ftp_on) ftp_stop(); else ftp_start();
            } else if (cur == 6 && lbl_auto_supported()) { // Tools tab - Auto brightness toggle
                if (R_SUCCEEDED(lblInitialize())) {
                    bool auto_br = false;
                    if (R_SUCCEEDED(lblIsAutoBrightnessControlEnabled(&auto_br))) {
                        if (auto_br)
                            lblDisableAutoBrightnessControl();
                        else
                            lblEnableAutoBrightnessControl();
                    }
                    lblExit();
                }
            }
        }

        // B button - Export on Tools page
        if (down & HidNpadButton_B) {
            if (cur == 6) {
                export_system_info();
            }
        }

        // Haptic Rumble Test triggers in Tools tab
        if (cur == 6 && vibe_init_ok) {
            if (down & HidNpadButton_X) {
                // Rumble Left Joy-Con
                HidVibrationValue vibe_val;
                vibe_val.freq_low = 160.0f;
                vibe_val.amp_low = 0.6f;
                vibe_val.freq_high = 320.0f;
                vibe_val.amp_high = 0.6f;
                hidSendVibrationValue(vibe_handles[0], &vibe_val);
                
                svcSleepThread(150000000); // rumble for 150ms
                
                vibe_val.amp_low = 0.0f;
                vibe_val.amp_high = 0.0f;
                hidSendVibrationValue(vibe_handles[0], &vibe_val);
            }
            if (down & HidNpadButton_Y) {
                // Rumble Right Joy-Con
                HidVibrationValue vibe_val;
                vibe_val.freq_low = 160.0f;
                vibe_val.amp_low = 0.6f;
                vibe_val.freq_high = 320.0f;
                vibe_val.amp_high = 0.6f;
                hidSendVibrationValue(vibe_handles[1], &vibe_val);
                
                svcSleepThread(150000000); // rumble for 150ms
                
                vibe_val.amp_low = 0.0f;
                vibe_val.amp_high = 0.0f;
                hidSendVibrationValue(vibe_handles[1], &vibe_val);
            }
        }

        // DPad Up/Down to adjust screen brightness (except on controller test page)
        if (cur != 5 && !lbl_emulator && (down & (HidNpadButton_Up | HidNpadButton_Down))) {
            if (ctrl_brightness < 0) {
                float br = 0.5f;
                if (R_FAILED(brightness_read(&br)))
                    br = 0.5f;
                ctrl_brightness = br;
            }
            if (down & HidNpadButton_Up) {
                ctrl_brightness += 0.05f;
                if (ctrl_brightness > 1.0f) ctrl_brightness = 1.0f;
            } else {
                ctrl_brightness -= 0.05f;
                if (ctrl_brightness < 0.0f) ctrl_brightness = 0.0f;
            }
            brightness_apply(ctrl_brightness);
        }

        // Force manual refresh with Y (except on tools page where Y is rumble)
        if (cur != 6 && (down & HidNpadButton_Y)) {
            refresh_count++;
        }

        // Auto-refresh check every 3s
        u64 now = armGetSystemTick();
        if ((now - last_refresh) >= refresh_interval) {
            refresh_count++;
            last_refresh = armGetSystemTick();
        }

        // Render Frame
        SDL_SetRenderDrawColor(renderer, color_bg.r, color_bg.g, color_bg.b, 255);
        SDL_RenderClear(renderer);

        draw_header(renderer);
        draw_tabs(renderer, cur);
        
        // Render Active Page
        if (cur == 5) {
            draw_pg5(renderer, &pad);
        } else if (pages[cur]) {
            pages[cur](renderer);
        }
        
        draw_footer(renderer);

        SDL_RenderPresent(renderer);
        SDL_Delay(16); // ~60 FPS
    }

exit_app:
    if (ftp_on) ftp_stop();

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
