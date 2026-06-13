#include "heartbeat/heartbeat.h"
#include "agent_conf.h"
#include "agent_fs.h"
#include "bus/message_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <pthread.h>


#define HEARTBEAT_PROMPT \
    "Read " HEARTBEAT_FILE " and follow any instructions or tasks listed there. " \
    "If nothing needs attention, reply with just: HEARTBEAT_OK"


/* ── Content check ────────────────────────────────────────────── */

/**
 * Check if HEARTBEAT.md has actionable content.
 * Returns true if any line is NOT:
 *   - empty / whitespace-only
 *   - a markdown header (starts with #)
 *   - a completed checkbox (- [x] or * [x])
 */
static bool heartbeat_has_tasks(void)
{
    FILE *f = fopen(HEARTBEAT_FILE, "r");
    if(!f)
    {
        return false;
    }

    char line[256];
    bool found_task = false;

    while(fgets(line, sizeof(line), f))
    {
        /* Skip leading whitespace */
        const char *p = line;
        while(*p && isspace((unsigned char)*p))
        {
            p++;
        }

        /* Skip empty lines */
        if(*p == '\0')
        {
            continue;
        }

        /* Skip markdown headers */
        if(*p == '#')
        {
            continue;
        }

        /* Skip completed checkboxes: "- [x]" or "* [x]" */
        if((*p == '-' || *p == '*') && *(p + 1) == ' ' && *(p + 2) == '[')
        {
            char mark = *(p + 3);
            if((mark == 'x' || mark == 'X') && *(p + 4) == ']')
            {
                continue;
            }
        }

        /* Found an actionable line */
        found_task = true;
        break;
    }

    fclose(f);
    return found_task;
}

/* ── Send heartbeat to agent ──────────────────────────────────── */

static bool heartbeat_send(void)
{
    if(!heartbeat_has_tasks())
    {
        AGENT_LOG("No actionable tasks in HEARTBEAT.md");
        return false;
    }

    bus_msg_t msg;
    /* heartbeat 只投递固定提示文本，push 时会复制到 inbound ring。 */
    bus_msg_set(&msg, CHANNEL_SYSTEM, "heartbeat", HEARTBEAT_PROMPT);

    int err = message_bus_push_inbound(&msg);
    if(err != 0)
    {
        AGENT_LOG("Failed to push heartbeat message: %d", err);
        return false;
    }

    AGENT_LOG("Triggered agent check");
    return true;
}

/* ── Timer callback ───────────────────────────────────────────── */

static void *heartbeat_timer_callback(void *xTimer)
{
    (void)xTimer;
    heartbeat_send();
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

int heartbeat_init(void)
{
    agent_fs_ensure_file(HEARTBEAT_FILE, "# Heartbeat\n\n");
    AGENT_LOG("Heartbeat service initialized (file: %s, interval: %ds)",
              HEARTBEAT_FILE, HEARTBEAT_INTERVAL_SEC);
    return 0;
}

int heartbeat_start(void)
{
    static pthread_t s_heartbeat_timer = 0;

    if(s_heartbeat_timer)
    {
        AGENT_LOG("Heartbeat timer already running");
        return 0;
    }

    if(0 == s_heartbeat_timer)
    {
        if(0 != pthread_create(&s_heartbeat_timer, NULL, heartbeat_timer_callback, NULL))
        {
            AGENT_LOG("fail to create heartbeat_timer_callback");
            s_heartbeat_timer = 0;
            return -1;
        }
    }

    AGENT_LOG("Heartbeat started (every %d min)", HEARTBEAT_INTERVAL_SEC / 60);
    return 0;
}

bool heartbeat_trigger(void)
{
    return heartbeat_send();
}

