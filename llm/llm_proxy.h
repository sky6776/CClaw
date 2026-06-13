#ifndef __MOD_LLM_PROXY_H__
#define __MOD_LLM_PROXY_H__

#include <stddef.h>
#include <stdbool.h>

#include "agent_conf.h"

typedef struct cclaw_cJSON cclaw_cJSON;

int llm_proxy_init(void);

/* ── Tool Use Support ──────────────────────────────────────────── */

typedef struct
{
    char id[64];        /* "toolu_xxx" */
    char name[32];      /* "web_search" */
    char *input;        /* heap-allocated JSON string */
    size_t input_len;
} llm_tool_call_t;

typedef struct
{
    char *text;                                  /* 累积的普通文本内容 */
    size_t text_len;
    char *reasoning;                             /* DeepSeek 思考模式的推理内容 */
    size_t reasoning_len;
    llm_tool_call_t calls[AGENT_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;                               /* stop_reason == "tool_use" */
    bool stream_had_output;                      /* 流式回调是否已向 CLI 输出过内容 */
} llm_response_t;

void llm_response_free(llm_response_t *resp);

/**
 * Send a chat completion request with tools to the configured LLM API (non-streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages       Private cclaw_cJSON array of messages (caller owns)
 * @param tools_json     Pre-built JSON string of tools array, or NULL for no tools
 * @param resp           Output: structured response with text and tool calls
 * @return ESP_OK on success
 */
int llm_chat_tools(const char *system_prompt,
                   cclaw_cJSON *messages,
                   const char *tools_json,
                   llm_response_t *resp);

/* ── 流式输出支持 ──────────────────────────────────────────────── *
 *
 * llm_chat_stream() 是 llm_chat_tools() 的流式版本。
 * 它向 LLM API 发送 "stream": true 请求，然后通过 SSE (Server-Sent Events)
 * 逐个接收 token，每收到一段文本增量就调用 on_chunk 回调。
 *
 * 与 llm_chat_tools() 的区别：
 * - llm_chat_tools：等待整个响应完成，一次性返回完整文本
 * - llm_chat_stream：边收边回调，调用者可以实时显示文本
 *
 * 流式结束后，resp 中仍然包含完整的累积文本和工具调用信息，
 * 因此 session 保存、outbound 推送等下游逻辑无需修改。
 *
 * 典型用法：
 *   llm_response_t resp;
 *   llm_chat_stream(prompt, msgs, tools, my_chunk_cb, my_ctx, &resp);
 *   // resp.text 包含完整文本，可以直接保存/推送
 */

/**
 * 流式文本增量回调。
 *
 * 每当从 SSE 事件中解析出一段新的文本 token 时调用。
 * 回调函数应该尽快返回，不要做耗时操作。
 *
 * @param text_delta  本次收到的文本片段（不是 NUL 结尾，用 delta_len 判断长度）
 * @param delta_len   文本片段的字节长度
 * @param userp       调用者传入的上下文指针
 * @return 0 继续流式读取，非 0 中断流式读取（如用户取消）
 */
typedef int (*llm_stream_chunk_cb)(const char *text_delta,
                                   size_t delta_len,
                                   void *userp);

/**
 * 流式版本的 LLM chat 调用。
 *
 * 内部流程：
 * 1. 构建带 "stream": true 的请求 JSON
 * 2. 通过 HTTP 流式 API 建立 SSE 连接
 * 3. 逐行解析 SSE 事件（data: {...}）
 * 4. 每解析出 delta.content 就调用 on_chunk
 * 5. 累积完整文本到 resp->text
 * 6. 如果有工具调用，累积到 resp->calls[]
 *
 * @param system_prompt  系统提示词
 * @param messages       消息历史（cJSON 数组，调用方拥有）
 * @param tools_json     工具定义 JSON 字符串，NULL 表示不使用工具
 * @param on_chunk       文本增量回调，NULL 表示不回调（仅累积）
 * @param userp          传递给 on_chunk 的上下文
 * @param resp           [out] 响应结果（包含完整累积文本和工具调用）
 * @return 0 成功，-1 失败
 */
int llm_chat_stream(const char *system_prompt,
                    cclaw_cJSON *messages,
                    const char *tools_json,
                    llm_stream_chunk_cb on_chunk,
                    void *userp,
                    llm_response_t *resp);

#endif

