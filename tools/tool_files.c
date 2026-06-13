#include "tools/tool_files.h"
#include "agent_conf.h"
#include "agent_tlsf.h"
#include "agent_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include "cJSON/cJSON.h"


#define MAX_FILE_SIZE (32 * 1024)

/**
 * Validate that a path starts with AGENT_BASE and contains no ".." traversal.
 */
static bool validate_path(const char *path)
{
    if(!path)
    {
        return false;
    }
    size_t base_len = strlen(AGENT_BASE);
    if(strncmp(path, AGENT_BASE, base_len) != 0)
    {
        return false;
    }
    /* Require a path separator after the base (unless base ends with '/') */
    if(base_len > 0 && AGENT_BASE[base_len - 1] != '/')
    {
        if(path[base_len] != '/')
        {
            return false;
        }
    }
    if(strstr(path, "..") != NULL)
    {
        return false;
    }
    return true;
}

/* ── read_file ─────────────────────────────────────────────── */

int tool_read_file_execute(const char *input_json, char *output, size_t output_size)
{
    if(agent_tlsf_init() != 0)
    {
        snprintf(output, output_size, "Error: allocator init failed");
        return -1;
    }

    cJSON *root = cJSON_Parse(input_json);
    if(!root)
    {
        snprintf(output, output_size, "Error: invalid JSON input");
        return -1;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if(!validate_path(path))
    {
        snprintf(output, output_size, "Error: path must start with %s/ and must not contain '..'", AGENT_BASE);
        cJSON_Delete(root);
        return -1;
    }

    FILE *f = fopen(path, "r");
    if(!f)
    {
        snprintf(output, output_size, "Error: file not found: %s", path);
        cJSON_Delete(root);
        return -1;
    }

    size_t max_read = output_size - 1;
    if(max_read > MAX_FILE_SIZE)
    {
        max_read = MAX_FILE_SIZE;
    }

    size_t n = fread(output, 1, max_read, f);
    output[n] = '\0';
    fclose(f);

    AGENT_LOG("read_file: %s (%d bytes)", path, (int)n);
    cJSON_Delete(root);
    return 0;
}

/* ── write_file ────────────────────────────────────────────── */

int tool_write_file_execute(const char *input_json, char *output, size_t output_size)
{
    if(agent_tlsf_init() != 0)
    {
        snprintf(output, output_size, "Error: allocator init failed");
        return -1;
    }

    cJSON *root = cJSON_Parse(input_json);
    if(!root)
    {
        snprintf(output, output_size, "Error: invalid JSON input");
        return -1;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));

    if(!validate_path(path))
    {
        snprintf(output, output_size, "Error: path must start with %s/ and must not contain '..'", AGENT_BASE);
        cJSON_Delete(root);
        return -1;
    }
    if(!content)
    {
        snprintf(output, output_size, "Error: missing 'content' field");
        cJSON_Delete(root);
        return -1;
    }

    if(agent_fs_ensure_parent_dir(path) != 0)
    {
        snprintf(output, output_size, "Error: cannot create parent directory for: %s", path);
        cJSON_Delete(root);
        return -1;
    }

    FILE *f = fopen(path, "w");
    if(!f)
    {
        snprintf(output, output_size, "Error: cannot open file for writing: %s", path);
        cJSON_Delete(root);
        return -1;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    if(written != len)
    {
        snprintf(output, output_size, "Error: wrote %d of %d bytes to %s", (int)written, (int)len, path);
        cJSON_Delete(root);
        return -1;
    }

    snprintf(output, output_size, "OK: wrote %d bytes to %s", (int)written, path);
    AGENT_LOG("write_file: %s (%d bytes)", path, (int)written);
    cJSON_Delete(root);
    return 0;
}

/* ── edit_file ─────────────────────────────────────────────── */

int tool_edit_file_execute(const char *input_json, char *output, size_t output_size)
{
    if(agent_tlsf_init() != 0)
    {
        snprintf(output, output_size, "Error: allocator init failed");
        return -1;
    }

    cJSON *root = cJSON_Parse(input_json);
    if(!root)
    {
        snprintf(output, output_size, "Error: invalid JSON input");
        return -1;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *old_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "old_string"));
    const char *new_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "new_string"));

    if(!validate_path(path))
    {
        snprintf(output, output_size, "Error: path must start with %s/ and must not contain '..'", AGENT_BASE);
        cJSON_Delete(root);
        return -1;
    }
    if(!old_str || !new_str)
    {
        snprintf(output, output_size, "Error: missing 'old_string' or 'new_string' field");
        cJSON_Delete(root);
        return -1;
    }

    /* Read existing file */
    FILE *f = fopen(path, "r");
    if(!f)
    {
        snprintf(output, output_size, "Error: file not found: %s", path);
        cJSON_Delete(root);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if(file_size <= 0 || file_size > MAX_FILE_SIZE)
    {
        snprintf(output, output_size, "Error: file too large or empty (%ld bytes)", file_size);
        fclose(f);
        cJSON_Delete(root);
        return -1;
    }

    /* Allocate buffer for the result (old content + possible expansion) */
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t max_result = file_size + (new_len > old_len ? new_len - old_len : 0) + 1;
    char *buf = agent_malloc(file_size + 1);
    char *result = agent_malloc(max_result);
    if(!buf || !result)
    {
        agent_free(buf);
        agent_free(result);
        fclose(f);
        snprintf(output, output_size, "Error: out of memory");
        cJSON_Delete(root);
        return -1;
    }

    size_t n = fread(buf, 1, file_size, f);
    buf[n] = '\0';
    fclose(f);

    /* Find and replace first occurrence */
    char *pos = strstr(buf, old_str);
    if(!pos)
    {
        snprintf(output, output_size, "Error: old_string not found in %s", path);
        agent_free(buf);
        agent_free(result);
        cJSON_Delete(root);
        return -1;
    }

    size_t prefix_len = pos - buf;
    memcpy(result, buf, prefix_len);
    memcpy(result + prefix_len, new_str, new_len);
    size_t suffix_start = prefix_len + old_len;
    size_t suffix_len = n - suffix_start;
    memcpy(result + prefix_len + new_len, buf + suffix_start, suffix_len);
    size_t total = prefix_len + new_len + suffix_len;
    result[total] = '\0';

    agent_free(buf);

    /* Write back */
    if(agent_fs_ensure_parent_dir(path) != 0)
    {
        snprintf(output, output_size, "Error: cannot create parent directory for: %s", path);
        agent_free(result);
        cJSON_Delete(root);
        return -1;
    }

    f = fopen(path, "w");
    if(!f)
    {
        snprintf(output, output_size, "Error: cannot open file for writing: %s", path);
        agent_free(result);
        cJSON_Delete(root);
        return -1;
    }

    fwrite(result, 1, total, f);
    fclose(f);
    agent_free(result);

    snprintf(output, output_size, "OK: edited %s (replaced %d bytes with %d bytes)", path, (int)old_len, (int)new_len);
    AGENT_LOG("edit_file: %s", path);
    cJSON_Delete(root);
    return 0;
}

