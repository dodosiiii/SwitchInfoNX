#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

static inline u16 swap16(u16 x) { return (x>>8) | (x<<8); }
#define htons(x) swap16(x)
#define ntohs(x) swap16(x)

#define W 80
#define BAR 16
#define PGS 7

// ─── Color codes ──────────────────────────────────────────
#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_DIM     "\x1b[2m"
#define C_ITALIC  "\x1b[3m"
#define C_UNDER   "\x1b[4m"

#define C_RED     "\x1b[31m"
#define C_GREEN   "\x1b[32m"
#define C_YELLOW  "\x1b[33m"
#define C_BLUE    "\x1b[34m"
#define C_MAGENTA "\x1b[35m"
#define C_CYAN    "\x1b[36m"
#define C_WHITE   "\x1b[37m"

#define C_BRED    "\x1b[1;31m"
#define C_BGREEN  "\x1b[1;32m"
#define C_BYELLOW "\x1b[1;33m"
#define C_BBLUE   "\x1b[1;34m"
#define C_BMAGENTA "\x1b[1;35m"
#define C_BCYAN   "\x1b[1;36m"
#define C_BWHITE  "\x1b[1;37m"

#define C_BG_BLUE   "\x1b[44m"
#define C_BG_CYAN   "\x1b[46m"
#define C_BG_GREY   "\x1b[100m"
#define C_BG_RED    "\x1b[41m"
#define C_BG_GREEN  "\x1b[42m"
#define C_BG_YELLOW "\x1b[43m"

static const char *pgname[PGS] = {
    "System", "Storage", "Network", "FTP", "Perf", "Tools", "About"
};

// Tick at app start for uptime
static u64 start_tick = 0;

// Refresh counter for animations
static u32 refresh_count = 0;

// Brightness control value
static float ctrl_brightness = -1.0f;

// ─── Drawing helpers ──────────────────────────────────────

static void centerc(const char *color, const char *s) {
    int l = (int)strlen(s), pad = (W - l) / 2;
    if (pad < 0) pad = 0;
    printf("%s", color);
    for (int i = 0; i < pad; i++) putchar(' ');
    printf("%s" C_RESET "\n", s);
}

static void box_top(void) {
    printf(C_BCYAN "+");
    for (int i = 1; i < W - 1; i++) putchar('-');
    printf("+" C_RESET "\n");
}

static void box_mid(void) {
    printf(C_CYAN "+");
    for (int i = 1; i < W - 1; i++) putchar('-');
    printf("+" C_RESET "\n");
}

static void box_bot(void) {
    printf(C_BCYAN "+");
    for (int i = 1; i < W - 1; i++) putchar('-');
    printf("+" C_RESET "\n");
}

static void box_empty(void) {
    printf(C_CYAN "|" C_RESET);
    for (int i = 1; i < W - 1; i++) putchar(' ');
    printf(C_CYAN "|" C_RESET "\n");
}

static void box_text(const char *color, const char *text) {
    int l = (int)strlen(text);
    int pad = (W - 2 - l) / 2;
    if (pad < 0) pad = 0;
    printf(C_CYAN "|" C_RESET);
    for (int i = 0; i < pad; i++) putchar(' ');
    printf("%s%s" C_RESET, color, text);
    int right = W - 2 - pad - l;
    for (int i = 0; i < right; i++) putchar(' ');
    printf(C_CYAN "|" C_RESET "\n");
}

static void hr(void) {
    printf(C_DIM C_CYAN);
    for (int i = 0; i < W; i++) putchar('-');
    printf(C_RESET "\n");
}

static void section(const char *title) {
    printf("\n");
    printf("  " C_BCYAN "[ " C_BWHITE "%s" C_BCYAN " ]" C_RESET, title);
    int used = 6 + (int)strlen(title);
    printf(C_DIM C_CYAN);
    for (int i = used; i < W - 2; i++) putchar('-');
    printf(C_RESET "\n\n");
}

static void drawbar(u32 val, u32 max, const char *color) {
    int f = (max > 0) ? (int)(val * BAR / max) : 0;
    if (f > BAR) f = BAR;
    printf(" %s[", color);
    for (int i = 0; i < f; i++) printf("\xE2\x96\x88");
    printf(C_RESET C_DIM);
    for (int i = f; i < BAR; i++) putchar('.');
    printf(C_RESET " " C_BWHITE "%3u%%" C_RESET, max ? (unsigned)(val * 100 / max) : 0);
}

// Mini bar for performance (compact)
static void minibar(u32 val, u32 max, int width, const char *color) {
    int f = (max > 0) ? (int)(val * width / max) : 0;
    if (f > width) f = width;
    printf("%s", color);
    for (int i = 0; i < f; i++) printf("\xE2\x96\x88");
    printf(C_RESET C_DIM);
    for (int i = f; i < width; i++) printf("\xE2\x96\x91");
    printf(C_RESET);
}

static const char* bar_color_usage(u32 pct) {
    if (pct < 60) return C_BGREEN;
    if (pct < 80) return C_BYELLOW;
    return C_BRED;
}

static const char* bar_color_batt(u32 pct) {
    if (pct > 60) return C_BGREEN;
    if (pct > 20) return C_BYELLOW;
    return C_BRED;
}

static const char* temp_color(s32 milliC) {
    if (milliC < 40000) return C_BGREEN;
    if (milliC < 55000) return C_BYELLOW;
    return C_BRED;
}

static void kvb(const char *k, const char *color, const char *v) {
    printf("  " C_CYAN "%-14s" C_RESET " %s%s" C_RESET "\n", k, color, v);
}

static void kvn(const char *k, const char *v) {
    printf("  " C_CYAN "%-14s" C_RESET " " C_DIM "%s" C_RESET "\n", k, v);
}

// ─── Status Bar (top) ─────────────────────────────────────

