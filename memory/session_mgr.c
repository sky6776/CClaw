#define _POSIX_C_SOURCE 200809L

#include "session_mgr.h"
#include "agent_conf.h"
#include "agent_tlsf.h"
#include "agent_fs.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>   /* fileno(), ftruncate() */
#include "cJSON/cJSON.h"

/**
 * 十六进制字符查找表，用于百分号编码。
 * percent_encode_char() 使用此表将 4 位 nibble 转为 '0'-'9'/'a'-'f'。
 */
static const char HEX_CHARS[] = "0123456789abcdef";

/*
 * 结构化 session 消息的字段复制 helper 会被多个函数复用。
 *
 * session_append_tool_chain() 位于本文件前半段，而 helper 的实现位于后面。
 * 如果这里没有 static 前置声明，C 编译器会在第一次调用时生成隐式的
 * 非 static 声明；随后遇到真正的 static 定义时就会报
 * "static declaration follows non-static declaration"。这里显式声明，
 * 既保持 helper 只在本文件内可见，也避免不同编译器/标准下的隐式声明问题。
 */
static void copy_string_field(cJSON *dst, const cJSON *src, const char *field_name);
static void copy_deep_field(cJSON *dst, const cJSON *src, const char *field_name);

/*
 * 将打开的 session 文件回滚到工具链写入前的位置。
 *
 * 为什么回滚前还要 fflush？
 * session_append_tool_chain() 会先写 assistant(tool_calls)，再写 tool_result。
 * 如果第二条消息构造失败，需要把第一条也撤回，避免历史里留下
 * "有 tool_calls 但没有 tool_result" 的半条工具链。
 *
 * 但 FILE* 默认带 stdio 缓冲：fprintf 成功只代表数据进入了用户态缓冲，
 * 未必已经写到文件描述符。若直接 ftruncate，然后 fclose 再把缓冲刷出去，
 * 截断后的文件仍可能重新出现刚才那条 assistant 记录。这里先 fflush，
 * 让 stdio 缓冲与底层 fd 对齐，再 ftruncate 到 pos_before。
 *
 * 注意：即使 fflush 失败，也仍然尝试 ftruncate。磁盘满等场景下，部分字节
 * 可能已经落盘，截断仍有机会把文件恢复到一致状态。函数本身只负责尽力回滚，
 * 调用方仍会返回失败，让上层不要继续基于不确定的 session 状态推进。
 */
static void session_rollback_file(FILE *f, long pos_before, const char *reason)
{
    if(!f)
    {
        return;
    }

    if(reason)
    {
        AGENT_LOG("Session: rollback tool chain because %s", reason);
    }

    if(pos_before < 0)
    {
        AGENT_LOG("Session: rollback skipped because ftell failed");
        return;
    }

    if(fflush(f) != 0)
    {
        AGENT_LOG("Session: fflush before rollback failed, still try ftruncate");
    }

    if(ftruncate(fileno(f), pos_before) != 0)
    {
        AGENT_LOG("Session: ftruncate rollback failed at pos %ld", pos_before);
    }
}

/**
 * 对 chat_id 做百分号编码（percent-encoding），生成安全的一一映射文件名。
 *
 * 为什么不能用下划线替换？
 *
 * 旧实现把所有非安全字符统一替换为 '_'，这是多对一的有损映射：
 *   "a/b" → "a_b"
 *   "a?b" → "a_b"
 *   "a_b" → "a_b"  （原始值）
 * 三个不同的 chat_id 映射到同一个 session 文件，导致会话串档。
 * 如果 chat_id 来自外部渠道（如 Telegram 群组 ID、微信用户 ID），
 * 串档会造成用户 A 看到用户 B 的对话历史。
 *
 * 百分号编码策略：
 * - 安全字符（字母、数字、下划线、连字符）原样保留
 * - 其他字符编码为 %XX（XX 为两位十六进制）
 * - 编码是一一映射：不同输入必定产生不同输出
 *
 * 编码示例：
 *   "a/b"   → "a%2Fb"
 *   "a?b"   → "a%3Fb"
 *   "a_b"   → "a_b"    （下划线是安全字符，原样保留）
 *   "-100"  → "-100"   （连字符和数字是安全字符）
 *
 * @param chat_id   原始 chat_id
 * @param buf       输出缓冲区
 * @param size      缓冲区大小（建议 >= 256，因为每个字符最多展开为 3 字节）
 */
