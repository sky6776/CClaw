#include "agent_tlsf.h"

#include "agent_conf.h"
#include "cJSON/cJSON.h"
#include "tlsf_thread.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

static unsigned char s_agent_tlsf_pool[AGENT_TLSF_POOL_SIZE]
__attribute__((aligned(TLSF_CACHELINE_SIZE)));
static tlsf_thread_t s_agent_tlsf;
static pthread_once_t s_agent_tlsf_once = PTHREAD_ONCE_INIT;
static int s_agent_tlsf_ready = 0;

static void agent_tlsf_log_alloc_failed(const char *op, size_t size)
{
    tlsf_stats_t stats;

    if(s_agent_tlsf_ready && tlsf_thread_stats(&s_agent_tlsf, &stats) == 0)
    {
        AGENT_LOG("%s failed: size=%u free=%u largest_free=%u used=%u blocks=%u free_blocks=%u overhead=%u",
                  op,
                  (unsigned)size,
                  (unsigned)stats.total_free,
                  (unsigned)stats.largest_free,
                  (unsigned)stats.total_used,
                  (unsigned)stats.block_count,
                  (unsigned)stats.free_count,
                  (unsigned)stats.overhead);
    }
    else
    {
        AGENT_LOG("%s failed: size=%u tlsf_ready=%d",
                  op, (unsigned)size, s_agent_tlsf_ready);
    }
}

static void agent_tlsf_bootstrap(void)
{
    size_t usable = tlsf_thread_init(&s_agent_tlsf,
                                     s_agent_tlsf_pool,
                                     sizeof(s_agent_tlsf_pool));
    if(usable == 0)
    {
        AGENT_LOG("TLSF init failed (%u bytes)",
                  (unsigned)sizeof(s_agent_tlsf_pool));
        return;
    }

    cJSON_Hooks hooks;
    hooks.malloc_fn = agent_malloc;
    hooks.free_fn = agent_free;
    cJSON_InitHooks(&hooks);

    s_agent_tlsf_ready = 1;
    AGENT_LOG("TLSF initialized (%u bytes pool, %u usable, %d arenas)",
              (unsigned)sizeof(s_agent_tlsf_pool),
              (unsigned)usable,
              s_agent_tlsf.count);
}

int agent_tlsf_init(void)
{
    pthread_once(&s_agent_tlsf_once, agent_tlsf_bootstrap);
    return s_agent_tlsf_ready ? 0 : -1;
}

void *agent_malloc(size_t size)
{
    if(agent_tlsf_init() != 0)
    {
        agent_tlsf_log_alloc_failed("agent_malloc init", size);
        return NULL;
    }

    void *ptr = tlsf_thread_malloc(&s_agent_tlsf, size);
    if(!ptr)
    {
        agent_tlsf_log_alloc_failed("agent_malloc", size);
    }
    return ptr;
}

void *agent_calloc(size_t nmemb, size_t size)
{
    if(nmemb != 0U && size > ((size_t) -1) / nmemb)
    {
        AGENT_LOG("agent_calloc overflow: nmemb=%u size=%u",
                  (unsigned)nmemb, (unsigned)size);
        return NULL;
    }

    size_t total = nmemb * size;
    void *ptr = agent_malloc(total);
    if(ptr)
    {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *agent_realloc(void *ptr, size_t size)
{
    if(agent_tlsf_init() != 0)
    {
        agent_tlsf_log_alloc_failed("agent_realloc init", size);
        return NULL;
    }

    void *new_ptr = tlsf_thread_realloc(&s_agent_tlsf, ptr, size);
    if(size != 0U && !new_ptr)
    {
        agent_tlsf_log_alloc_failed("agent_realloc", size);
    }
    return new_ptr;
}

void agent_free(void *ptr)
{
    if(!ptr)
    {
        return;
    }
    if(agent_tlsf_init() != 0)
    {
        return;
    }
    tlsf_thread_free(&s_agent_tlsf, ptr);
}

char *agent_strdup(const char *src)
{
    if(!src)
    {
        return NULL;
    }

    size_t len = strlen(src);
    char *copy = agent_malloc(len + 1U);
    if(!copy)
    {
        return NULL;
    }

    memcpy(copy, src, len + 1U);
    return copy;
}

int agent_tlsf_get_stats(tlsf_stats_t *stats)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }
    return tlsf_thread_stats(&s_agent_tlsf, stats);
}