static void statusbar(void) {
    // Battery quick info
    u32 batt = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    psmGetBatteryChargePercentage(&batt);
    psmGetChargerType(&ch);

    // IP quick info
    u32 ip = 0;
    nifmGetCurrentIpAddress(&ip);

    // Time
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);

    const char *bclr = batt > 60 ? C_BGREEN : batt > 20 ? C_BYELLOW : C_BRED;

    printf(C_BG_GREY C_BWHITE " Switch Info NX" C_RESET);
    printf(C_BG_GREY C_DIM C_WHITE " v4.0" C_RESET);

    // Spacing
    int used = 20;

    // IP in middle
    if (ip) {
        char ips[20];
        snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
        int iplen = (int)strlen(ips);
        int mid = (W - iplen) / 2;
        for (int i = used; i < mid; i++) { printf(C_BG_GREY " "); used++; }
        printf(C_BG_GREY C_BCYAN "%s" C_RESET, ips);
        used += iplen;
    }

    // Right side: battery + time
    char right[40];
    if (lt) {
        snprintf(right, sizeof(right), "%s%s%u%% %02d:%02d",
            ch ? "\xE2\x9A\xA1" : "",
            bclr, batt,
            lt->tm_hour, lt->tm_min);
    } else {
        snprintf(right, sizeof(right), "%s%u%%", bclr, batt);
    }
    // Approximate visible length (removing color codes)
    int rvis = 0;
    if (lt) rvis = (ch ? 1 : 0) + (batt >= 100 ? 4 : batt >= 10 ? 3 : 2) + 1 + 5;
    else rvis = (batt >= 100 ? 4 : batt >= 10 ? 3 : 2);

    int target = W - rvis;
    for (int i = used; i < target; i++) { printf(C_BG_GREY " "); used++; }
    printf(C_BG_GREY "%s" C_RESET, right);
    printf("\n");
}

// ─── Tab bar ──────────────────────────────────────────────

static void tabs(int cur) {
    int tw = W / PGS;

    // Tab row with names
    for (int i = 0; i < PGS; i++) {
        if (i == cur) {
            printf(C_BG_CYAN C_BWHITE);
        } else {
            printf(C_BG_BLUE C_WHITE);
        }

        int ll = (int)strlen(pgname[i]);
        int pad = (tw - ll) / 2;
        if (pad < 0) pad = 0;
        for (int j = 0; j < pad; j++) putchar(' ');
        printf("%s", pgname[i]);
        int right = tw - pad - ll;
        for (int j = 0; j < right; j++) putchar(' ');
    }
    // Fill remainder
    int rem = W - tw * PGS;
    if (rem > 0) {
        printf(C_BG_BLUE);
        for (int i = 0; i < rem; i++) putchar(' ');
    }
    printf(C_RESET "\n");

    // Underline indicator
    for (int i = 0; i < PGS; i++) {
        if (i == cur) {
            printf(C_BCYAN);
            for (int j = 0; j < tw; j++) printf("\xE2\x96\x80");
        } else {
            printf(C_DIM C_BLUE);
            for (int j = 0; j < tw; j++) putchar(' ');
        }
    }
    if (rem > 0) {
        for (int i = 0; i < rem; i++) putchar(' ');
    }
    printf(C_RESET "\n");
}

// ─── Footer ───────────────────────────────────────────────

static void footer(void) {
    hr();
    // Animated dots
    const char *dots[] = { ".", "..", "...", "   " };
    int di = refresh_count % 4;

    printf("  " C_BYELLOW "L/R" C_DIM " Tab  ");
    printf(C_BYELLOW "A" C_DIM " Action  ");
    printf(C_BYELLOW "Y" C_DIM " Refresh  ");
    printf(C_BYELLOW "DPad" C_DIM " Adj  ");
    printf(C_BYELLOW "+" C_DIM " Exit");
    printf(C_RESET "\n");

    // Bottom bar
    u64 now = armGetSystemTick();
    u64 elapsed = (now - start_tick) / armGetSystemTickFreq();
    u32 hrs = (u32)(elapsed / 3600);
    u32 mins = (u32)((elapsed % 3600) / 60);
    u32 secs = (u32)(elapsed % 60);

    printf("  " C_DIM "Up %02u:%02u:%02u", hrs, mins, secs);

    int used = 13;
    int target = W - 12;
    for (int i = used; i < target; i++) putchar(' ');
    printf("Auto%s " C_BCYAN "\xE2\x97\x8F" C_RESET C_DIM " Live" C_RESET "\n", dots[di]);
}

// ─── FTP Server ───────────────────────────────────────────
#define FTP_PORT 5000
#define FTP_MAXCL 4
#define FTP_BUF 4096
#define FTP_LOG_MAX 8

static volatile int ftp_on = 0;
static int ftp_srv = -1, ftp_data = -1;
static Thread ftp_thr;
static char ftp_cwd[256] = "/";
static u32 ftp_ip = 0;
static u32 ftp_xfer_count = 0;
static u64 ftp_bytes_total = 0;

// Activity log
static char ftp_log[FTP_LOG_MAX][80];
static int ftp_log_idx = 0;
static Mutex ftp_log_mtx;

// Rename support
static char ftp_rnfr[260] = {0};

static void ftp_addlog(const char *msg) {
    mutexLock(&ftp_log_mtx);
    // Add timestamp
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt)
        snprintf(ftp_log[ftp_log_idx % FTP_LOG_MAX], 80, "%02d:%02d:%02d %s",
            lt->tm_hour, lt->tm_min, lt->tm_sec, msg);
    else
        snprintf(ftp_log[ftp_log_idx % FTP_LOG_MAX], 80, "%s", msg);
    ftp_log_idx++;
    mutexUnlock(&ftp_log_mtx);
}