static void session_sanitize_chat_id(const char *chat_id, char *buf, size_t size)
{
    if(!chat_id || !buf || size == 0)
    {
        if(buf && size > 0)
        {
            buf[0] = '\0';
        }
        return;
    }

    size_t src_len = strlen(chat_id);
    size_t out = 0;

    for(size_t i = 0; i < src_len; i++)
    {
        char c = chat_id[i];

        /* 安全字符集：字母、数字、下划线、连字符 → 原样保留 */
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-')
        {
            if(out + 1 >= size)
            {
                break;  /* 缓冲区不足，停止编码 */
            }
            buf[out++] = c;
        }
        else
        {
            /* 非安全字符 → 百分号编码 %XX
             * 每个字符展开为 3 字节（'%' + 两个十六进制数字），
             * 需要确保缓冲区有足够空间。 */
            if(out + 3 >= size)
            {
                break;  /* 缓冲区不足，停止编码 */
            }
            buf[out++] = '%';
            buf[out++] = HEX_CHARS[(unsigned char)c >> 4];     /* 高 4 位 */
            buf[out++] = HEX_CHARS[(unsigned char)c & 0x0F];   /* 低 4 位 */
        }
    }
    buf[out] = '\0';

    /* 如果编码后的结果被缓冲区截断，记录日志。
     * 随着 safe_id 缓冲增大到 320 字节（见 session_path），
     * 正常 chat_id（<=96 字节，最坏展开为 288 字节）不会触发此截断。 */
    if(out > 0 && out + 1 >= size)
    {
        AGENT_LOG("Session: percent-encoded chat_id truncated at %lu bytes",
                  (unsigned long)out);
    }
}

/**
 * 根据 channel + chat_id 生成 session 文件路径。
 *
 * 格式: <AGENT_SESSION_DIR>/<safe_channel>_<safe_id>.jsonl
 * 例如: "agent/sessions/cli_console.jsonl"
 *       "agent/sessions/weixin_user123.jsonl"
 *       "agent/sessions/telegram_-1001234567890.jsonl"
 *
 * channel 隔离的必要性：
 *
 * 旧实现只以 chat_id 为 session key，不同 channel（CLI、微信、Telegram）
 * 只要 chat_id 相同就会共用同一个 session 文件。例如：
 *   CLI channel:     chat_id="console" → agent/sessions/tg_console.jsonl
 *   微信 channel:    chat_id="console" → agent/sessions/tg_console.jsonl（同一文件！）
 *   Telegram channel: chat_id="console" → agent/sessions/tg_console.jsonl（同一文件！）
 *
 * 虽然实际上各 channel 的 chat_id 命名方式不同（CLI 用 "console"、
 * Telegram 用数字 ID、微信用用户 ID），但无法保证未来不会出现碰撞。
 * 将 channel 编码进文件名可以从根本上杜绝跨 channel 串上下文的问题。
 *
 * @param channel   消息来源通道（如 "cli"、"weixin"、"telegram"）
 * @param chat_id   原始 chat_id
 * @param buf       输出路径缓冲区
 * @param size      缓冲区大小（建议 >= 384）
 * @return 0 成功，-1 路径截断（buf 被置空，调用方 fopen 会失败）
 */
static int session_path(const char *channel, const char *chat_id,
                        char *buf, size_t size)
{
    /*
     * safe_channel / safe_id 缓冲区：对 channel 和 chat_id 做安全编码。
     *
     * 百分号编码后每个非安全字符展开为 3 字节：
     *   MESSAGE_BUS_CHAT_ID_MAX = 96，最坏情况 96 * 3 = 288 字节。
     *   320 > 288，确保任何合法 chat_id 编码后不会被截断。
     *
     * channel 最大长度为 MESSAGE_BUS_CHANNEL_MAX = 16，编码后最多 48 字节，
     * 64 字节缓冲区足够。
     */
    char safe_channel[64];
    char safe_id[320];
    session_sanitize_chat_id(channel, safe_channel, sizeof(safe_channel));
    session_sanitize_chat_id(chat_id, safe_id, sizeof(safe_id));

    int n = snprintf(buf, size, "%s/%s_%s.jsonl",
                     AGENT_SESSION_DIR, safe_channel, safe_id);

    /*
     * 检测 snprintf 截断：
     *
     * snprintf 返回值 n 是"如果不截断，应该写入的字符数"（不含 '\0'）。
     * 如果 n >= size，说明输出被截断了。截断后的路径可能：
     * 1. 文件名碰撞：不同 chat_id 写到同一个截断文件
     * 2. 文件名不完整：后缀 ".jsonl" 被切断，文件无法被正确识别
     *
     * 处理策略：将 buf 置为空字符串（buf[0]='\0'）。
     * 所有调用方在 fopen 前检查路径有效性（空路径 fopen 会失败返回 NULL），
     * 并记录日志帮助排查。
     *
     * 注意：随着 safe_id 增大到 128、调用方 path 缓冲增大到 256，
     * 此截断场景在实际使用中极难触发，但保留防御性检查。
     */
    if(n < 0 || (size_t)n >= size)
    {
        AGENT_LOG("Session: path truncated (need %d, have %lu) for chat_id: %s",
                  n, (unsigned long)size, chat_id);
        buf[0] = '\0';
        return -1;
    }

    return 0;
}

