#ifndef __MOD_TOOL_CRON_H__
#define __MOD_TOOL_CRON_H__


#include <stddef.h>

/**
 * Add a scheduled cron job.
 * Input JSON: { name, schedule_type ("after"/"every"/"at"), delay_s, interval_s, at_epoch, message, channel?, chat_id? }
 */
int tool_cron_add_execute(const char *input_json, char *output, size_t output_size);

/**
 * List all scheduled cron jobs.
 * Input JSON: {} (no required fields)
 */
int tool_cron_list_execute(const char *input_json, char *output, size_t output_size);

/**
 * Remove a scheduled cron job by ID.
 * Input JSON: { job_id }
 */
int tool_cron_remove_execute(const char *input_json, char *output, size_t output_size);

#endif