static int fsend(int fd, const char *m) { return send(fd, m, strlen(m), 0); }

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
    char buf[FTP_BUF];
    fsend(fd, "220 SwitchInfoNX FTP v4.0 ready\r\n");
    ftp_data = -1;
    ftp_addlog("Client connected");

    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recv(fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        for (int i = 0; buf[i]; i++)
            if (buf[i]=='\r'||buf[i]=='\n') { buf[i]=0; break; }

        char cmd[32]={0}, arg[256]={0};
        sscanf(buf, "%31s %255[^\r\n]", cmd, arg);

        if (strcmp(cmd,"USER")==0) fsend(fd,"230 OK\r\n");
        else if (strcmp(cmd,"PASS")==0) fsend(fd,"230 OK\r\n");
        else if (strcmp(cmd,"SYST")==0) fsend(fd,"215 UNIX Type: L8\r\n");
        else if (strcmp(cmd,"FEAT")==0) {
            fsend(fd,"211-Features:\r\n SIZE\r\n PASV\r\n UTF8\r\n MDTM\r\n211 End\r\n");
        }
        else if (strcmp(cmd,"OPTS")==0) fsend(fd,"200 OK\r\n");
        else if (strcmp(cmd,"PWD")==0||strcmp(cmd,"XPWD")==0) {
            char p[512]; snprintf(p,sizeof(p),"257 \"%s\"\r\n",ftp_cwd);
            fsend(fd,p);
        }
        else if (strcmp(cmd,"QUIT")==0) {
            fsend(fd,"221 Bye\r\n");
            ftp_addlog("Client disconnected");
            break;
        }
        else if (strcmp(cmd,"TYPE")==0) fsend(fd,"200 Type set\r\n");
        else if (strcmp(cmd,"NOOP")==0) fsend(fd,"200 OK\r\n");
        else if (strcmp(cmd,"CDUP")==0||strcmp(cmd,"XCUP")==0) {
            fclean(ftp_cwd, ftp_cwd, "..");
            fsend(fd,"250 OK\r\n");
        }
        else if (strcmp(cmd,"SIZE")==0) {
            char p[512]; fclean(p,ftp_cwd,arg);
            char *fp = p; if (fp[0]=='/') fp++;
            struct stat st;
            if (stat(fp,&st)==0) {
                char z[64]; snprintf(z,sizeof(z),"213 %lu\r\n",(unsigned long)st.st_size);
                fsend(fd,z);
            } else fsend(fd,"550 File not found\r\n");
        }
        else if (strcmp(cmd,"MDTM")==0) {
            char p[512]; fclean(p,ftp_cwd,arg);
            char *fp = p; if (fp[0]=='/') fp++;
            struct stat st;
            if (stat(fp,&st)==0) {
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
            fclean(ftp_cwd,ftp_cwd,arg);
            if (!ftp_cwd[0]) strcpy(ftp_cwd,"/");
            char log[64]; snprintf(log,sizeof(log),"CWD %s", ftp_cwd);
            ftp_addlog(log);
            fsend(fd,"250 OK\r\n");
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
            char *fp = dp; if (fp[0]=='/') fp++; if (!fp[0]) fp=".";
            DIR *d = opendir(fp);
            if (!d) { fsend(fd,"450 Cannot open dir\r\n"); continue; }
            if (ftp_data<0) { closedir(d); fsend(fd,"425 No data connection\r\n"); continue; }
            struct sockaddr_in cl;
            socklen_t cln=sizeof(cl);
            int df = accept(ftp_data,(struct sockaddr*)&cl,&cln);
            if (df<0) { closedir(d); fsend(fd,"425 Accept failed\r\n"); continue; }
            close(ftp_data); ftp_data=-1;
            fsend(fd,"150 Listing\r\n");
            struct dirent *e;
            while ((e=readdir(d))!=NULL) {
                char l[512]; struct stat st;
                char f[512]; snprintf(f,sizeof(f),"%s/%s",fp,e->d_name);
                int id = (stat(f,&st)==0 && S_ISDIR(st.st_mode));
                char sz[32]="-"; if (!id) snprintf(sz,sizeof(sz),"%lu",(unsigned long)st.st_size);
                snprintf(l,sizeof(l),"%s 1 switch switch %12s Jan  1 00:00 %s\r\n",
                    id?"drwxr-xr-x":"-rw-r--r--",sz,e->d_name);
                send(df,l,strlen(l),0);
            }
            closedir(d); close(df); fsend(fd,"226 Done\r\n");
        }
        else if (strcmp(cmd,"RETR")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char *fp=p; if (fp[0]=='/') fp++;
            FILE *f = fopen(fp,"rb");
            if (!f) { fsend(fd,"550 File not found\r\n"); continue; }
            if (ftp_data<0) { fclose(f); fsend(fd,"425 No data connection\r\n"); continue; }
            struct sockaddr_in cl;
            socklen_t cln=sizeof(cl);
            int df = accept(ftp_data,(struct sockaddr*)&cl,&cln);
            if (df<0) { fclose(f); fsend(fd,"425 Accept failed\r\n"); continue; }
            close(ftp_data); ftp_data=-1;
            fsend(fd,"150 Sending\r\n");
            char db[8192]; int r;
            u64 sent = 0;
            while ((r=fread(db,1,sizeof(db),f))>0) { send(df,db,r,0); sent += r; }
            fclose(f); close(df); fsend(fd,"226 Done\r\n");
            ftp_xfer_count++;
            ftp_bytes_total += sent;
            char log[64]; snprintf(log,sizeof(log),"RETR %s (%llu B)", arg, (unsigned long long)sent);
            ftp_addlog(log);
        }
        else if (strcmp(cmd,"STOR")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char *fp=p; if (fp[0]=='/') fp++;
            FILE *f = fopen(fp,"wb");
            if (!f) { fsend(fd,"550 Cannot create file\r\n"); continue; }
            if (ftp_data<0) { fclose(f); fsend(fd,"425 No data connection\r\n"); continue; }
            struct sockaddr_in cl;
            socklen_t cln=sizeof(cl);
            int df = accept(ftp_data,(struct sockaddr*)&cl,&cln);
            if (df<0) { fclose(f); fsend(fd,"425 Accept failed\r\n"); continue; }
            close(ftp_data); ftp_data=-1;
            fsend(fd,"150 Receiving\r\n");
            char db[8192]; int r;
            u64 rcvd = 0;
            while ((r=recv(df,db,sizeof(db),0))>0) { fwrite(db,1,r,f); rcvd += r; }
            fclose(f); close(df); fsend(fd,"226 Transfer complete\r\n");
            ftp_xfer_count++;
            ftp_bytes_total += rcvd;
            char log[64]; snprintf(log,sizeof(log),"STOR %s (%llu B)", arg, (unsigned long long)rcvd);
            ftp_addlog(log);
        }
        else if (strcmp(cmd,"MKD")==0 || strcmp(cmd,"XMKD")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char *fp=p; if (fp[0]=='/') fp++;
            if (mkdir(fp, 0755)==0) {
                char r[512]; snprintf(r,sizeof(r),"257 \"%s\" created\r\n",p);
                fsend(fd,r);
                char log[64]; snprintf(log,sizeof(log),"MKD %s", arg);
                ftp_addlog(log);
            } else fsend(fd,"550 Cannot create directory\r\n");
        }
        else if (strcmp(cmd,"RMD")==0 || strcmp(cmd,"XRMD")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char *fp=p; if (fp[0]=='/') fp++;
            if (rmdir(fp)==0) {
                fsend(fd,"250 Directory removed\r\n");
                char log[64]; snprintf(log,sizeof(log),"RMD %s", arg);
                ftp_addlog(log);
            } else fsend(fd,"550 Cannot remove directory\r\n");
        }
        else if (strcmp(cmd,"DELE")==0) {
            char p[260]; fclean(p,ftp_cwd,arg);
            char *fp=p; if (fp[0]=='/') fp++;
            if (remove(fp)==0) {
                fsend(fd,"250 File deleted\r\n");
                char log[64]; snprintf(log,sizeof(log),"DELE %s", arg);
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
            char *src = ftp_rnfr; if (src[0]=='/') src++;
            char *dp = dst; if (dp[0]=='/') dp++;
            if (rename(src, dp)==0) {
                fsend(fd,"250 Rename successful\r\n");
                char log[64]; snprintf(log,sizeof(log),"RENAME -> %s", arg);
                ftp_addlog(log);
            } else fsend(fd,"550 Rename failed\r\n");
            ftp_rnfr[0] = 0;
        }
        else if (strcmp(cmd,"PORT")==0) fsend(fd,"200 Use PASV instead\r\n");
        else if (strcmp(cmd,"ABOR")==0) fsend(fd,"226 Abort OK\r\n");
        else { char er[128]; snprintf(er,sizeof(er),"500 Unknown: %s\r\n",cmd); fsend(fd,er); }
    }
    if (ftp_data>=0) { close(ftp_data); ftp_data=-1; }
    close(fd);
}

static void fserve(void *arg) {
    (void)arg;
    socketInitializeDefault();
    struct sockaddr_in a;
    ftp_srv = socket(AF_INET, SOCK_STREAM, 0);
    if (ftp_srv<0) { socketExit(); ftp_on=0; return; }
    int o=1; setsockopt(ftp_srv,SOL_SOCKET,SO_REUSEADDR,&o,sizeof(o));
    a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(FTP_PORT);
    if (bind(ftp_srv,(struct sockaddr*)&a,sizeof(a))<0||listen(ftp_srv,FTP_MAXCL)<0)
        { close(ftp_srv); socketExit(); ftp_on=0; return; }
    while (ftp_on) {
        struct sockaddr_in c;
        socklen_t cl=sizeof(c);
        int fd = accept(ftp_srv,(struct sockaddr*)&c,&cl);
        if (fd<0) { if (!ftp_on) break; continue; }
        fcli(fd, ftp_ip);
    }
    close(ftp_srv); socketExit(); ftp_on=0;
}

static void ftp_start(void) {
    if (ftp_on) return;
    strcpy(ftp_cwd,"/");
    nifmGetCurrentIpAddress(&ftp_ip);
    ftp_xfer_count = 0;
    ftp_bytes_total = 0;
    ftp_log_idx = 0;
    ftp_rnfr[0] = 0;
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
    shutdown(ftp_srv, SHUT_RDWR);
    threadWaitForExit(&ftp_thr);
    threadClose(&ftp_thr);
}

// ─── Header ───────────────────────────────────────────────

static void header(const char *title) {
    box_top();
    box_text(C_BWHITE, title);
    box_bot();
}

// ─── Page 0: System ───────────────────────────────────────

static void pg0(void) {
    header("System Information");
    char t[64];

    section("Firmware & Hardware");

    SetSysFirmwareVersion fw = {0};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        snprintf(t, sizeof(t), "%d.%d.%d", fw.major, fw.minor, fw.micro);
        kvb("Firmware", C_BWHITE, t);
    }

    SetSysSerialNumber sn;
    if (R_SUCCEEDED(setsysGetSerialNumber(&sn)))
        kvb("Serial", C_BWHITE, sn.number);

    int docked = appletGetOperationMode();
    if (docked) {
        kvb("Mode", C_BGREEN, "Docked");
    } else {
        kvb("Mode", C_BCYAN, "Handheld");
    }

    // User profile
    if (R_SUCCEEDED(accountInitialize(AccountServiceType_Application))) {
        AccountUid uid;
        if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid)) {
            AccountProfile prof;
            if (R_SUCCEEDED(accountGetProfile(&prof, uid))) {
                AccountProfileBase pb;
                if (R_SUCCEEDED(accountProfileGet(&prof, NULL, &pb)))
                    kvb("User", C_BYELLOW, pb.nickname);
                accountProfileClose(&prof);
            }
        }
        accountExit();
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
                kvb("Language", C_DIM C_WHITE, langNames[(int)langCode]);
        }
    }

    // Uptime
    u64 now_tick = armGetSystemTick();
    u64 elapsed = (now_tick - start_tick) / armGetSystemTickFreq();
    u32 hrs = (u32)(elapsed / 3600);
    u32 mins = (u32)((elapsed % 3600) / 60);
    u32 secs = (u32)(elapsed % 60);
    snprintf(t, sizeof(t), "%02uh %02um %02us", hrs, mins, secs);
    kvb("Uptime", C_DIM C_WHITE, t);

    section("Battery & Power");

    // Battery with visual
    u32 batt = 0; PsmChargerType ch = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batt))) {
        psmGetChargerType(&ch);
        const char *bc = bar_color_batt(batt);
        if (ch) {
            snprintf(t, sizeof(t), "%u%% " C_BGREEN "[CHARGING]", batt);
            printf("  " C_CYAN "%-14s" C_RESET " %s%s" C_RESET "\n", "Battery", C_BGREEN, t);
        } else {
            snprintf(t, sizeof(t), "%u%%", batt);
            kvb("Battery", bc, t);
        }
        printf("  " C_CYAN "%-14s" C_RESET, "");
        drawbar(batt, 100, bc);
        putchar('\n');
    }

    // Brightness
    float br = 0;
    if (R_SUCCEEDED(lblInitialize())) {
        if (R_SUCCEEDED(lblGetBrightnessSettingAppliedToBacklight(&br))) {
            snprintf(t, sizeof(t), "%.0f%%", br*100);
            kvb("Brightness", C_BYELLOW, t);
            printf("  " C_CYAN "%-14s" C_RESET, "");
            drawbar((u32)(br*100), 100, C_BYELLOW);
            putchar('\n');
        }
        lblExit();
    }

    section("Temperature");

    // Temperature
    s32 skin = 0;
    if (R_SUCCEEDED(tcInitialize())) {
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin))) {
            const char *tc = temp_color(skin);
            snprintf(t, sizeof(t), "%d.%d C", skin/1000, (skin%1000)/100);
            kvb("Skin Temp", tc, t);
            u32 tpct = 0;
            if (skin < 25000) tpct = 0;
            else if (skin > 70000) tpct = 100;
            else tpct = (u32)((skin - 25000) * 100 / 45000);
            printf("  " C_CYAN "%-14s" C_RESET, "");
            drawbar(tpct, 100, tc);
            putchar('\n');

            // Visual thermometer
            const char *label = skin < 35000 ? "Cool" : skin < 45000 ? "Warm" : skin < 55000 ? "Hot" : "CRITICAL";
            printf("  " C_CYAN "%-14s" C_RESET " %s%s" C_RESET "\n", "Status", tc, label);
        }
        tcExit();
    }
}