int session_mgr_init(void)
{
    agent_fs_ensure_dir(AGENT_SESSION_DIR);
    AGENT_LOG("Session manager initialized at %s", AGENT_SESSION_DIR);
    return 0;
}

int session_append(const char *channel, const char *chat_id,
                   const char *role, const char *content)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }

    /* 路径缓冲区 384 字节：足够容纳 AGENT_SESSION_DIR(14) + "/"(1) +
     * safe_channel(最多48) + "_"(1) + safe_id(最多288) + ".jsonl"(6) + '\0'(1) = 359。
     * 旧 path[256] 在 channel 隔离后可能不够，升级到 384。 */
    char path[384];
    if(session_path(channel, chat_id, path, sizeof(path)) != 0)
    {
        return -1;
    }

    if(agent_fs_ensure_parent_dir(path) != 0)
    {
        return -1;
    }

    FILE *f = fopen(path, "a");
    if(!f)
    {
        AGENT_LOG("Cannot open session file %s", path);
        return -1;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    /*
     * 写入失败检测（与 session_append_message_json 保持一致）。
     *
     * 旧实现只检查 line 是否为 NULL，但即使 line 非空，
     * fprintf 也可能因磁盘满等原因写入失败，
     * fclose 同样可能失败（延迟写入错误）。
     *
     * 如果写入失败但返回成功（0），session 文件会出现：
     * 1. 空行或截断的 JSON → session_get_history_json 解析失败
     * 2. 漏写 user 或 assistant 消息 → 历史对话不连续
     * 3. 工具调用场景中漏写 user 消息 → 上下文断裂
     *
     * 检测逻辑：
     * - line == NULL → cJSON 内存分配失败
     * - fprintf 返回值 < 预期写入字节数 → 磁盘满或 I/O 错误
     * - fclose 返回非 0 → 延迟写入错误
     */
    if(!line)
    {
        AGENT_LOG("Session: plain append cJSON_PrintUnformatted returned NULL (OOM?)");
        fclose(f);
        return -1;
    }

    size_t line_len = strlen(line);
    int written = fprintf(f, "%s\n", line);
    agent_free(line);

    if(written < 0 || (size_t)written < line_len + 1)
    {
        AGENT_LOG("Session: plain append fprintf wrote %d bytes, expected %lu (disk full?)",
                  written, (unsigned long)(line_len + 1));
        fclose(f);
        return -1;
    }

    if(fclose(f) != 0)
    {
        AGENT_LOG("Session: plain append fclose failed (delayed write error)");
        return -1;
    }

    return 0;
}

/**
 * 原子地写入 assistant(tool_calls) + tool_result 两条消息到 session 文件。
 *
 * 为什么需要原子写入？
 *
 * 工具调用场景下，ReAct 循环需要在 session 中持久化两条配对消息：
 *   1. assistant 消息（含 tool_calls 数组）
 *   2. user 消息（含 tool_result 内容）
 *
 * 如果用两次独立的 session_append_message_json() 调用，可能出现：
 * - 第一条成功、第二条失败 → session 中有 assistant(tool_calls) 但无 tool_result
 * - 第一条失败、第二条成功 → session 中有孤儿 tool_result 但无对应的 tool_calls
 *
 * DeepSeek 思考模式 + 工具调用要求：assistant 消息必须包含 reasoning_content
 * 和 tool_calls，下一条 user 消息必须包含对应的 tool_result。任何一环缺失
 * 都会导致后续 API 请求因上下文不完整而返回 400。
 *
 * 原子性保证：
 * 本函数在同一个文件句柄中写入两条 JSONL 记录。如果第二条写入失败，
 * 通过 ftruncate 将文件截断回写入前的位置，确保不会留下半条工具调用链。
 * 如果截断也失败（极端情况），至少 fclose 返回错误让调用方知道。
 *
 * @param channel       消息来源通道
 * @param chat_id       Session 标识符
 * @param asst_msg      assistant 消息（含 tool_calls）
 * @param tool_result   user 消息（含 tool_result）
 * @return 0 两条都写入成功，-1 任一失败（文件已回滚或回滚失败）
 */
