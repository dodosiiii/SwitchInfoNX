#include "save_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FsSaveDataInfo g_saves[256];
static int g_save_count;
static const char *g_mount = "svm";
// Reused by the recursive copy routine: a 64 KiB local buffer in every
// directory frame could exhaust the Switch application's stack on deep saves.
static char g_copy_buffer[16 * 1024];

static bool path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_join(char *out, size_t capacity, const char *left, const char *right) {
    size_t a = strlen(left), b = strlen(right);
    if (a + 1 + b >= capacity) return false;
    memcpy(out, left, a);
    out[a] = '/';
    memcpy(out + a + 1, right, b + 1);
    return true;
}

static Result mkdirs(const char *path) {
    char tmp[FS_MAX_PATH];
    size_t n = strlen(path);
    if (!n || n >= sizeof(tmp)) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i]; tmp[i] = '\0';
            if (saved == '/' && i && tmp[i - 1] == ':') { tmp[i] = saved; continue; }
            if (saved == '\0' && i && tmp[i - 1] == '/') continue;
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
            tmp[i] = saved;
        }
    }
    return 0;
}

static Result copy_tree(const char *src, const char *dst) {
    DIR *dir = opendir(src);
    if (!dir) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    Result rc = mkdirs(dst);
    if (R_FAILED(rc)) { closedir(dir); return rc; }
    struct dirent *ent;
    char from[FS_MAX_PATH], to[FS_MAX_PATH];
    while ((ent = readdir(dir)) != NULL && R_SUCCEEDED(rc)) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (!strncmp(src, "sdmc:", 5) && !strcmp(ent->d_name, ".save_manager_complete")) continue;
        if (snprintf(from, sizeof(from), "%s/%s", src, ent->d_name) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", dst, ent->d_name) >= (int)sizeof(to)) {
            rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); break;
        }
        struct stat st;
        if (stat(from, &st) != 0) { rc = MAKERESULT(Module_Libnx, LibnxError_NotFound); break; }
        if (S_ISDIR(st.st_mode)) { rc = copy_tree(from, to); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
        if (!in || !out) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        while (R_SUCCEEDED(rc) && in) {
            size_t got = fread(g_copy_buffer, 1, sizeof(g_copy_buffer), in);
            if (got && fwrite(g_copy_buffer, 1, got, out) != got) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
            if (got < sizeof(g_copy_buffer)) { if (ferror(in)) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound); break; }
        }
        if (in) fclose(in);
        if (out && fclose(out) != 0 && R_SUCCEEDED(rc)) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    closedir(dir);
    return rc;
}

static Result clear_tree(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    Result rc = 0;
    struct dirent *ent;
    char child[FS_MAX_PATH];
    while ((ent = readdir(dir)) != NULL && R_SUCCEEDED(rc)) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >= (int)sizeof(child)) { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); break; }
        struct stat st;
        if (stat(child, &st) != 0) { rc = MAKERESULT(Module_Libnx, LibnxError_NotFound); break; }
        if (S_ISDIR(st.st_mode)) { rc = clear_tree(child); if (R_SUCCEEDED(rc) && rmdir(child) != 0) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound); }
        else if (unlink(child) != 0) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    closedir(dir);
    return rc;
}

static void save_paths(const FsSaveDataInfo *info, char *base, size_t base_len, char *parent, size_t parent_len) {
    snprintf(base, base_len, "sdmc:/switch/SwitchInfoNX/saves/%016llX-%016llX-%016llX",
        (unsigned long long)info->application_id,
        (unsigned long long)info->uid.uid[0], (unsigned long long)info->uid.uid[1]);
    snprintf(parent, parent_len, "%s", base);
}

static bool snapshot_complete(const char *snapshot) {
    char marker[FS_MAX_PATH];
    if (!path_join(marker, sizeof(marker), snapshot, ".save_manager_complete")) return false;
    struct stat st;
    return stat(marker, &st) == 0 && S_ISREG(st.st_mode);
}

static Result mount_record(const FsSaveDataInfo *info) {
    return fsdevMountSaveData(g_mount, info->application_id, info->uid);
}

static void unmount_record(void) { fsdevUnmountDevice(g_mount); }

extern "C" Result save_manager_refresh(void) {
    g_save_count = 0;
    FsSaveDataInfoReader reader;
    Result rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
    if (R_FAILED(rc)) return rc;
    FsSaveDataInfo batch[32];
    s64 count = 0;
    do {
        rc = fsSaveDataInfoReaderRead(&reader, batch, 32, &count);
        if (R_FAILED(rc)) break;
        for (s64 i = 0; i < count && g_save_count < 256; ++i) {
            if (batch[i].save_data_type == FsSaveDataType_Account &&
                batch[i].save_data_rank == FsSaveDataRank_Primary && batch[i].save_data_index == 0)
                g_saves[g_save_count++] = batch[i];
        }
    } while (count > 0 && g_save_count < 256);
    fsSaveDataInfoReaderClose(&reader);
    return rc;
}