// ─── Page 1: Storage ──────────────────────────────────────

static void pg1(void) {
    header("Storage Information");
    char t[64];

    section("SD Card");

    FsFileSystem sd;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
        s64 f=0,tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&sd,"/",&f)) && R_SUCCEEDED(fsFsGetTotalSpace(&sd,"/",&tot)) && tot>0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            const char *clr = bar_color_usage(pct);

            snprintf(t, sizeof(t), "%.2f GB", tot/1.0e9);
            kvb("Total", C_BWHITE, t);
            snprintf(t, sizeof(t), "%.2f GB (%u%%)", used/1.0e9, pct);
            kvb("Used", clr, t);
            snprintf(t, sizeof(t), "%.2f GB", f/1.0e9);
            kvb("Free", C_BGREEN, t);

            printf("\n  " C_CYAN "  Storage Map:" C_RESET "\n");
            printf("    ");
            // Wide visual bar
            int barw = W - 8;
            int filled = (int)((u64)pct * barw / 100);
            if (filled > barw) filled = barw;
            printf("%s", clr);
            for (int i = 0; i < filled; i++) printf("\xE2\x96\x88");
            printf(C_DIM C_GREEN);
            for (int i = filled; i < barw; i++) printf("\xE2\x96\x91");
            printf(C_RESET "\n");
            printf("    " C_DIM "%s = Used   ", clr);
            printf(C_DIM C_GREEN "= Free" C_RESET "\n");
        } else kvn("Status", "Error reading SD card");
        fsFsClose(&sd);
    } else {
        printf("    " C_DIM C_RED "No SD card inserted" C_RESET "\n");
    }

    section("NAND System");

    FsFileSystem ns;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&ns,FsBisPartitionId_System,""))) {
        s64 f=0,tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&ns,"/",&f)) && R_SUCCEEDED(fsFsGetTotalSpace(&ns,"/",&tot)) && tot>0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            const char *clr = bar_color_usage(pct);
            snprintf(t, sizeof(t), "%.1f / %.1f GB (%u%%)", used/1.0e9, tot/1.0e9, pct);
            kvb("Used/Total", clr, t);
            printf("  " C_CYAN "%-14s" C_RESET, "");
            drawbar(pct, 100, clr);
            putchar('\n');
        } else kvn("Status", "Unavailable");
        fsFsClose(&ns);
    } else kvn("Status", "Unavailable");

    section("NAND User");

    FsFileSystem nu;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&nu,FsBisPartitionId_User,""))) {
        s64 f=0,tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&nu,"/",&f)) && R_SUCCEEDED(fsFsGetTotalSpace(&nu,"/",&tot)) && tot>0) {
            s64 used = tot - f;
            u32 pct = (u32)(used * 100 / tot);
            const char *clr = bar_color_usage(pct);
            snprintf(t, sizeof(t), "%.1f / %.1f GB (%u%%)", used/1.0e9, tot/1.0e9, pct);
            kvb("Used/Total", clr, t);
            printf("  " C_CYAN "%-14s" C_RESET, "");
            drawbar(pct, 100, clr);
            putchar('\n');
        } else kvn("Status", "Unavailable");
        fsFsClose(&nu);
    } else kvn("Status", "Unavailable");
}