int session_append_tool_chain(const char *channel, const char *chat_id,
                              const cclaw_cJSON *asst_msg,
                              const cclaw_cJSON *tool_result)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }

    if(!asst_msg || !tool_result)
    {
        return -1;
    }

    char path[384];
    if(session_path(channel, chat_id, path, sizeof(path)) != 0)
    {
        return -1;
    }

    if(agent_fs_ensure_parent_dir(path) != 0)
    {
        return -1;
    }

    FILE *f = fopen(path, "a");
    if(!f)
    {
        AGENT_LOG("Cannot open session file %s for atomic tool chain append", path);
        return -1;
    }

    /*
     * 关闭 stdio 层缓冲，让 fprintf 直接落到底层 fd。
     *
     * 工具链写入需要支持失败回滚：一旦第二条 tool_result 失败，就要用
     * ftruncate 回到写入前的位置。无缓冲模式可以减少 FILE* 缓冲和 fd 文件
     * 长度之间的状态差，避免 "先 ftruncate，后 fclose 又把旧缓冲写回去"。
     *
     * 下方 rollback helper 仍会在截断前调用 fflush，作为防御性兜底；
     * 但这里先设为 _IONBF，能让正常路径和错误路径都更接近底层文件状态。
     */
    if(setvbuf(f, NULL, _IONBF, 0) != 0)
    {
        AGENT_LOG("Session: failed to disable stdio buffering for tool chain append");
        fclose(f);
        return -1;
    }

    /*
     * 记录写入前的文件位置，用于失败时截断回滚。
     *
     * ftell 在 append 模式下返回当前文件末尾位置，
     * 即本次写入的起始偏移量。如果第二条消息写入失败，
     * 用 ftruncate 把文件截断到此位置，效果等同于
     * 两条消息都没写过。
     */
    long pos_before = ftell(f);

    /* ── 写入第一条：assistant(tool_calls) ────────────────── */
    cJSON *obj1 = cJSON_CreateObject();
    if(!obj1)
    {
        fclose(f);
        return -1;
    }

    copy_string_field(obj1, asst_msg, "role");
    copy_deep_field(obj1, asst_msg, "content");
    copy_string_field(obj1, asst_msg, "reasoning_content");
    copy_deep_field(obj1, asst_msg, "tool_calls");
    copy_string_field(obj1, asst_msg, "tool_call_id");
    cJSON_AddNumberToObject(obj1, "ts", (double)time(NULL));

    char *line1 = cJSON_PrintUnformatted(obj1);
    cJSON_Delete(obj1);

    if(!line1)
    {
        AGENT_LOG("Session: tool chain line1 cJSON_Print returned NULL");
        fclose(f);
        return -1;
    }

    size_t line1_len = strlen(line1);
    int w1 = fprintf(f, "%s\n", line1);
    agent_free(line1);

    if(w1 < 0 || (size_t)w1 < line1_len + 1)
    {
        AGENT_LOG("Session: tool chain line1 fprintf failed (wrote %d, expected %lu)",
                  w1, (unsigned long)(line1_len + 1));
        session_rollback_file(f, pos_before, "line1 fprintf failed");
        fclose(f);
        return -1;
    }

    /* ── 写入第二条：user(tool_result) ────────────────────── */
    cJSON *obj2 = cJSON_CreateObject();
    if(!obj2)
    {
        /* 第一条已写入，需要回滚 */
        AGENT_LOG("Session: tool chain line2 cJSON_CreateObject failed, rolling back");
        session_rollback_file(f, pos_before, "line2 object allocation failed");
        fclose(f);
        return -1;
    }

    copy_string_field(obj2, tool_result, "role");
    copy_deep_field(obj2, tool_result, "content");
    copy_string_field(obj2, tool_result, "reasoning_content");
    copy_deep_field(obj2, tool_result, "tool_calls");
    copy_string_field(obj2, tool_result, "tool_call_id");
    cJSON_AddNumberToObject(obj2, "ts", (double)time(NULL));

    char *line2 = cJSON_PrintUnformatted(obj2);
    cJSON_Delete(obj2);

    if(!line2)
    {
        AGENT_LOG("Session: tool chain line2 cJSON_Print returned NULL, rolling back");
        session_rollback_file(f, pos_before, "line2 JSON serialization failed");
        fclose(f);
        return -1;
    }

    size_t line2_len = strlen(line2);
    int w2 = fprintf(f, "%s\n", line2);
    agent_free(line2);

    if(w2 < 0 || (size_t)w2 < line2_len + 1)
    {
        AGENT_LOG("Session: tool chain line2 fprintf failed (wrote %d, expected %lu), rolling back",
                  w2, (unsigned long)(line2_len + 1));
        session_rollback_file(f, pos_before, "line2 fprintf failed");
        fclose(f);
        return -1;
    }

    /* ── 刷盘并检查写入错误，失败时在关闭前回滚 ──────────
     *
     * 直接 fclose 的问题：
     * fclose 内部会先 fflush 再关闭 fd。如果 fflush 过程中发现
     * 延迟写入错误（如磁盘满），fclose 返回 EOF，但 fd 已经关闭，
     * 此时无法再调用 ftruncate 回滚已写入的数据。
     * 结果：session 文件中可能留下不完整的工具链（assistant 写了但
     * tool_result 没刷完，或两条都部分写入）。
     *
     * 修复策略：先显式 fflush 并检查错误。如果 fflush 失败，
     * 此时 fd 仍然打开，可以 ftruncate 回滚到 pos_before，
     * 然后再 fclose（关闭一个已经 truncate 过的文件是安全的）。
     */
    if(fflush(f) != 0)
    {
        AGENT_LOG("Session: tool chain fflush failed (disk full?), rolling back");
        session_rollback_file(f, pos_before, "final fflush failed");
        fclose(f);
        return -1;
    }

    if(fclose(f) != 0)
    {
        AGENT_LOG("Session: tool chain fclose failed after successful fflush");
        /* fflush 成功但 fclose 失败：极端情况（如 fd 关闭本身出错）。
         * 数据应该已刷盘（fflush 成功），但无法截断已关闭的文件。
         * 返回 -1 让调用方知道，但数据大概率是完整的。 */
        return -1;
    }

    return 0;
}

