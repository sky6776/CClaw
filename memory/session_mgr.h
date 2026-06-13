#ifndef __MOD_SESSION_MGR_H__
#define __MOD_SESSION_MGR_H__

#include <stddef.h>

/* 前向声明，避免在头文件里引入 cJSON.h */
typedef struct cclaw_cJSON cclaw_cJSON;

/**
 * Initialize session manager.
 */
int session_mgr_init(void);

/**
 * Append a message to a session file (JSONL format).
 * @param channel   消息来源通道（如 "cli"、"weixin"、"telegram"），用于 session 隔离
 * @param chat_id   Session identifier (e.g., "12345")
 * @param role      "user" or "assistant"
 * @param content   Message text
 */
int session_append(const char *channel, const char *chat_id,
                   const char *role, const char *content);

/**
 * 将一条结构化消息追加到 session 文件（JSONL 格式）。
 *
 * 与 session_append() 不同，本接口接受完整的 cJSON 消息对象，
 * 可以保存 reasoning_content、tool_calls、tool_call_id 等结构化字段。
 * 这些字段在 DeepSeek 思考模式 + 工具调用场景下是必需的：
 * 后续请求必须完整回传 assistant 的 reasoning_content 和 tool_calls，
 * 否则 API 返回 400。
 *
 * 持久化的字段包括（白名单）：
 * - role（必需）
 * - content
 * - reasoning_content（DeepSeek 思考模式）
 * - tool_calls（工具调用）
 * - tool_call_id（工具结果）
 * - ts（时间戳，自动添加）
 *
 * @param channel   消息来源通道，用于 session 隔离
 * @param chat_id   Session 标识符
 * @param message   cJSON 消息对象（调用方拥有，本函数会复制需要的内容）
 * @return 0 成功，-1 失败
 */
int session_append_message_json(const char *channel, const char *chat_id,
                                const cclaw_cJSON *message);

/**
 * Load session history as a JSON array string suitable for LLM messages.
 * Returns the last max_msgs messages as:
 * [{"role":"user","content":"..."},{"role":"assistant","content":"..."},...]
 *
 * @param channel   消息来源通道，用于 session 隔离
 * @param chat_id   Session identifier
 * @param buf       Output buffer (caller allocates)
 * @param size      Buffer size
 * @param max_msgs  Maximum number of messages to return
 */
int session_get_history_json(const char *channel, const char *chat_id,
                             char *buf, size_t size, int max_msgs);

/**
 * Clear a session (delete the file).
 * @param channel   消息来源通道，用于 session 隔离
 * @param chat_id   Session identifier
 */
int session_clear(const char *channel, const char *chat_id);

/**
 * 原子地写入 assistant(tool_calls) + tool_result 两条消息到 session 文件。
 *
 * 工具调用场景下，ReAct 循环需要在 session 中持久化两条配对消息：
 *   1. assistant 消息（含 tool_calls 数组）
 *   2. user 消息（含 tool_result 内容）
 *
 * 本函数在同一个文件句柄中写入两条 JSONL 记录，如果第二条写入失败，
 * 通过 ftruncate 将文件截断回写入前的位置，确保不会留下半条工具调用链。
 *
 * @param channel       消息来源通道
 * @param chat_id       Session 标识符
 * @param asst_msg      assistant 消息（含 tool_calls）
 * @param tool_result   user 消息（含 tool_result）
 * @return 0 两条都写入成功，-1 任一失败（文件已回滚或回滚失败）
 */
int session_append_tool_chain(const char *channel, const char *chat_id,
                              const cclaw_cJSON *asst_msg,
                              const cclaw_cJSON *tool_result);

/**
 * List all session files (prints to log).
 */
void session_list(void);

#endif