// ─── Page 2: Network ─────────────────────────────────────

static void pg2(void) {
    header("Network Information");
    char t[64];

    section("IP Configuration");

    u32 ip=0,msk=0,gw=0,d1=0,d2=0;
    int connected = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip,&msk,&gw,&d1,&d2)) && ip) {
        connected = 1;
        snprintf(t,sizeof(t),"%u.%u.%u.%u",ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
        kvb("Address", C_BGREEN, t);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",msk&0xFF,(msk>>8)&0xFF,(msk>>16)&0xFF,(msk>>24)&0xFF);
        kvb("Netmask", C_BWHITE, t);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",gw&0xFF,(gw>>8)&0xFF,(gw>>16)&0xFF,(gw>>24)&0xFF);
        kvb("Gateway", C_BWHITE, t);
        snprintf(t,sizeof(t),"%u.%u.%u.%u",d1&0xFF,(d1>>8)&0xFF,(d1>>16)&0xFF,(d1>>24)&0xFF);
        kvb("DNS #1", C_DIM C_WHITE, t);
        if (d2) {
            snprintf(t,sizeof(t),"%u.%u.%u.%u",d2&0xFF,(d2>>8)&0xFF,(d2>>16)&0xFF,(d2>>24)&0xFF);
            kvb("DNS #2", C_DIM C_WHITE, t);
        }
    } else {
        printf("\n");
        centerc(C_BRED, "Not connected to any network");
        printf("\n");
        centerc(C_DIM C_WHITE, "Go to System Settings > Internet");
        return;
    }

    section("Connection Details");

    NifmInternetConnectionType ct; u32 ws=0; NifmInternetConnectionStatus cs;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&ct,&ws,&cs))) {
        if (ct == 1) {
            kvb("Type", C_BCYAN, "Wi-Fi");
        } else if (ct == 2) {
            kvb("Type", C_BGREEN, "Ethernet");
        } else {
            kvb("Type", C_BYELLOW, "Unknown");
        }

        if (cs==4) {
            kvb("Status", C_BGREEN, "Connected");
        } else {
            // Animated connecting dots
            const char *dots[] = { "Connecting", "Connecting.", "Connecting..", "Connecting..." };
            kvb("Status", C_BYELLOW, dots[refresh_count % 4]);
        }

        if (ws > 0) {
            u32 sig = ws * 33;
            if (sig > 100) sig = 100;

            // Signal bars visual
            printf("  " C_CYAN "%-14s" C_RESET " ", "Signal");
            for (u32 i = 1; i <= 3; i++) {
                if (i <= ws) printf(C_BGREEN "\xE2\x96\x88");
                else printf(C_DIM "\xE2\x96\x91");
            }
            printf(C_RESET " ");
            const char *sig_label = ws >= 3 ? "Excellent" : ws >= 2 ? "Good" : "Weak";
            const char *sig_clr = ws >= 3 ? C_BGREEN : ws >= 2 ? C_BYELLOW : C_BRED;
            printf("%s%s (%u/3)" C_RESET "\n", sig_clr, sig_label, ws);
        }
    } else if (connected) {
        kvb("Status", C_BYELLOW, "Status unknown");
    }

    // RSSI
    section("Wi-Fi Signal Quality");

    if (R_SUCCEEDED(wlaninfInitialize())) {
        WlanInfState wst;
        if (R_SUCCEEDED(wlaninfGetState(&wst)) && wst == WlanInfState_Connected) {
            s32 rssi = 0;
            if (R_SUCCEEDED(wlaninfGetRSSI(&rssi))) {
                int qual = (rssi + 90) * 100 / 60;
                if (qual > 100) qual = 100;
                if (qual < 0) qual = 0;

                snprintf(t, sizeof(t), "%d dBm", rssi);
                kvb("RSSI", C_BWHITE, t);

                const char *q_label = qual > 75 ? "Excellent" : qual > 50 ? "Good" : qual > 25 ? "Fair" : "Poor";
                const char *q_clr = qual > 75 ? C_BGREEN : qual > 50 ? C_BYELLOW : qual > 25 ? C_BYELLOW : C_BRED;

                kvb("Quality", q_clr, q_label);

                // Wide signal bar
                printf("    ");
                minibar((u32)qual, 100, W - 8, q_clr);
                putchar('\n');
            }
        } else {
            kvn("Wi-Fi", "Not connected via Wi-Fi");
        }
        wlaninfExit();
    }
}