/* ── list_dir ──────────────────────────────────────────────── */

int tool_list_dir_execute(const char *input_json, char *output, size_t output_size)
{
    if(agent_tlsf_init() != 0)
    {
        snprintf(output, output_size, "Error: allocator init failed");
        return -1;
    }

    cJSON *root = cJSON_Parse(input_json);
    const char *prefix = NULL;
    if(root)
    {
        cJSON *pfx = cJSON_GetObjectItem(root, "prefix");
        if(pfx && cJSON_IsString(pfx))
        {
            prefix = pfx->valuestring;
        }
    }

    agent_fs_ensure_dir(AGENT_BASE);

    DIR *dir = opendir(AGENT_BASE);
    if(!dir)
    {
        snprintf(output, output_size, "Error: cannot open %s directory", AGENT_BASE);
        cJSON_Delete(root);
        return -1;
    }

    size_t off = 0;
    struct dirent *ent;
    int count = 0;

    while((ent = readdir(dir)) != NULL && off < output_size - 1)
    {
        /* Build full path: SPIFFS entries are just filenames with embedded slashes */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", AGENT_BASE, ent->d_name);

        if(prefix && strncmp(full_path, prefix, strlen(prefix)) != 0)
        {
            continue;
        }

        off += snprintf(output + off, output_size - off, "%s\n", full_path);
        count++;
    }

    closedir(dir);

    if(count == 0)
    {
        snprintf(output, output_size, "(no files found)");
    }

    AGENT_LOG("list_dir: %d files (prefix=%s)", count, prefix ? prefix : "(none)");
    cJSON_Delete(root);
    return 0;
}

