#include "agent_console.h"
#include "agent_conf.h"
#include "bus/message_bus.h"

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef AGENT_CONSOLE_ENABLE
#define AGENT_CONSOLE_ENABLE 0
#endif

#if AGENT_CONSOLE_ENABLE

#define AGENT_CONSOLE_LINE_MAX 1024
#define AGENT_CONSOLE_CHAT_ID  "console"
#define AGENT_CONSOLE_PROMPT   "> "

static pthread_t s_console_thread = 0;

static void console_print_prompt(void)
{
    fputs(AGENT_CONSOLE_PROMPT, stdout);
    fflush(stdout);
}

static void console_chomp(char *line)
{
    size_t len = strlen(line);

    while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
    {
        line[len - 1] = '\0';
        len--;
    }
}

static bool console_line_is_blank(const char *line)
{
    while(*line)
    {
        if(!isspace((unsigned char)*line))
        {
            return false;
        }
        line++;
    }

    return true;
}

static void console_discard_line_tail(void)
{
    int ch = 0;

    while((ch = getchar()) != '\n' && ch != EOF)
    {
    }
}

static int console_push_input(const char *line)
{
    bus_msg_t msg;

    bus_msg_set(&msg, CHANNEL_CLI, AGENT_CONSOLE_CHAT_ID, line);
    return message_bus_push_inbound(&msg);
}

static void *agent_console_task(void *arg)
{
    char line[AGENT_CONSOLE_LINE_MAX];

    (void)arg;
    AGENT_LOG("Agent console started");
    console_print_prompt();

    while(fgets(line, sizeof(line), stdin))
    {
        size_t len = strlen(line);
        bool truncated = (len > 0 && line[len - 1] != '\n');

        if(truncated)
        {
            console_discard_line_tail();
        }

        console_chomp(line);
        if(console_line_is_blank(line))
        {
            console_print_prompt();
            continue;
        }

        if(console_push_input(line) != 0)
        {
            AGENT_LOG("Console input dropped");
            console_print_prompt();
            continue;
        }

        if(truncated)
        {
            AGENT_LOG("Console input truncated to %d bytes",
                      AGENT_CONSOLE_LINE_MAX - 1);
        }
    }

    AGENT_LOG("Agent console stopped");
    s_console_thread = 0;
    return NULL;
}

int agent_console_init(void)
{
    AGENT_LOG("Agent console initialized");
    return 0;
}

int agent_console_start(void)
{
    if(s_console_thread != 0)
    {
        return 0;
    }

    if(pthread_create(&s_console_thread, NULL, agent_console_task, NULL) != 0)
    {
        AGENT_LOG("fail to create agent_console_task");
        s_console_thread = 0;
        return -1;
    }

    return 0;
}

int agent_console_send_message(const char *chat_id, const char *content)
{
    printf("\nagent[%s]:\n%s\n",
           (chat_id && chat_id[0]) ? chat_id : AGENT_CONSOLE_CHAT_ID,
           content ? content : "");
    console_print_prompt();
    return 0;
}

void agent_console_print_prompt(void)
{
    console_print_prompt();
}

#else

int agent_console_init(void)
{
    return 0;
}

int agent_console_start(void)
{
    return 0;
}

int agent_console_send_message(const char *chat_id, const char *content)
{
    (void)chat_id;
    (void)content;
    return 0;
}

void agent_console_print_prompt(void)
{
    /* console 未启用时为空操作 */
}

#endif