// ─── Page 3: FTP ──────────────────────────────────────────

static void pg3(void) {
    header("FTP Server");
    char t[64];
    char ipstr[32] = "0.0.0.0";

    u32 ip=0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip)
        snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);

    section("Server Status");

    if (ftp_on) {
        // Animated running indicator
        const char *spin[] = { "\xE2\x97\x89", "\xE2\x97\x8B" };
        printf("  " C_CYAN "%-14s" C_RESET " " C_BGREEN "%s RUNNING" C_RESET "\n", "Status", spin[refresh_count % 2]);
    } else {
        kvb("Status", C_BRED, "\xE2\x97\x8F STOPPED");
    }
    kvb("IP Address", C_BWHITE, ipstr);
    snprintf(t, sizeof(t), "%d", FTP_PORT);
    kvb("Port", C_BWHITE, t);

    if (ftp_on) {
        kvb("Root", C_BWHITE, "/ (SD card)");
        snprintf(t, sizeof(t), "%u", ftp_xfer_count);
        kvb("Transfers", C_BCYAN, t);

        // Bytes transferred
        if (ftp_bytes_total > 1073741824ULL) {
            snprintf(t, sizeof(t), "%.2f GB", ftp_bytes_total / 1073741824.0);
        } else if (ftp_bytes_total > 1048576ULL) {
            snprintf(t, sizeof(t), "%.1f MB", ftp_bytes_total / 1048576.0);
        } else if (ftp_bytes_total > 1024ULL) {
            snprintf(t, sizeof(t), "%.0f KB", ftp_bytes_total / 1024.0);
        } else {
            snprintf(t, sizeof(t), "%llu B", (unsigned long long)ftp_bytes_total);
        }
        kvb("Data", C_BCYAN, t);

        section("How to Connect");

        box_top();
        box_empty();
        char url[64]; snprintf(url, sizeof(url), "ftp://%s:%d", ipstr, FTP_PORT);
        box_text(C_BYELLOW, url);
        box_empty();
        box_text(C_DIM C_WHITE, "User: anonymous  |  Pass: (anything)");
        box_text(C_DIM C_WHITE, "FileZilla / WinSCP / Explorer");
        box_empty();
        box_bot();

        // Activity log
        section("Activity Log");
        mutexLock(&ftp_log_mtx);
        int start = ftp_log_idx > FTP_LOG_MAX ? ftp_log_idx - FTP_LOG_MAX : 0;
        int shown = 0;
        for (int i = start; i < ftp_log_idx && i < start + FTP_LOG_MAX; i++) {
            if (ftp_log[i % FTP_LOG_MAX][0]) {
                printf("    " C_DIM C_CYAN ">" C_RESET " " C_DIM "%s" C_RESET "\n", ftp_log[i % FTP_LOG_MAX]);
                shown++;
            }
        }
        mutexUnlock(&ftp_log_mtx);
        if (!shown) centerc(C_DIM, "No activity yet...");

        printf("\n");
        centerc(C_BRED, "Press [A] to Stop");
    } else {
        section("Features");

        printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Upload files to your Switch\n");
        printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Download files from your Switch\n");
        printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Create and delete folders\n");
        printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Rename files and folders\n");
        printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Compatible with all FTP clients\n");
        printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Activity logging with timestamps\n");

        printf("\n");
        box_top();
        box_text(C_BGREEN, "Press [A] to Start FTP Server");
        box_bot();
    }
}

// ─── Page 4: Performance ─────────────────────────────────

