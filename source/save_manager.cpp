#include "save_manager.h"

#include <dirent.h>
#include <errno.h>
#include <climits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FsSaveDataInfo g_saves[256];
static int g_save_count;
#define SAVE_BACKUP_MAX 256
struct SaveBackupEntry {
    char path[FS_MAX_PATH];
    u64 timestamp;
    unsigned long long key;
};
static SaveBackupEntry g_backups[SAVE_BACKUP_MAX];
static int g_backup_count;
static int g_backup_save_index = -1;
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

static bool parse_backup_name(const char *name, u64 *timestamp, unsigned long long *key) {
    if (!name || name[0] < '0' || name[0] > '9') return false;
    char *end = NULL;
    errno = 0;
    unsigned long long stamp = strtoull(name, &end, 10);
    if (errno || end == name || !end || *end != '-' || stamp > ULLONG_MAX / 100) return false;
    errno = 0;
    char *suffix_end = NULL;
    long suffix = strtol(end + 1, &suffix_end, 10);
    if (errno || suffix_end == end + 1 || !suffix_end || *suffix_end || suffix < 0 || suffix > 99)
        return false;
    if (timestamp) *timestamp = (u64)stamp;
    if (key) *key = stamp * 100 + (unsigned long long)suffix;
    return true;
}

static void invalidate_backups(void) {
    g_backup_count = 0;
    g_backup_save_index = -1;
}

static bool find_latest_backup(const char *parent, const char *exclude,
    char *out, size_t out_size) {
    DIR *dir = opendir(parent);
    if (!dir) return false;
    bool found = false;
    unsigned long long best = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        u64 timestamp = 0;
        unsigned long long key = 0;
        char candidate[FS_MAX_PATH];
        if (!parse_backup_name(ent->d_name, &timestamp, &key) ||
            !path_join(candidate, sizeof(candidate), parent, ent->d_name) ||
            (exclude && !strcmp(candidate, exclude)) ||
            !path_is_dir(candidate) || !snapshot_complete(candidate) || (found && key <= best))
            continue;
        size_t len = strnlen(candidate, sizeof(candidate));
        if (len >= out_size) continue;
        memcpy(out, candidate, len + 1);
        best = key;
        found = true;
    }
    closedir(dir);
    return found;
}

static Result mount_record(const FsSaveDataInfo *info) {
    return fsdevMountSaveData(g_mount, info->application_id, info->uid);
}

static void unmount_record(void) { fsdevUnmountDevice(g_mount); }

extern "C" Result save_manager_refresh(void) {
    g_save_count = 0;
    invalidate_backups();
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
    if (R_SUCCEEDED(rc)) invalidate_backups();
    return rc;
}

extern "C" Result save_manager_refresh_backups(int index) {
    if (index < 0 || index >= g_save_count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    invalidate_backups();
    char base[FS_MAX_PATH], parent[FS_MAX_PATH];
    save_paths(&g_saves[index], base, sizeof(base), parent, sizeof(parent));
    DIR *dir = opendir(parent);
    if (!dir) {
        if (errno == ENOENT) {
            g_backup_save_index = index;
            return 0;
        }
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        u64 timestamp = 0;
        unsigned long long key = 0;
        char candidate[FS_MAX_PATH];
        if (!parse_backup_name(ent->d_name, &timestamp, &key) ||
            !path_join(candidate, sizeof(candidate), parent, ent->d_name) ||
            !path_is_dir(candidate) || !snapshot_complete(candidate)) continue;
        int pos = g_backup_count;
        if (pos == SAVE_BACKUP_MAX && key <= g_backups[pos - 1].key) continue;
        if (pos < SAVE_BACKUP_MAX) g_backup_count++;
        else pos--;
        while (pos > 0 && g_backups[pos - 1].key < key) {
            g_backups[pos] = g_backups[pos - 1];
            pos--;
        }
        size_t len = strnlen(candidate, sizeof(candidate));
        if (len >= sizeof(g_backups[pos].path)) continue;
        memcpy(g_backups[pos].path, candidate, len + 1);
        g_backups[pos].timestamp = timestamp;
        g_backups[pos].key = key;
    }
    closedir(dir);
    g_backup_save_index = index;
    return 0;
}

extern "C" int save_manager_backup_count(int index) {
    return index == g_backup_save_index ? g_backup_count : 0;
}

extern "C" Result save_manager_get_backup_timestamp(int index, int backup_index, u64 *timestamp) {
    if (index != g_backup_save_index || backup_index < 0 || backup_index >= g_backup_count || !timestamp)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *timestamp = g_backups[backup_index].timestamp;
    return 0;
}

extern "C" Result save_manager_restore_backup(int index, int backup_index) {
    if (index < 0 || index >= g_save_count || index != g_backup_save_index ||
        backup_index < 0 || backup_index >= g_backup_count)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const FsSaveDataInfo *info = &g_saves[index];
    char target[FS_MAX_PATH], base[FS_MAX_PATH], parent[FS_MAX_PATH], rollback[FS_MAX_PATH];
    size_t target_len = strnlen(g_backups[backup_index].path, sizeof(target));
    if (target_len >= sizeof(target)) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memcpy(target, g_backups[backup_index].path, target_len + 1);
    save_paths(info, base, sizeof(base), parent, sizeof(parent));

    // Keep a fresh copy of the live save before restoring the selected snapshot.
    Result rc = save_manager_backup(index);
    if (R_FAILED(rc)) return rc;
    if (!find_latest_backup(parent, target, rollback, sizeof(rollback)))
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    rc = mount_record(info);
    if (R_FAILED(rc)) return rc;
    rc = clear_tree("svm:/");
    if (R_SUCCEEDED(rc)) rc = copy_tree(target, "svm:/");
    if (R_SUCCEEDED(rc)) rc = fsdevCommitDevice(g_mount);
    if (R_FAILED(rc)) {
        clear_tree("svm:/");
        copy_tree(rollback, "svm:/");
        fsdevCommitDevice(g_mount);
    }
    unmount_record();
    invalidate_backups();
    return rc;
}

extern "C" Result save_manager_restore_latest(int index) {
    Result rc = save_manager_refresh_backups(index);
    if (R_FAILED(rc)) return rc;
    if (!save_manager_backup_count(index)) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    return save_manager_restore_backup(index, 0);
}