/**
 * 将 cJSON 值复制为字符串，用于白名单字段复制。
 *
 * 如果源字段存在且是字符串类型，复制其值到目标对象。
 * 用于 session_append_message_json() 和 session_get_history_json()
 * 中的白名单字段复制逻辑。
 */
static void copy_string_field(cJSON *dst, const cJSON *src, const char *field_name)
{
    cJSON *item = cJSON_GetObjectItem((cJSON *)src, field_name);
    if(item && cJSON_IsString(item) && item->valuestring)
    {
        cJSON_AddStringToObject(dst, field_name, item->valuestring);
    }
}

/**
 * 将 cJSON 值复制为完整子对象（用于 tool_calls 等嵌套结构）。
 *
 * 如果源字段存在，深拷贝到目标对象。支持字符串、数组、对象等任意类型。
 */
static void copy_deep_field(cJSON *dst, const cJSON *src, const char *field_name)
{
    cJSON *item = cJSON_GetObjectItem((cJSON *)src, field_name);
    if(item)
    {
        cJSON *copy = cJSON_Duplicate(item, 1);
        if(copy)
        {
            cJSON_AddItemToObject(dst, field_name, copy);
        }
    }
}

/**
 * 检查一条消息是否是 tool_result 载体（孤儿检测用）。
 *
 * DeepSeek 工具调用流程中，工具结果以 role="user" + content 数组存储，
 * 数组内包含 type="tool_result" 的元素。
 *
 * 历史裁剪可能导致对应的 assistant(tool_calls) 消息被切掉，
 * 留下没有前序上下文的 tool_result 消息。OpenAI-compatible API 要求
 * role="tool" 消息前必须有包含 tool_calls 的 assistant 消息，
 * 否则返回 400 错误。
 *
 * @param msg   待检查的 cJSON 消息对象
 * @return true 如果是 tool_result 载体消息
 */
static bool is_tool_result_carrier(const cJSON *msg)
{
    cJSON *role = cJSON_GetObjectItem((cJSON *)msg, "role");
    if(!role || !cJSON_IsString(role) || strcmp(role->valuestring, "user") != 0)
    {
        return false;
    }

    cJSON *content = cJSON_GetObjectItem((cJSON *)msg, "content");
    if(!content || !cJSON_IsArray(content))
    {
        return false;
    }

    /* 检查 content 数组中是否存在 type="tool_result" 的元素 */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, content)
    {
        cJSON *type = cJSON_GetObjectItem(item, "type");
        if(type && cJSON_IsString(type) && strcmp(type->valuestring, "tool_result") == 0)
        {
            return true;
        }
    }

    return false;
}

int session_append_message_json(const char *channel, const char *chat_id,
                                const cclaw_cJSON *message)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }

    if(!message)
    {
        return -1;
    }

    /* 路径缓冲区 384 字节，同 session_append() 的理由 */
    char path[384];
    if(session_path(channel, chat_id, path, sizeof(path)) != 0)
    {
        return -1;
    }

    if(agent_fs_ensure_parent_dir(path) != 0)
    {
        return -1;
    }

    FILE *f = fopen(path, "a");
    if(!f)
    {
        AGENT_LOG("Cannot open session file %s for structured append", path);
        return -1;
    }

    /*
     * 白名单复制：只保存 LLM API 需要的字段，忽略内部临时字段。
     *
     * 这样做的好处：
     * 1. session 文件不会膨胀（不存 ts 以外的元数据）
     * 2. 回放 history 时，LLM API 能收到完整的结构化消息
     * 3. 即使以后增加新字段，只需在此白名单里添加即可
     */
    cJSON *obj = cJSON_CreateObject();
    if(!obj)
    {
        fclose(f);
        return -1;
    }

    /* role 是必需字段 */
    copy_string_field(obj, message, "role");

    /* 内容：可能是纯文本字符串，也可能是 content blocks 数组。
     * 使用 copy_deep_field 统一处理两种情况。 */
    copy_deep_field(obj, message, "content");

    /* DeepSeek 思考模式推理内容（必须回传，否则 API 返回 400） */
    copy_string_field(obj, message, "reasoning_content");

    /* 工具调用（assistant 消息中的 tool_calls 数组） */
    copy_deep_field(obj, message, "tool_calls");

    /* 工具结果消息的 tool_call_id */
    copy_string_field(obj, message, "tool_call_id");

    /* 时间戳 */
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    /*
     * 写入失败检测。
     *
     * 旧实现只检查 line 是否为 NULL，但即使 line 非空，
     * fprintf 也可能因磁盘满、权限变更等原因写入失败，
     * fclose 同样可能失败（延迟写入错误）。
     *
     * 如果写入失败但返回成功（0），session 文件会出现：
     * 1. 空行或截断的 JSON → session_get_history_json 解析失败
     * 2. 漏写 assistant 消息的 tool_calls → 下一轮 DeepSeek API 400
     * 3. 漏写 tool_result → LLM 上下文缺少工具结果配对
     *
     * 修复：
     * - line == NULL（cJSON 内存分配失败）→ 返回 -1
     * - fprintf 返回值 < 预期写入字节数 → 返回 -1
     * - fclose 返回非 0（延迟写入错误）→ 返回 -1
     */
    if(!line)
    {
        AGENT_LOG("Session: cJSON_PrintUnformatted returned NULL (OOM?)");
        fclose(f);
        return -1;
    }

    size_t line_len = strlen(line);
    int written = fprintf(f, "%s\n", line);
    agent_free(line);

    if(written < 0 || (size_t)written < line_len + 1)
    {
        /* fprintf 返回值包括 "%s\n" 中的 '\n'，所以至少应为 line_len + 1 */
        AGENT_LOG("Session: fprintf wrote %d bytes, expected %lu (disk full?)",
                  written, (unsigned long)(line_len + 1));
        fclose(f);
        return -1;
    }

    if(fclose(f) != 0)
    {
        /* fclose 失败通常意味着延迟写入错误（如磁盘空间不足）。
         * 此时文件内容可能不完整。记录日志但无法回滚已写入的数据。
         * 返回 -1 让调用方知道写入异常，避免在残缺上下文上继续构建。 */
        AGENT_LOG("Session: fclose failed (delayed write error, disk full?)");
        return -1;
    }

    return 0;
}

