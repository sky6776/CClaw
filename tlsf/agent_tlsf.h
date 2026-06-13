#ifndef __CCLAW_TLSF_H__
#define __CCLAW_TLSF_H__

#include <stddef.h>

#include "tlsf.h"

#ifndef AGENT_TLSF_POOL_SIZE
#define AGENT_TLSF_POOL_SIZE (4U * 1024U * 1024U)
#endif

int agent_tlsf_init(void);

void *agent_malloc(size_t size);
void *agent_calloc(size_t nmemb, size_t size);
void *agent_realloc(void *ptr, size_t size);
void agent_free(void *ptr);
char *agent_strdup(const char *src);

int agent_tlsf_get_stats(tlsf_stats_t *stats);

#endif
