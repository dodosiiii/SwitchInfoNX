#pragma once
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

Result save_manager_refresh(void);
int save_manager_count(void);
Result save_manager_get(int index, u64 *application_id, AccountUid *uid);
Result save_manager_backup(int index);
Result save_manager_restore_latest(int index);

#ifdef __cplusplus
}
#endif