int session_get_history_json(const char *channel, const char *chat_id,
                             char *buf, size_t size, int max_msgs)
{
    /* 校验 max_msgs：用于 ring buffer 的取模运算和数组下标。
     * - <= 0 会导致除零或非法内存访问
     * - > AGENT_SESSION_MAX_MSGS 会越界写入 messages[] 数组
     * 将非法值 clamp 到安全范围 [1, AGENT_SESSION_MAX_MSGS]。 */
    if(max_msgs <= 0 || max_msgs > AGENT_SESSION_MAX_MSGS)
    {
        if(max_msgs <= 0)
        {
            AGENT_LOG("Session: max_msgs=%d invalid, clamped to 1", max_msgs);
            max_msgs = 1;
        }
        else
        {
            AGENT_LOG("Session: max_msgs=%d exceeds AGENT_SESSION_MAX_MSGS=%d, clamped",
                      max_msgs, AGENT_SESSION_MAX_MSGS);
            max_msgs = AGENT_SESSION_MAX_MSGS;
        }
    }

    if(agent_tlsf_init() != 0)
    {
        snprintf(buf, size, "[]");
        return -1;
    }

    /* 路径缓冲区 384 字节，同 session_append() 的理由 */
    char path[384];
    if(session_path(channel, chat_id, path, sizeof(path)) != 0)
    {
        snprintf(buf, size, "[]");
        return -1;
    }

    FILE *f = fopen(path, "r");
    if(!f)
    {
        /* No history yet */
        snprintf(buf, size, "[]");
        return 0;
    }

    /* 动态行读取：支持超长 JSONL 行。
     *
     * 结构化消息（含 reasoning_content、tool_calls、tool_result 等）
     * 可能远超固定 2048 字节限制。使用 TLSF 动态分配 + 按需扩容：
     *
     * 1. 初始分配 2048 字节（覆盖绝大多数普通消息）
     * 2. fgets 每次读入缓冲区的剩余空间
     * 3. 如果行未以 '\n' 结尾且缓冲区将满，realloc 扩容到 2 倍
     * 4. 重复直到读到完整一行或 EOF
     *
     * 数据流示意（超长行场景）：
     *
     *   第 1 次 fgets:  {"role":"assistant","content":[{"type":"text","text":"..."},{"type":"reasoning","reasoning_con
     *                   ↑ 缓冲区 2048 满，无 '\n' → realloc 到 4096
     *
     *   第 2 次 fgets:  tent":"很长的推理内容..."},{"type":"tool_use","id":"call_1","name":"weather","input":{...}}]}
     *                   ↑ 读到 '\n' → 行完整，cJSON_Parse 成功
     *
     * 如果 agent_realloc 失败（TLSF 内存不足），用已读到的部分内容
     * 尝试解析，解析失败则跳过该行并记录日志。
     */
    cJSON *messages[AGENT_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    size_t line_cap = 2048;
    char *line = agent_malloc(line_cap);
    if(!line)
    {
        fclose(f);
        snprintf(buf, size, "[]");
        return -1;
    }

    while(1)
    {
        /* 读取完整的一行 JSONL。
         * 内层 for 循环持续读取直到遇到 '\n'（行完整）或 EOF。
         * 缓冲区不够时动态扩容到 2 倍。 */
        size_t line_len = 0;

        for(;;)
        {
            /* 剩余空间不足 64 字节则提前扩容，避免 fgets 只读极少字符 */
            if(line_cap - line_len < 64)
            {
                size_t new_cap = line_cap * 2;
                char *p = agent_realloc(line, new_cap);
                if(!p)
                {
                    AGENT_LOG("Session line OOM at %lu bytes, parse partial",
                              (unsigned long)line_cap);
                    break;
                }
                line = p;
                line_cap = new_cap;
            }

            if(!fgets(line + line_len, (int)(line_cap - line_len), f))
            {
                break;  /* EOF 或读错误 */
            }

            line_len += strlen(line + line_len);

            /* 读到换行符 → 一行完整，跳出内层循环去解析 */
            if(line_len > 0 && line[line_len - 1] == '\n')
            {
                break;
            }
            /* 未读到换行且还有空间 → EOF 但无换行，行也视为完整 */
        }

        if(line_len == 0)
        {
            break;  /* EOF，无更多数据 */
        }

        /* 去掉末尾换行符 */
        if(line_len > 0 && line[line_len - 1] == '\n')
        {
            line[--line_len] = '\0';
        }
        if(line[0] == '\0')
        {
            continue;
        }

        cJSON *obj = cJSON_Parse(line);
        if(!obj)
        {
            /* 解析失败：可能是超长行被截断（TLSF OOM），记录前 80 字符辅助排查 */
            AGENT_LOG("Session line parse failed (len=%lu): %.80s",
                      (unsigned long)line_len, line);
            continue;
        }

        /* Ring buffer: overwrite oldest if full */
        if(count >= max_msgs)
        {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if(count < max_msgs)
        {
            count++;
        }
    }

    fclose(f);
    agent_free(line);
    line = NULL;

    /* Build JSON array — 白名单复制结构化字段。
     *
     * 不能只取 role + content：DeepSeek 思考模式要求后续请求
     * 携带 assistant 的 reasoning_content 和 tool_calls，
     * 否则 API 返回 400。role="tool" 消息也需要 tool_call_id。
     */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;

    /* 跳过开头的孤儿 tool_result 消息。
     *
     * 背景：session 文件按 JSONL 逐行存储消息，加载历史时用 ring buffer
     * 按 max_msgs 上限裁剪。裁剪以单条消息为单位，不感知消息间的逻辑配对。
     * DeepSeek 工具调用流程产生的消息序列为：
     *
     *   assistant(tool_calls, reasoning_content)  ← 工具调用请求
     *   user([tool_result, tool_result, ...])      ← 工具执行结果
     *   assistant(text, reasoning_content)          ← 最终回答（或下一轮工具调用）
     *
     * 当 max_msgs 裁剪切掉 assistant(tool_calls) 但保留了后续的
     * user(tool_result) 时，这条 user 消息就成了"孤儿"：
     * convert_messages_openai_compat() 会将其中的 tool_result 块
     * 转换为 role="tool" 消息，但 API 要求 role="tool" 之前必须有
     * 包含 tool_calls 的 assistant 消息，否则返回 400。
     *
     * 典型裁剪场景（max_msgs=4，总共 6 条消息）：
     *
     *   全量 session:                          裁剪后（取最后 4 条）：
     *   ┌─────────────────────────┐            ┌──────────────────────────────┐
     *   │ user("今天天气")         │ ← 被裁掉   │ user(tool_result) ← 孤儿     │
     *   │ assistant(tool_calls)   │ ← 被裁掉   │ assistant("晴天25°C")        │
     *   │ user(tool_result)       │            │ user("明天呢")               │
     *   │ assistant("晴天25°C")   │            │ assistant("明天多云")        │
     *   │ user("明天呢")          │            └──────────────────────────────┘
     *   │ assistant("明天多云")   │              ↑ API 收到 role="tool"
     *   └─────────────────────────┘              但没有前序 assistant(tool_calls)
     *                                            → 400 Bad Request
     *
     * 解决方案：从 ring buffer 输出的头部开始扫描，跳过所有连续的
     * tool_result 载体消息（is_tool_result_carrier() 返回 true），
     * 直到遇到第一条非孤儿消息为止。
     *
     * 跳过后的有效序列（以本例）：
     *   assistant("晴天25°C")   ← 有效起始（assistant 不依赖前序上下文）
     *   user("明天呢")          ← 有效
     *   assistant("明天多云")   ← 有效
     *   → API 正常处理
     *
     * 注意事项：
     * - 只跳过开头的连续孤儿，中间的 tool_result 不会受影响
     *   （中间的 tool_result 前面一定有对应的 assistant(tool_calls)）
     * - 如果所有消息都是孤儿（极端情况），skip_count == count，
     *   输出为空数组 []，API 按无历史处理，不会报错
     */
    int skip_count = 0;
    for(int i = 0; i < count; i++)
    {
        int idx = (start + i) % max_msgs;
        if(is_tool_result_carrier(messages[idx]))
        {
            skip_count++;
        }
        else
        {
            break;
        }
    }

    for(int i = skip_count; i < count; i++)
    {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role = cJSON_GetObjectItem(src, "role");
        if(!role || !cJSON_IsString(role))
        {
            cJSON_Delete(entry);
            continue;
        }

        /* 必需字段：role */
        cJSON_AddStringToObject(entry, "role", role->valuestring);

        /* 白名单字段：content, reasoning_content, tool_calls, tool_call_id。
         * content 使用 copy_deep_field 以支持字符串和 content blocks 数组。 */
        copy_deep_field(entry, src, "content");
        copy_string_field(entry, src, "reasoning_content");
        copy_deep_field(entry, src, "tool_calls");
        copy_string_field(entry, src, "tool_call_id");

        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for(int i = 0; i < count; i++)
    {
        int idx = (cleanup_start + i) % max_msgs;
        cJSON_Delete(messages[idx]);
    }

    /* 将 JSON 数组序列化为字符串，确保不超过输出缓冲区大小。
     *
     * 如果序列化结果超过 buf 的 size，不能直接 strncpy 截断，
     * 因为截断会产生非法 JSON（如 [{"role":"user","conten），
     * cJSON_Parse 加载时会失败，等于所有历史都丢失。
     *
     * 处理策略：从数组头部逐步移除最旧的消息，直到剩余消息
     * 能完整放入缓冲区。每次移除后还需重新检测孤儿 tool_result
     * （移除 assistant(tool_calls) 可能使后面的 user(tool_result) 变成孤儿）。
     *
     * 示意（size 不足时）：
     *
     *   原始数组: [msg1, msg2, msg3, msg4, msg5]  → 序列化 12KB > size(8KB)
     *   移除 msg1: [msg2, msg3, msg4, msg5]        → 序列化 10KB > size
     *   移除 msg2: [msg3, msg4, msg5]              → 序列化 7KB < size ✓
     *   msg3 是孤儿 tool_result → 跳过
     *   最终输出: [msg4, msg5]                      → 合法 JSON
     */
    char *json_str = cJSON_PrintUnformatted(arr);

    while(json_str && strlen(json_str) >= size)
    {
        /* 数组已空，无法再裁剪 */
        if(cJSON_GetArraySize(arr) == 0)
        {
            break;
        }

        /* 移除最旧的一条消息 */
        cJSON_DeleteItemFromArray(arr, 0);

        /* 移除后检查新的首条是否变成孤儿 tool_result */
        while(cJSON_GetArraySize(arr) > 0)
        {
            cJSON *first = cJSON_GetArrayItem(arr, 0);
            if(first && is_tool_result_carrier(first))
            {
                cJSON_DeleteItemFromArray(arr, 0);
            }
            else
            {
                break;
            }
        }

        /* 重新序列化 */
        agent_free(json_str);
        json_str = cJSON_PrintUnformatted(arr);
    }

    cJSON_Delete(arr);

    if(json_str)
    {
        size_t json_len = strlen(json_str);
        if(json_len < size)
        {
            /* 完整放入缓冲区（含 '\0'） */
            memcpy(buf, json_str, json_len + 1);
        }
        else
        {
            /* 防御性兜底：理论上不应到达此处（上面已处理） */
            memcpy(buf, json_str, size - 1);
            buf[size - 1] = '\0';
            AGENT_LOG("History JSON truncated to %lu bytes (should not happen)",
                      (unsigned long)(size - 1));
        }
        agent_free(json_str);
    }
    else
    {
        snprintf(buf, size, "[]");
    }

    return 0;
}

int session_clear(const char *channel, const char *chat_id)
{
    /* 路径缓冲区 384 字节，同 session_append() 的理由 */
    char path[384];
    if(session_path(channel, chat_id, path, sizeof(path)) != 0)
    {
        return -1;
    }

    if(remove(path) == 0)
    {
        AGENT_LOG("Session %s:%s cleared", channel ? channel : "?", chat_id);
        return 0;
    }
    return -1;
}

void session_list(void)
{
    DIR *dir = opendir(AGENT_SESSION_DIR);
    if(!dir)
    {
        dir = opendir(AGENT_BASE);
        if(!dir)
        {
            AGENT_LOG("Cannot open %s directory", AGENT_BASE);
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while((entry = readdir(dir)) != NULL)
    {
        /* session 文件格式为 <channel>_<chat_id>.jsonl，匹配 .jsonl 后缀即可 */
        if(strstr(entry->d_name, ".jsonl"))
        {
            AGENT_LOG("  Session: %s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if(count == 0)
    {
        AGENT_LOG("No sessions found");
    }
}
