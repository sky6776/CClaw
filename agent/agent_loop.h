#ifndef __CCLAW_LOOP_H__
#define __CCLAW_LOOP_H__


/**
 * Initialize the agent loop.
 */
int agent_loop_init(void);

/**
 * Consumes from inbound queue, calls LLM API, pushes to outbound queue.
 */
int agent_loop_start(void);

#endif

