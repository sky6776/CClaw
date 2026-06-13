#include "skill_loader.h"
#include "agent_conf.h"
#include "agent_fs.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>


/*
 * Skills are stored as markdown files in agent/skills/
 */
int skill_loader_init(void)
{
    AGENT_LOG("Initializing skills system");
    agent_fs_ensure_dir(AGENT_SKILLS_DIR);

    DIR *dir = opendir(AGENT_SKILLS_DIR);
    if(!dir)
    {
        AGENT_LOG("Cannot open %s - skills may not be available", AGENT_SKILLS_DIR);
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    while((ent = readdir(dir)) != NULL)
    {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if(name[0] != '.' && len > 3 &&
                strcmp(name + len - 3, ".md") == 0)
        {
            count++;
        }
    }
    closedir(dir);

    AGENT_LOG("Skills system ready (%d skills)", count);
    return 0;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title".
 * Writes the title (without "# " prefix) into out.
 */
static void extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if(len >= 2 && line[0] == '#' && line[1] == ' ')
    {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while(len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' '))
    {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while(fgets(line, sizeof(line), f) && off < out_size - 1)
    {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if(len == 0 || (len == 1 && line[0] == '\n') ||
                (len >= 2 && line[0] == '#' && line[1] == '#'))
        {
            break;
        }

        /* Skip leading blank lines */
        if(off == 0 && line[0] == '\n')
        {
            continue;
        }

        /* Trim trailing newline for concatenation */
        if(line[len - 1] == '\n')
        {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while(off > 0 && out[off - 1] == ' ')
    {
        off--;
    }
    out[off] = '\0';
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    agent_fs_ensure_dir(AGENT_SKILLS_DIR);

    DIR *dir = opendir(AGENT_SKILLS_DIR);
    if(!dir)
    {
        AGENT_LOG("Cannot open %s for skill enumeration", AGENT_SKILLS_DIR);
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;

    while((ent = readdir(dir)) != NULL && off < size - 1)
    {
        const char *name = ent->d_name;

        if(name[0] == '.')
        {
            continue;
        }

        size_t name_len = strlen(name);
        if(name_len < 4)
        {
            continue;
        }
        if(strcmp(name + name_len - 3, ".md") != 0)
        {
            continue;
        }

        /* Build full path */
        char full_path[296];
        snprintf(full_path, sizeof(full_path), "%s/%s", AGENT_SKILLS_DIR, name);

        FILE *f = fopen(full_path, "r");
        if(!f)
        {
            continue;
        }

        /* Read first line for title */
        char first_line[128];
        if(!fgets(first_line, sizeof(first_line), f))
        {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[256];
        extract_description(f, desc, sizeof(desc));
        fclose(f);

        /* Append to summary */
        off += snprintf(buf + off, size - off,
                        "- **%s**: %s (read with: read_file %s)\n",
                        title, desc, full_path);
    }

    closedir(dir);

    buf[off] = '\0';
    AGENT_LOG("Skills summary: %d bytes", (int)off);
    return off;
}

