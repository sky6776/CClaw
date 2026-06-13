#include "tool_registry.h"
#include "agent_conf.h"
#include "agent_tlsf.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON/cJSON.h"


#define MAX_TOOLS 16

static tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const tool_t *tool)
{
    if(s_tool_count >= MAX_TOOLS)
    {
        AGENT_LOG("Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    AGENT_LOG("Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if(!arr)
    {
        AGENT_LOG("Failed to allocate tools JSON array");
        return;
    }

    for(int i = 0; i < s_tool_count; i++)
    {
        cJSON *tool = cJSON_CreateObject();
        if(!tool)
        {
            AGENT_LOG("Failed to allocate tool JSON for %s", s_tools[i].name);
            continue;
        }
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if(schema)
        {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    if(NULL != s_tools_json)
    {
        agent_free(s_tools_json);
    }
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if(s_tools_json)
    {
        AGENT_LOG("Tools JSON built (%d tools, %d bytes)",
                  s_tool_count, (int)strlen(s_tools_json));
    }
    else
    {
        AGENT_LOG("Failed to serialize tools JSON (%d tools)", s_tool_count);
    }
}

int tool_registry_init(void)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }

    s_tool_count = 0;

    /* Register read_file */
    tool_t rf =
    {
        .name = "read_file",
        .description = "Read a file from agent storage. Path must start with " AGENT_BASE "/.",
        .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " AGENT_BASE "/\"}},"
        "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    tool_t wf =
    {
        .name = "write_file",
        .description = "Write or overwrite a file on agent storage. Path must start with " AGENT_BASE "/.",
        .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " AGENT_BASE "/\"},"
        "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
        "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    tool_t ef =
    {
        .name = "edit_file",
        .description = "Find and replace text in a file on agent. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " AGENT_BASE "/\"},"
        "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
        "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
        "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    tool_t ld =
    {
        .name = "list_dir",
        .description = "List files on agent storage, optionally filtered by path prefix.",
        .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " AGENT_BASE "/memory/\"}},"
        "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    tool_t ca =
    {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. Use 'after' for one-time relative reminders like 'in 5 minutes'; use 'every' only for explicit recurring reminders. The message will trigger a user-facing reminder when the job fires.",
        .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
        "\"schedule_type\":{\"type\":\"string\",\"description\":\"'after' for one-shot relative delay, 'at' for one-shot unix timestamp, or 'every' for recurring interval\"},"
        "\"delay_s\":{\"type\":\"integer\",\"description\":\"Delay in seconds (required for 'after'; use for requests like '5 minutes later' or 'in 10 minutes')\"},"
        "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every'; only use when the user explicitly asks for a recurring reminder)\"},"
        "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
        "\"message\":{\"type\":\"string\",\"description\":\"Reminder content for the user when the job fires; phrase it as something the assistant should tell the user, not as a request to the assistant\"},"
        "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram' or 'weixin'). If omitted, current turn channel is used when available\"},"
        "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel is 'telegram' or 'weixin'. If omitted during a Telegram/Weixin turn, current chat_id is used\"}"
        "},"
        "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    tool_t cl =
    {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{},"
        "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    tool_t cr =
    {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
        "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    build_tools_json();

    AGENT_LOG("Tool registry initialized");
    return 0;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

int tool_registry_execute(const char *name, const char *input_json,
                          char *output, size_t output_size)
{
    for(int i = 0; i < s_tool_count; i++)
    {
        if(strcmp(s_tools[i].name, name) == 0)
        {
            AGENT_LOG("Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    AGENT_LOG("Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return -1;
}

