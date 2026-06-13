#define _POSIX_C_SOURCE 200809L

#include "memory_store.h"
#include "agent_conf.h"
#include "agent_fs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static void get_date_str(char *buf, size_t size, int days_ago)
{
    time_t now;
    time(&now);
    now -= days_ago * 86400;
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, size, "%Y-%m-%d", &tm);
}

int memory_store_init(void)
{
    agent_fs_ensure_dir(AGENT_MEMORY_DIR);
    agent_fs_ensure_dir(AGENT_MEMORY_DAILY_DIR);
    agent_fs_ensure_file(AGENT_MEMORY_FILE, "# Memory\n\n");
    AGENT_LOG("memory store initialized at %s", AGENT_BASE);
    return 0;
}

int memory_read_long_term(char *buf, size_t size)
{
    FILE *f = fopen(AGENT_MEMORY_FILE, "r");
    if(!f)
    {
        buf[0] = '\0';
        AGENT_LOG("not found %s", AGENT_MEMORY_FILE);
        return -1;
    }

    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 0;
}

int memory_write_long_term(const char *content)
{
    if(agent_fs_ensure_parent_dir(AGENT_MEMORY_FILE) != 0)
    {
        return -1;
    }

    FILE *f = fopen(AGENT_MEMORY_FILE, "w");
    if(!f)
    {
        AGENT_LOG("fail to write %s", AGENT_MEMORY_FILE);
        return -1;
    }
    fputs(content, f);
    fclose(f);

    AGENT_LOG("Long-term memory updated (%d bytes)", (int)strlen(content));
    return 0;
}

int memory_append_today(const char *note)
{
    char date_str[16];
    get_date_str(date_str, sizeof(date_str), 0);

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.md", AGENT_MEMORY_DAILY_DIR, date_str);

    if(agent_fs_ensure_parent_dir(path) != 0)
    {
        return -1;
    }

    FILE *f = fopen(path, "a");
    if(!f)
    {
        /* Try creating — if file doesn't exist yet, write header */
        f = fopen(path, "w");
        if(!f)
        {
            AGENT_LOG("Cannot open %s", path);
            return -1;
        }
        fprintf(f, "# %s\n\n", date_str);
    }

    fprintf(f, "%s\n", note);
    fclose(f);
    return 0;
}

int memory_read_recent(char *buf, size_t size, int days)
{
    size_t offset = 0;
    buf[0] = '\0';

    for(int i = 0; i < days && offset < size - 1; i++)
    {
        char date_str[16];
        get_date_str(date_str, sizeof(date_str), i);

        char path[64];
        snprintf(path, sizeof(path), "%s/%s.md", AGENT_MEMORY_DAILY_DIR, date_str);

        FILE *f = fopen(path, "r");
        if(!f)
        {
            continue;
        }

        if(offset > 0 && offset < size - 4)
        {
            offset += snprintf(buf + offset, size - offset, "\n---\n");
        }

        size_t n = fread(buf + offset, 1, size - offset - 1, f);
        offset += n;
        buf[offset] = '\0';
        fclose(f);
    }

    return 0;
}

