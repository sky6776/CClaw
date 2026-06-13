#include "cron/cron_service.h"
#include "agent_conf.h"
#include "agent_tlsf.h"
#include "agent_fs.h"
#include "bus/message_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include "cJSON/cJSON.h"


#define MAX_CRON_JOBS  CRON_MAX_JOBS

static cron_job_t s_jobs[MAX_CRON_JOBS];
static int s_job_count = 0;

static int cron_save_jobs(void);

static bool cron_sanitize_destination(cron_job_t *job)
{
    bool changed = false;
    if(!job)
    {
        return false;
    }

    if(job->channel[0] == '\0')
    {
        strncpy(job->channel, CHANNEL_SYSTEM, sizeof(job->channel) - 1);
        changed = true;
    }

    if(strcmp(job->channel, CHANNEL_TELEGRAM) == 0 ||
            strcmp(job->channel, CHANNEL_WEIXIN) == 0)
    {
        if(job->chat_id[0] == '\0' || strcmp(job->chat_id, "cron") == 0)
        {
            AGENT_LOG("Cron job %s has invalid %s chat_id, fallback to system:cron",
                      job->id[0] ? job->id : "<new>",
                      job->channel);
            strncpy(job->channel, CHANNEL_SYSTEM, sizeof(job->channel) - 1);
            strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
            changed = true;
        }
    }
    else if(strcmp(job->channel, CHANNEL_CLI) == 0)
    {
        if(job->chat_id[0] == '\0' || strcmp(job->chat_id, "cron") == 0)
        {
            strncpy(job->chat_id, "cli", sizeof(job->chat_id) - 1);
            changed = true;
        }
    }
    else if(job->chat_id[0] == '\0')
    {
        strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
        changed = true;
    }

    return changed;
}

/* ── Persistence ──────────────────────────────────────────────── */

static void cron_generate_id(char *id_buf)
{
    uint32_t r = rand();
    snprintf(id_buf, 9, "%08x", (unsigned int)r);
}

static int cron_load_jobs(void)
{
    FILE *f = fopen(CRON_FILE, "r");
    if(!f)
    {
        AGENT_LOG("No cron file found, starting fresh");
        s_job_count = 0;
        return 0;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if(fsize <= 0 || fsize > 8192)
    {
        AGENT_LOG("Cron file invalid size: %ld", fsize);
        fclose(f);
        s_job_count = 0;
        return 0;
    }

    char *buf = agent_malloc(fsize + 1);
    if(!buf)
    {
        fclose(f);
        return -1;
    }

    size_t n = fread(buf, 1, fsize, f);
    buf[n] = '\0';
    fclose(f);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(buf);
    agent_free(buf);

    if(!root)
    {
        AGENT_LOG("Failed to parse cron JSON");
        s_job_count = 0;
        return 0;
    }

    cJSON *jobs_arr = cJSON_GetObjectItem(root, "jobs");
    if(!jobs_arr || !cJSON_IsArray(jobs_arr))
    {
        cJSON_Delete(root);
        s_job_count = 0;
        return 0;
    }

    s_job_count = 0;
    bool repaired = false;
    cJSON *item;
    cJSON_ArrayForEach(item, jobs_arr)
    {
        if(s_job_count >= MAX_CRON_JOBS)
        {
            break;
        }

        cron_job_t *job = &s_jobs[s_job_count];
        memset(job, 0, sizeof(cron_job_t));

        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        const char *kind_str = cJSON_GetStringValue(cJSON_GetObjectItem(item, "kind"));
        const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(item, "message"));
        const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(item, "channel"));
        const char *chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "chat_id"));

        if(!id || !name || !kind_str || !message)
        {
            continue;
        }

        strncpy(job->id, id, sizeof(job->id) - 1);
        strncpy(job->name, name, sizeof(job->name) - 1);
        strncpy(job->message, message, sizeof(job->message) - 1);
        strncpy(job->channel, channel ? channel : CHANNEL_SYSTEM,
                sizeof(job->channel) - 1);
        strncpy(job->chat_id, chat_id ? chat_id : "cron",
                sizeof(job->chat_id) - 1);
        if(cron_sanitize_destination(job))
        {
            repaired = true;
        }

        cJSON *enabled_j = cJSON_GetObjectItem(item, "enabled");
        job->enabled = enabled_j ? cJSON_IsTrue(enabled_j) : true;

        cJSON *delete_j = cJSON_GetObjectItem(item, "delete_after_run");
        job->delete_after_run = delete_j ? cJSON_IsTrue(delete_j) : false;

        if(strcmp(kind_str, "every") == 0)
        {
            job->kind = CRON_KIND_EVERY;
            cJSON *interval = cJSON_GetObjectItem(item, "interval_s");
            job->interval_s = (interval && cJSON_IsNumber(interval))
                              ? (uint32_t)interval->valuedouble : 0;
        }
        else if(strcmp(kind_str, "at") == 0)
        {
            job->kind = CRON_KIND_AT;
            cJSON *at_epoch = cJSON_GetObjectItem(item, "at_epoch");
            job->at_epoch = (at_epoch && cJSON_IsNumber(at_epoch))
                            ? (int64_t)at_epoch->valuedouble : 0;
        }
        else
        {
            continue; /* Unknown kind, skip */
        }

        cJSON *last_run = cJSON_GetObjectItem(item, "last_run");
        job->last_run = (last_run && cJSON_IsNumber(last_run))
                        ? (int64_t)last_run->valuedouble : 0;

        cJSON *next_run = cJSON_GetObjectItem(item, "next_run");
        job->next_run = (next_run && cJSON_IsNumber(next_run))
                        ? (int64_t)next_run->valuedouble : 0;

        s_job_count++;
    }

    cJSON_Delete(root);
    if(repaired)
    {
        cron_save_jobs();
    }
    AGENT_LOG("Loaded %d cron jobs", s_job_count);
    return 0;
}

