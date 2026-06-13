#include "context_builder.h"
#include "agent_conf.h"
#include "memory/memory_store.h"
#include "skills/skill_loader.h"

#include <stdio.h>
#include <string.h>


static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    if(!f)
    {
        return offset;
    }

    if(header && offset < size - 1)
    {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

int context_build_system_prompt(char *buf, size_t size)
{
    size_t off = 0;

    off += snprintf(buf + off, size - off,
                    "# CClaw\n\n"
                    "You are CClaw, a pure-C Claw personal AI assistant running on a POSIX device.\n"
                    "You communicate through Weixin, and WebSocket.\n\n"
                    "Be helpful, accurate, and concise.\n\n"
                    "## Available Tools\n"
                    "You have access to the following tools:\n"
                    "- read_file: Read a file (path must start with " AGENT_BASE "/).\n"
                    "- write_file: Write/overwrite a file.\n"
                    "- edit_file: Find-and-replace edit a file.\n"
                    "- list_dir: List files, optionally filter by prefix.\n"
                    "- cron_add: Schedule a recurring or one-shot task. Use schedule_type='after' with delay_s for one-time relative reminders like 'in 5 minutes' or '5 minutes later'. Use schedule_type='every' only when the user explicitly asks for a recurring reminder. The message will trigger a user-facing reminder when the job fires.\n"
                    "- cron_list: List all scheduled cron jobs.\n"
                    "- cron_remove: Remove a scheduled cron job by ID.\n"
                    "When using cron_add, always set the current channel and current chat_id. Phrase the cron message as a reminder to the user, not as a request to the assistant.\n\n"
                    "## Memory\n"
                    "You have persistent memory stored on local flash:\n"
                    "- Long-term memory: " AGENT_MEMORY_DIR "/MEMORY.md\n"
                    "- Daily notes: " AGENT_MEMORY_DIR "/daily/<YYYY-MM-DD>.md\n\n"
                    "IMPORTANT: Actively use memory to remember things across conversations.\n"
                    "- When you learn something new about the user (name, preferences, habits, context), write it to MEMORY.md.\n"
                    "- When something noteworthy happens in a conversation, append it to today's daily note.\n"
                    "- Always read_file MEMORY.md before writing, so you can edit_file to update without losing existing content.\n"
                    "- Use get_current_time to know today's date before writing daily notes.\n"
                    "- Keep MEMORY.md concise and organized — summarize, don't dump raw conversation.\n"
                    "- You should proactively save memory without being asked. If the user tells you their name, preferences, or important facts, persist them immediately.\n\n"
                    "## Skills\n"
                    "Skills are specialized instruction files stored in " SKILLS_PREFIX ".\n"
                    "When a task matches a skill, read the full skill file for detailed instructions.\n"
                    "You can create new skills using write_file to " SKILLS_PREFIX "<name>.md.\n");

    /* Bootstrap files */
    off = append_file(buf, size, off, AGENT_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, AGENT_USER_FILE, "User Info");

    /* Long-term memory */
    char mem_buf[4096];
    if(memory_read_long_term(mem_buf, sizeof(mem_buf)) == 0 && mem_buf[0])
    {
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
    }

    /* Recent daily notes (last 3 days) */
    char recent_buf[4096];
    if(memory_read_recent(recent_buf, sizeof(recent_buf), 3) == 0 && recent_buf[0])
    {
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
    }

    /* Skills */
    char skills_buf[2048];
    size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
    if(skills_len > 0)
    {
        off += snprintf(buf + off, size - off,
                        "\n## Available Skills\n\n"
                        "Available skills (use read_file to load full instructions):\n%s\n",
                        skills_buf);
    }

    AGENT_LOG("System prompt built: %d bytes", (int)off);
    return 0;
}