extern "C" int save_manager_count(void) { return g_save_count; }

extern "C" Result save_manager_get(int index, u64 *application_id, AccountUid *uid) {
    if (index < 0 || index >= g_save_count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (application_id) *application_id = g_saves[index].application_id;
    if (uid) *uid = g_saves[index].uid;
    return 0;
}

extern "C" Result save_manager_backup(int index) {
    if (index < 0 || index >= g_save_count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const FsSaveDataInfo *info = &g_saves[index];
    Result rc = mount_record(info);
    if (R_FAILED(rc)) return rc;
    char base[FS_MAX_PATH], parent[FS_MAX_PATH], dst[FS_MAX_PATH];
    save_paths(info, base, sizeof(base), parent, sizeof(parent));
    rc = mkdirs(parent);
    if (R_SUCCEEDED(rc)) {
        unsigned long long stamp = (unsigned long long)time(NULL);
        bool slot_found = false;
        for (int suffix = 0; suffix < 100; ++suffix) {
            char leaf[40];
            snprintf(leaf, sizeof(leaf), "%llu-%02d", stamp, suffix);
            if (!path_join(dst, sizeof(dst), parent, leaf)) { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); break; }
            struct stat st;
            if (stat(dst, &st) != 0) { slot_found = true; break; }
        }
        if (R_SUCCEEDED(rc) && !slot_found) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        if (R_SUCCEEDED(rc)) rc = copy_tree("svm:/", dst);
        if (R_SUCCEEDED(rc)) {
            char marker[FS_MAX_PATH];
            if (!path_join(marker, sizeof(marker), dst, ".save_manager_complete")) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            if (R_SUCCEEDED(rc)) {
                FILE *done = fopen(marker, "wb");
                if (!done) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
                else if (fclose(done) != 0) rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
            }
        }
    }
    unmount_record();
    return rc;
}

extern "C" Result save_manager_restore_latest(int index) {
    if (index < 0 || index >= g_save_count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const FsSaveDataInfo *info = &g_saves[index];
    char base[FS_MAX_PATH], parent[FS_MAX_PATH], newest[FS_MAX_PATH];
    save_paths(info, base, sizeof(base), parent, sizeof(parent));
    DIR *dir = opendir(parent);
    if (!dir) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    unsigned long long best = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char *end = NULL;
        unsigned long long stamp = strtoull(ent->d_name, &end, 10);
        int suffix = end && *end == '-' ? atoi(end + 1) : -1;
        unsigned long long key = stamp * 100 + (suffix >= 0 ? (unsigned)suffix : 0);
        if (end && *end == '-' && key >= best) {
            char candidate[FS_MAX_PATH];
            if (path_join(candidate, sizeof(candidate), parent, ent->d_name) && path_is_dir(candidate) && snapshot_complete(candidate)) { best = key; snprintf(newest, sizeof(newest), "%s", candidate); }
        }
    }
    closedir(dir);
    if (!best) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    // Always preserve the live save first. This snapshot is also the rollback source.
    Result rc = save_manager_backup(index);
    if (R_FAILED(rc)) return rc;
    dir = opendir(parent);
    if (!dir) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    char rollback[FS_MAX_PATH] = {0}; best = 0;
    while ((ent = readdir(dir)) != NULL) {
        char *end = NULL;
        unsigned long long stamp = strtoull(ent->d_name, &end, 10);
        int suffix = end && *end == '-' ? atoi(end + 1) : -1;
        unsigned long long key = stamp * 100 + (suffix >= 0 ? (unsigned)suffix : 0);
        char candidate[FS_MAX_PATH];
        if (end && *end == '-') {
            if (!path_join(candidate, sizeof(candidate), parent, ent->d_name)) continue;
            if (path_is_dir(candidate) && snapshot_complete(candidate) && key >= best && strcmp(candidate, newest)) {
                best = key; snprintf(rollback, sizeof(rollback), "%s", candidate);
            }
        }
    }
    closedir(dir);
    if (!rollback[0]) return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    rc = mount_record(info);
    if (R_FAILED(rc)) return rc;
    rc = clear_tree("svm:/");
    if (R_SUCCEEDED(rc)) rc = copy_tree(newest, "svm:/");
    if (R_SUCCEEDED(rc)) rc = fsdevCommitDevice(g_mount);
    if (R_FAILED(rc)) {
        clear_tree("svm:/");
        copy_tree(rollback, "svm:/");
        fsdevCommitDevice(g_mount);
    }
    unmount_record();
    return rc;
}