static void pg4(void) {
    header("Performance Monitor");
    char t[64];

    section("Clock Speeds");

    ClkrstSession cc, cg, cm;
    u32 cpu=0, gpu=0, mem=0;
    int clk_ok = 0;
    if (R_SUCCEEDED(clkrstOpenSession(&cc,(PcvModuleId)PcvModule_CpuBus,3)) &&
        R_SUCCEEDED(clkrstOpenSession(&cg,(PcvModuleId)PcvModule_GPU,3)) &&
        R_SUCCEEDED(clkrstOpenSession(&cm,(PcvModuleId)PcvModule_EMC,3))) {
        clkrstGetClockRate(&cc,&cpu);
        clkrstGetClockRate(&cg,&gpu);
        clkrstGetClockRate(&cm,&mem);
        clk_ok = 1;

        // CPU
        u32 cpu_mhz = cpu/1000000;
        u32 cpu_max = 1785; // Max CPU for Switch
        snprintf(t, sizeof(t), "%u MHz", cpu_mhz);
        kvb("CPU Clock", C_BWHITE, t);
        printf("    ");
        minibar(cpu_mhz, cpu_max, W - 8, C_BCYAN);
        printf("\n");

        // GPU
        u32 gpu_mhz = gpu/1000000;
        u32 gpu_max = 921; // Max GPU for Switch docked
        snprintf(t, sizeof(t), "%u MHz", gpu_mhz);
        kvb("GPU Clock", C_BWHITE, t);
        printf("    ");
        minibar(gpu_mhz, gpu_max, W - 8, C_BMAGENTA);
        printf("\n");

        // Memory
        u32 mem_mhz = mem/1000000;
        u32 mem_max = 1600;
        snprintf(t, sizeof(t), "%u MHz", mem_mhz);
        kvb("Memory", C_BWHITE, t);
        printf("    ");
        minibar(mem_mhz, mem_max, W - 8, C_BGREEN);
        printf("\n");

        clkrstCloseSession(&cc);
        clkrstCloseSession(&cg);
        clkrstCloseSession(&cm);
    }

    section("Thermal Status");

    s32 skin = 0;
    if (R_SUCCEEDED(tcInitialize())) {
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin))) {
            const char *tc = temp_color(skin);
            snprintf(t, sizeof(t), "%d.%d C", skin/1000, (skin%1000)/100);
            kvb("Temperature", tc, t);

            // Visual thermometer bar
            u32 tpct = 0;
            if (skin < 25000) tpct = 0;
            else if (skin > 70000) tpct = 100;
            else tpct = (u32)((skin - 25000) * 100 / 45000);

            printf("    ");
            printf(C_BBLUE);
            int tw = W - 8;
            int tf = (int)(tpct * tw / 100);
            if (tf > tw) tf = tw;
            // Cold zone (blue) -> warm (yellow) -> hot (red)
            for (int i = 0; i < tf; i++) {
                if (i < tw / 3) printf(C_BBLUE);
                else if (i < tw * 2 / 3) printf(C_BYELLOW);
                else printf(C_BRED);
                printf("\xE2\x96\x88");
            }
            printf(C_DIM);
            for (int i = tf; i < tw; i++) printf("\xE2\x96\x91");
            printf(C_RESET "\n");

            const char *label = skin < 35000 ? "Cool" : skin < 45000 ? "Warm" : skin < 55000 ? "Hot" : "CRITICAL!";
            kvb("Status", tc, label);
        }
        tcExit();
    }

    section("Power Status");

    u32 batt = 0; PsmChargerType ch = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batt))) {
        psmGetChargerType(&ch);
        const char *bc = bar_color_batt(batt);

        snprintf(t, sizeof(t), "%u%%", batt);
        if (ch) {
            printf("  " C_CYAN "%-14s" C_RESET " %s%s" C_RESET " " C_BGREEN "[CHARGING]" C_RESET "\n", "Battery", bc, t);
        } else {
            kvb("Battery", bc, t);
        }
        printf("    ");
        minibar(batt, 100, W - 8, bc);
        putchar('\n');

        // Estimated time remaining (rough estimate)
        if (!ch && batt > 0) {
            // Rough estimate: ~3h at 100% for handheld
            u32 est_min = batt * 180 / 100;
            snprintf(t, sizeof(t), "~%uh %um (estimate)", est_min / 60, est_min % 60);
            kvb("Remaining", C_DIM C_WHITE, t);
        }
    }

    if (clk_ok) {
        section("Performance Profile");
        u32 cpu_mhz = cpu/1000000;
        if (cpu_mhz >= 1500) {
            kvb("Profile", C_BRED, "Boost Mode");
        } else if (cpu_mhz >= 1000) {
            kvb("Profile", C_BYELLOW, "High Performance");
        } else {
            kvb("Profile", C_BGREEN, "Power Saving");
        }

        int docked = appletGetOperationMode();
        kvb("Dock Status", docked ? C_BGREEN : C_BCYAN, docked ? "Docked (Higher clocks)" : "Handheld (Battery saving)");
    }
}

// ─── Page 5: Tools ────────────────────────────────────────

static void pg5(void) {
    header("Tools & Utilities");

    section("Display Control");

    // Current brightness
    float br = 0;
    if (R_SUCCEEDED(lblInitialize())) {
        lblGetBrightnessSettingAppliedToBacklight(&br);
        if (ctrl_brightness < 0) ctrl_brightness = br;

        char t[32];
        snprintf(t, sizeof(t), "%.0f%%", ctrl_brightness * 100);
        kvb("Brightness", C_BYELLOW, t);

        // Visual slider
        printf("    ");
        int slw = W - 8;
        int pos = (int)(ctrl_brightness * slw);
        if (pos > slw) pos = slw;
        printf(C_BYELLOW);
        for (int i = 0; i < pos; i++) printf("\xE2\x96\x88");
        if (pos < slw) printf(C_BWHITE "\xE2\x97\x8F"); // Slider knob
        printf(C_DIM);
        for (int i = pos + 1; i < slw; i++) printf("\xE2\x96\x91");
        printf(C_RESET "\n");
        printf("    " C_DIM "Use DPad Up/Down to adjust" C_RESET "\n");

        lblExit();
    }

    section("System Status");

    // Mode info
    int docked = appletGetOperationMode();
    kvb("Mode", docked ? C_BGREEN : C_BCYAN, docked ? "Docked" : "Handheld");

    // Network status
    u32 ip = 0;
    nifmGetCurrentIpAddress(&ip);
    kvb("Network", ip ? C_BGREEN : C_BRED, ip ? "Connected" : "Disconnected");

    // FTP status
    kvb("FTP Server", ftp_on ? C_BGREEN : C_BRED, ftp_on ? "Running" : "Stopped");

    // SD Card presence
    FsFileSystem sd;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
        s64 f=0, tot=0;
        if (R_SUCCEEDED(fsFsGetFreeSpace(&sd,"/",&f)) && R_SUCCEEDED(fsFsGetTotalSpace(&sd,"/",&tot)) && tot>0) {
            char t[32];
            snprintf(t, sizeof(t), "Inserted (%.1f GB free)", f/1.0e9);
            kvb("SD Card", C_BGREEN, t);
        }
        fsFsClose(&sd);
    } else {
        kvb("SD Card", C_BRED, "Not inserted");
    }

    section("Quick Actions");

    printf("    " C_BYELLOW "[A]" C_RESET " on FTP tab  - Start/Stop FTP\n");
    printf("    " C_BYELLOW "[Y]" C_RESET " anywhere    - Force refresh\n");
    printf("    " C_BYELLOW "DPad U/D" C_RESET "       - Brightness control\n");
    printf("    " C_BYELLOW "[+]" C_RESET "             - Exit application\n");

    section("Diagnostics");

    // Memory info (rough)
    kvn("App Memory", "Running normally");
    kvn("Refresh Rate", "3 second auto-refresh");

    char ct[32];
    snprintf(ct, sizeof(ct), "%u", refresh_count);
    kvn("Refresh Count", ct);
}

