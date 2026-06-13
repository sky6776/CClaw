#ifndef __MOD_HEARTBEAT_H__
#define __MOD_HEARTBEAT_H__

#include <stdbool.h>

/**
 * Initialize the heartbeat service (logs ready state).
 */
int heartbeat_init(void);

/**
 * Start the heartbeat timer. Checks HEARTBEAT.md periodically
 * and sends a prompt to the agent if actionable tasks are found.
 */
int heartbeat_start(void);

/**
 * Manually trigger a heartbeat check (for CLI testing).
 * Returns true if the agent was prompted, false if no tasks found.
 */
bool heartbeat_trigger(void);

#endif

