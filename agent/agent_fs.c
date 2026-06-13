#include "agent_fs.h"
#include "agent_conf.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define AGENT_PATH_MAX 512

static int path_is_dir(const char *path)
{
    struct stat st;

    if(stat(path, &st) != 0)
    {
        return 0;
    }
    return S_ISDIR(st.st_mode) ? 1 : -1;
}

int agent_fs_ensure_dir(const char *path)
{
    char tmp[AGENT_PATH_MAX];
    size_t len;

    if(!path || !path[0])
    {
        return -1;
    }

    if(path_is_dir(path) == 1)
    {
        return 0;
    }

    len = strlen(path);
    if(len >= sizeof(tmp))
    {
        AGENT_LOG("Path too long for mkdir: %s", path);
        return -1;
    }

    memcpy(tmp, path, len + 1);
    while(len > 1 && tmp[len - 1] == '/')
    {
        tmp[--len] = '\0';
    }

    for(char *p = tmp + 1; *p; p++)
    {
        if(*p != '/')
        {
            continue;
        }

        *p = '\0';
        if(tmp[0] && mkdir(tmp, 0755) != 0 && errno != EEXIST)
        {
            AGENT_LOG("mkdir failed: %s errno=%d", tmp, errno);
            *p = '/';
            return -1;
        }
        *p = '/';
    }

    if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
    {
        AGENT_LOG("mkdir failed: %s errno=%d", tmp, errno);
        return -1;
    }

    if(path_is_dir(tmp) != 1)
    {
        AGENT_LOG("path exists but is not directory: %s", tmp);
        return -1;
    }

    return 0;
}

int agent_fs_ensure_parent_dir(const char *path)
{
    char parent[AGENT_PATH_MAX];
    char *slash;
    size_t len;

    if(!path || !path[0])
    {
        return -1;
    }

    slash = strrchr(path, '/');
    if(!slash)
    {
        return 0;
    }

    len = (size_t)(slash - path);
    if(len == 0)
    {
        return 0;
    }
    if(len >= sizeof(parent))
    {
        AGENT_LOG("Parent path too long: %s", path);
        return -1;
    }

    memcpy(parent, path, len);
    parent[len] = '\0';
    return agent_fs_ensure_dir(parent);
}

int agent_fs_ensure_file(const char *path, const char *default_content)
{
    FILE *f;

    if(agent_fs_ensure_parent_dir(path) != 0)
    {
        return -1;
    }

    f = fopen(path, "r");
    if(f)
    {
        fclose(f);
        return 0;
    }

    f = fopen(path, "w");
    if(!f)
    {
        AGENT_LOG("create file failed: %s errno=%d", path, errno);
        return -1;
    }

    if(default_content && default_content[0])
    {
        fputs(default_content, f);
    }
    fclose(f);
    AGENT_LOG("created file: %s", path);
    return 0;
}

int agent_fs_init_layout(void)
{
    int rc = 0;

    rc |= agent_fs_ensure_dir(AGENT_BASE);
    rc |= agent_fs_ensure_dir(AGENT_CONFIG_DIR);
    rc |= agent_fs_ensure_dir(AGENT_MEMORY_DIR);
    rc |= agent_fs_ensure_dir(AGENT_MEMORY_DAILY_DIR);
    rc |= agent_fs_ensure_dir(AGENT_SESSION_DIR);
    rc |= agent_fs_ensure_dir(AGENT_SKILLS_DIR);

    rc |= agent_fs_ensure_file(AGENT_SOUL_FILE, "# Personality\n\n");
    rc |= agent_fs_ensure_file(AGENT_USER_FILE, "# User Info\n\n");
    rc |= agent_fs_ensure_file(AGENT_MEMORY_FILE, "# Memory\n\n");
    rc |= agent_fs_ensure_file(CRON_FILE, "{\"jobs\":[]}\n");
    rc |= agent_fs_ensure_file(HEARTBEAT_FILE, "# Heartbeat\n\n");

    AGENT_LOG("agent storage layout %s at %s", rc == 0 ? "ready" : "incomplete", AGENT_BASE);
    return rc == 0 ? 0 : -1;
}