// ─── Page 6: About ────────────────────────────────────────

static void pg6(void) {
    header("About Switch Info NX");

    printf("\n");
    box_top();
    box_empty();
    box_text(C_BCYAN, "Switch Info NX");
    box_text(C_BYELLOW, "Version 4.0");
    box_empty();
    box_text(C_DIM C_WHITE, "A comprehensive system info tool");
    box_text(C_DIM C_WHITE, "for Nintendo Switch homebrew");
    box_empty();
    box_bot();

    section("Features");

    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " System info: firmware, serial, language, user\n");
    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Storage: SD Card + NAND (System/User)\n");
    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Network: IP config, Wi-Fi signal, RSSI\n");
    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " FTP server: upload/download/rename/delete\n");
    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Performance: CPU/GPU/MEM clocks, thermals\n");
    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Tools: brightness control, diagnostics\n");
    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Live auto-refresh every 3 seconds\n");
    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Battery & charging monitoring\n");
    printf("    " C_BGREEN "\xE2\x9C\x93" C_RESET " Uptime tracking\n");

    section("Controls");

    printf("    " C_BYELLOW "L / R" C_RESET "         Navigate tabs\n");
    printf("    " C_BYELLOW "B / X" C_RESET "         Navigate tabs (alt)\n");
    printf("    " C_BYELLOW "A" C_RESET "             Action (context-dependent)\n");
    printf("    " C_BYELLOW "Y" C_RESET "             Force refresh\n");
    printf("    " C_BYELLOW "DPad U/D" C_RESET "      Adjust brightness\n");
    printf("    " C_BYELLOW "+" C_RESET "             Exit\n");

    section("Credits");

    printf("    " C_DIM "Built with libnx + devkitA64" C_RESET "\n");
    printf("    " C_DIM "Thanks to the Switch homebrew community" C_RESET "\n");
    printf("    " C_DIM "github.com/switchbrew/libnx" C_RESET "\n");
    printf("\n");
}

// ─── Main ──────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    consoleInit(NULL);

    setsysInitialize();
    setInitialize();
    psmInitialize();
    nifmInitialize(NifmServiceType_User);
    clkrstInitialize();

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    start_tick = armGetSystemTick();
    ctrl_brightness = -1.0f;

    void (*pages[PGS])(void) = { pg0, pg1, pg2, pg3, pg4, pg5, pg6 };
    int cur = 0;

    // Initial draw
    consoleClear();
    statusbar();
    tabs(cur);
    pages[cur]();
    footer();

    // Auto-refresh (3 seconds)
    u64 last_refresh = armGetSystemTick();
    u64 refresh_interval = 3ULL * armGetSystemTickFreq();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) break;

        int need_redraw = 0;

        // A = action
        if (down & HidNpadButton_A) {
            if (cur == 3) {
                if (ftp_on) ftp_stop(); else ftp_start();
            }
            need_redraw = 1;
        }

        // L = previous tab
        if (down & HidNpadButton_L) {
            cur = (cur - 1 + PGS) % PGS;
            need_redraw = 1;
        }

        // R = next tab
        if (down & HidNpadButton_R) {
            cur = (cur + 1) % PGS;
            need_redraw = 1;
        }

        // B = previous tab, X = next tab
        if (down & HidNpadButton_B) {
            cur = (cur - 1 + PGS) % PGS;
            need_redraw = 1;
        }
        if (down & HidNpadButton_X) {
            cur = (cur + 1) % PGS;
            need_redraw = 1;
        }

        // DPad Left/Right = also navigate tabs
        if (down & HidNpadButton_Left) {
            cur = (cur - 1 + PGS) % PGS;
            need_redraw = 1;
        }
        if (down & HidNpadButton_Right) {
            cur = (cur + 1) % PGS;
            need_redraw = 1;
        }

        // DPad Up/Down = brightness control (Tools page, but works everywhere)
        if (down & HidNpadButton_Up) {
            if (ctrl_brightness < 0) {
                float br = 0.5f;
                if (R_SUCCEEDED(lblInitialize())) {
                    lblGetBrightnessSettingAppliedToBacklight(&br);
                    lblExit();
                }
                ctrl_brightness = br;
            }
            ctrl_brightness += 0.05f;
            if (ctrl_brightness > 1.0f) ctrl_brightness = 1.0f;
            if (R_SUCCEEDED(lblInitialize())) {
                lblSetBrightnessSettingForCurrentController(ctrl_brightness);
                lblExit();
            }
            need_redraw = 1;
        }
        if (down & HidNpadButton_Down) {
            if (ctrl_brightness < 0) {
                float br = 0.5f;
                if (R_SUCCEEDED(lblInitialize())) {
                    lblGetBrightnessSettingAppliedToBacklight(&br);
                    lblExit();
                }
                ctrl_brightness = br;
            }
            ctrl_brightness -= 0.05f;
            if (ctrl_brightness < 0.0f) ctrl_brightness = 0.0f;
            if (R_SUCCEEDED(lblInitialize())) {
                lblSetBrightnessSettingForCurrentController(ctrl_brightness);
                lblExit();
            }
            need_redraw = 1;
        }

        // Y = manual refresh
        if (down & HidNpadButton_Y) {
            need_redraw = 1;
        }

        // Auto-refresh
        u64 now = armGetSystemTick();
        if ((now - last_refresh) >= refresh_interval) {
            need_redraw = 1;
        }

        if (need_redraw) {
            refresh_count++;
            consoleClear();
            statusbar();
            tabs(cur);
            pages[cur]();
            footer();
            last_refresh = armGetSystemTick();
        }

        consoleUpdate(NULL);
        svcSleepThread(50000000);
    }

    if (ftp_on) ftp_stop();
    clkrstExit();
    nifmExit();
    psmExit();
    setExit();
    setsysExit();
    consoleExit(NULL);
    return 0;
}
