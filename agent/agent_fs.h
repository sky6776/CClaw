#ifndef __CCLAW_FS_H__
#define __CCLAW_FS_H__

int agent_fs_ensure_dir(const char *path);
int agent_fs_ensure_parent_dir(const char *path);
int agent_fs_ensure_file(const char *path, const char *default_content);
int agent_fs_init_layout(void);

#endif