static int cron_save_jobs(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *jobs_arr = cJSON_CreateArray();

    for(int i = 0; i < s_job_count; i++)
    {
        cron_job_t *job = &s_jobs[i];
        cJSON *item = cJSON_CreateObject();

        cJSON_AddStringToObject(item, "id", job->id);
        cJSON_AddStringToObject(item, "name", job->name);
        cJSON_AddBoolToObject(item, "enabled", job->enabled);
        cJSON_AddStringToObject(item, "kind",
                                job->kind == CRON_KIND_EVERY ? "every" : "at");

        if(job->kind == CRON_KIND_EVERY)
        {
            cJSON_AddNumberToObject(item, "interval_s", job->interval_s);
        }
        else
        {
            cJSON_AddNumberToObject(item, "at_epoch", (double)job->at_epoch);
        }

        cJSON_AddStringToObject(item, "message", job->message);
        cJSON_AddStringToObject(item, "channel", job->channel);
        cJSON_AddStringToObject(item, "chat_id", job->chat_id);
        cJSON_AddNumberToObject(item, "last_run", (double)job->last_run);
        cJSON_AddNumberToObject(item, "next_run", (double)job->next_run);
        cJSON_AddBoolToObject(item, "delete_after_run", job->delete_after_run);

        cJSON_AddItemToArray(jobs_arr, item);
    }

    cJSON_AddItemToObject(root, "jobs", jobs_arr);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if(!json_str)
    {
        AGENT_LOG("Failed to serialize cron jobs");
        return -1;
    }

    if(agent_fs_ensure_parent_dir(CRON_FILE) != 0)
    {
        agent_free(json_str);
        return -1;
    }

    FILE *f = fopen(CRON_FILE, "w");
    if(!f)
    {
        AGENT_LOG("Failed to open %s for writing", CRON_FILE);
        agent_free(json_str);
        return -1;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    agent_free(json_str);

    if(written != len)
    {
        AGENT_LOG("Cron save incomplete: %d/%d bytes", (int)written, (int)len);
        return -1;
    }

    AGENT_LOG("Saved %d cron jobs to %s", s_job_count, CRON_FILE);
    return 0;
}

/* ── Due-job processing ───────────────────────────────────────── */

static void cron_process_due_jobs(void)
{
    time_t now = time(NULL);

    bool changed = false;

    for(int i = 0; i < s_job_count; i++)
    {
        cron_job_t *job = &s_jobs[i];
        if(!job->enabled)
        {
            continue;
        }
        if(job->next_run <= 0)
        {
            continue;
        }
        if(job->next_run > now)
        {
            continue;
        }

        /* Job is due — fire it */
        AGENT_LOG("Cron job firing: %s (%s)", job->name, job->id);

        /*
         * 定时任务是 inbound 的一个生产者。bus_msg_set 只建立视图，
         * 真正的正文复制发生在 message_bus_push_inbound() 内部。
         */
        char task_msg[768];
        snprintf(task_msg, sizeof(task_msg),
                 AGENT_CRON_TASK_PREFIX "\n"
                 "Job name: %s\n"
                 "Job message: %s\n"
                 "Instruction: This message was generated by the local cron scheduler. "
                 "Treat the job message as the task to perform for the user. "
                 "If it is a simple reminder, reply directly with a short friendly reminder. "
                 "If it asks for tool-backed work, use the appropriate tools and then send the result. "
                 "Do not inspect, list, add, remove, or discuss cron jobs unless the job message explicitly asks for schedule management.",
                 job->name, job->message);

        bus_msg_t msg;
        bus_msg_set(&msg, job->channel, job->chat_id, task_msg);
        int err = message_bus_push_inbound(&msg);
        if(err != 0)
        {
            AGENT_LOG("Failed to push cron message: %d", err);
        }

        /* Update state */
        job->last_run = now;

        if(job->kind == CRON_KIND_AT)
        {
            /* One-shot: disable or delete */
            if(job->delete_after_run)
            {
                /* Remove by shifting array */
                AGENT_LOG("Deleting one-shot job: %s", job->name);
                for(int j = i; j < s_job_count - 1; j++)
                {
                    s_jobs[j] = s_jobs[j + 1];
                }
                s_job_count--;
                i--; /* Re-check this index */
            }
            else
            {
                job->enabled = false;
                job->next_run = 0;
            }
        }
        else
        {
            /* Recurring: compute next run */
            job->next_run = now + job->interval_s;
        }

        changed = true;
    }

    if(changed)
    {
        cron_save_jobs();
    }
}

static void *cron_task_main(void *arg)
{
    (void)arg;

    while(1)
    {
        sleep(CRON_CHECK_INTERVAL_SEC);
        cron_process_due_jobs();
    }

    return NULL;
}

/* ── Compute initial next_run for a new job ───────────────────── */

static void compute_initial_next_run(cron_job_t *job)
{
    time_t now = time(NULL);

    if(job->kind == CRON_KIND_EVERY)
    {
        job->next_run = now + job->interval_s;
    }
    else if(job->kind == CRON_KIND_AT)
    {
        if(job->at_epoch > now)
        {
            job->next_run = job->at_epoch;
        }
        else
        {
            /* Already in the past */
            job->next_run = 0;
            job->enabled = false;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────── */

int cron_service_init(void)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }
    agent_fs_ensure_file(CRON_FILE, "{\"jobs\":[]}\n");
    return cron_load_jobs();
}

int cron_service_start(void)
{
    /* Recompute next_run for all enabled jobs that don't have one */
    time_t now = time(NULL);
    for(int i = 0; i < s_job_count; i++)
    {
        cron_job_t *job = &s_jobs[i];
        if(job->enabled && job->next_run <= 0)
        {
            if(job->kind == CRON_KIND_EVERY)
            {
                job->next_run = now + job->interval_s;
            }
            else if(job->kind == CRON_KIND_AT && job->at_epoch > now)
            {
                job->next_run = job->at_epoch;
            }
        }
    }

    static pthread_t s_cron_task = 0;
    if(0 == s_cron_task)
    {
        if(0 != pthread_create(&s_cron_task, NULL, cron_task_main, NULL))
        {
            AGENT_LOG("fail to create cron_task_main");
            s_cron_task = 0;
            return -1;
        }
    }

    AGENT_LOG("Cron service started (%d jobs, check every %ds)",
              s_job_count, CRON_CHECK_INTERVAL_SEC);
    return 0;
}

int cron_add_job(cron_job_t *job)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }

    if(s_job_count >= MAX_CRON_JOBS)
    {
        AGENT_LOG("Max cron jobs reached (%d)", MAX_CRON_JOBS);
        return -1;
    }

    /* Generate ID */
    cron_generate_id(job->id);

    /* Validate/sanitize channel and chat_id before storing. */
    cron_sanitize_destination(job);

    /* Compute initial next_run */
    job->enabled = true;
    job->last_run = 0;
    compute_initial_next_run(job);

    /* Copy into static array */
    s_jobs[s_job_count] = *job;
    s_job_count++;

    cron_save_jobs();

    AGENT_LOG("Added cron job: %s (%s) kind=%s next_run=%lld",
              job->name, job->id,
              job->kind == CRON_KIND_EVERY ? "every" : "at",
              (long long)job->next_run);
    return 0;
}

int cron_remove_job(const char *job_id)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }

    for(int i = 0; i < s_job_count; i++)
    {
        if(strcmp(s_jobs[i].id, job_id) == 0)
        {
            AGENT_LOG("Removing cron job: %s (%s)", s_jobs[i].name, job_id);

            /* Shift remaining jobs down */
            for(int j = i; j < s_job_count - 1; j++)
            {
                s_jobs[j] = s_jobs[j + 1];
            }
            s_job_count--;

            cron_save_jobs();
            return 0;
        }
    }

    AGENT_LOG("Cron job not found: %s", job_id);
    return -1;
}

void cron_list_jobs(const cron_job_t **jobs, int *count)
{
    *jobs = s_jobs;
    *count = s_job_count;
}

