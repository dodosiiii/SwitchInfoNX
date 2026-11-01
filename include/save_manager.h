#pragma once
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

Result save_manager_refresh(void);
int save_manager_count(void);
Result save_manager_get(int index, u64 *application_id, AccountUid *uid);
Result save_manager_backup(int index);
Result save_manager_refresh_backups(int index);
int save_manager_backup_count(int index);
Result save_manager_get_backup_timestamp(int index, int backup_index, u64 *timestamp);
Result save_manager_restore_backup(int index, int backup_index);
Result save_manager_restore_latest(int index);

#ifdef __cplusplus
}
#endif
