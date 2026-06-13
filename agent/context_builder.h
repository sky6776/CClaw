#ifndef __MOD_CONTEXT_BUILDER_H__
#define __MOD_CONTEXT_BUILDER_H__


#include <stddef.h>

/**
 * Build the system prompt from bootstrap files (SOUL.md, USER.md)
 * and memory context (MEMORY.md + recent daily notes).
 *
 * @param buf   Output buffer (caller allocates, recommend MIMI_CONTEXT_BUF_SIZE)
 * @param size  Buffer size
 */
int context_build_system_prompt(char *buf, size_t size);

#endif

