#include "llm_proxy.h"
#include "agent_conf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_http.h"
#include "agent_tlsf.h"
#include "cJSON/cJSON.h"

#define LLM_API_KEY_MAX_LEN 320
#define LLM_MODEL_MAX_LEN   64
#define LLM_DUMP_MAX_BYTES  (16 * 1024)
#define LLM_DUMP_CHUNK_BYTES 320

static char s_api_key[LLM_API_KEY_MAX_LEN] = {0};
static char s_model[LLM_MODEL_MAX_LEN] = AGENT_LLM_DEFAULT_MODEL;
static const char *s_api_key_source = "none";

/*
 * Provider 扩展点：
 *
 * 这个文件当前只注册 DeepSeek，但实现上不把 DeepSeek 写死在请求流程里。
 * 每个 provider 通过 llm_provider_def_t 描述三类差异：
 * 1. API 地址和 API key 环境变量；
 * 2. 请求 JSON 的构造方式；
 * 3. 响应 JSON 的解析方式，以及鉴权 header 的生成方式。
 *
 * DeepSeek 使用 OpenAI-compatible 的 /chat/completions 协议，所以它复用
 * build_openai_compat_request() 和 parse_openai_compat_response()。
 * 以后如果新增 provider：
 * - OpenAI-compatible provider 只需要在 s_provider_defs[] 里新增一项；
 * - 非兼容 provider 则新增自己的 build_request/parse_response/header 函数。
 */
typedef struct llm_provider_def llm_provider_def_t;

typedef char *(*llm_build_request_fn)(const llm_provider_def_t *provider,
                                      const char *system_prompt,
                                      cJSON *messages,
                                      const char *tools_json);
typedef int (*llm_parse_response_fn)(const llm_provider_def_t *provider,
                                     const char *json,
                                     llm_response_t *resp);
typedef int (*llm_format_auth_header_fn)(char *out,
                                         size_t out_size,
                                         const char *api_key);

struct llm_provider_def
{
    const char *name;
    const char *api_url;
    const char *api_key_env;
    llm_build_request_fn build_request;
    llm_parse_response_fn parse_response;
    llm_format_auth_header_fn format_auth_header;
};

static char *build_openai_compat_request(const llm_provider_def_t *provider,
                                         const char *system_prompt,
                                         cJSON *messages,
                                         const char *tools_json);
static int parse_openai_compat_response(const llm_provider_def_t *provider,
                                        const char *json,
                                        llm_response_t *resp);
static int format_bearer_auth_header(char *out,
                                     size_t out_size,
                                     const char *api_key);

static const llm_provider_def_t s_provider_defs[] =
{
    {
        AGENT_LLM_PROVIDER_NAME,
        AGENT_LLM_API_URL,
        AGENT_DEEPSEEK_API_KEY_ENV,
        build_openai_compat_request,
        parse_openai_compat_response,
        format_bearer_auth_header,
    },
};

static const llm_provider_def_t *s_provider = &s_provider_defs[0];

/* 在 provider 注册表中按名字查找。当前只会命中 deepseek。 */
static const llm_provider_def_t *find_provider(const char *name)
{
    size_t count = sizeof(s_provider_defs) / sizeof(s_provider_defs[0]);

    if(!name || !name[0])
    {
        return NULL;
    }

    for(size_t i = 0; i < count; i++)
    {
        if(strcmp(s_provider_defs[i].name, name) == 0)
        {
            return &s_provider_defs[i];
        }
    }

    return NULL;
}

static size_t bounded_strlen(const char *s, size_t max_len)
{
    size_t len = 0;
    if(!s)
    {
        return 0;
    }
    while(len < max_len && s[len] != '\0')
    {
        len++;
    }
    return len;
}

static char *llm_strdup(const char *src)
{
    if(!src)
    {
        return NULL;
    }

    size_t len = strlen(src);
    char *copy = agent_malloc(len + 1);
    if(!copy)
    {
        return NULL;
    }
    memcpy(copy, src, len + 1);
    return copy;
}

static bool append_text(char **dst, size_t *dst_len, const char *src)
{
    if(!src || !src[0])
    {
        return true;
    }

    size_t src_len = strlen(src);
    char *tmp = agent_realloc(*dst, *dst_len + src_len + 1);
    if(!tmp)
    {
        return false;
    }

    memcpy(tmp + *dst_len, src, src_len);
    *dst_len += src_len;
    tmp[*dst_len] = '\0';
    *dst = tmp;
    return true;
}

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if(!dst || dst_size == 0)
    {
        return;
    }
    if(!src)
    {
        dst[0] = '\0';
        return;
    }

    size_t n = bounded_strlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void llm_log_payload(const char *label, const char *payload)
{
    if(!payload)
    {
        AGENT_LOG("%s: <null>", label);
        return;
    }

    size_t total = strlen(payload);
#if AGENT_LLM_LOG_VERBOSE_PAYLOAD
    size_t shown = total > LLM_DUMP_MAX_BYTES ? LLM_DUMP_MAX_BYTES : total;
    AGENT_LOG("%s (%u bytes)%s",
              label,
              (unsigned)total,
              (shown < total) ? " [truncated]" : "");

    char chunk[LLM_DUMP_CHUNK_BYTES + 1];
    for(size_t off = 0; off < shown; off += LLM_DUMP_CHUNK_BYTES)
    {
        size_t n = shown - off;
        if(n > LLM_DUMP_CHUNK_BYTES)
        {
            n = LLM_DUMP_CHUNK_BYTES;
        }
        memcpy(chunk, payload + off, n);
        chunk[n] = '\0';
        AGENT_LOG("%s[%u]: %s", label, (unsigned)off, chunk);
    }
#else
    if(AGENT_LLM_LOG_PREVIEW_BYTES > 0)
    {
        size_t shown = total > AGENT_LLM_LOG_PREVIEW_BYTES ? AGENT_LLM_LOG_PREVIEW_BYTES : total;
        char preview[AGENT_LLM_LOG_PREVIEW_BYTES + 1];
        memcpy(preview, payload, shown);
        preview[shown] = '\0';
        for(size_t i = 0; i < shown; i++)
        {
            if(preview[i] == '\n' || preview[i] == '\r' || preview[i] == '\t')
            {
                preview[i] = ' ';
            }
        }
        AGENT_LOG("%s (%u bytes): %s%s",
                  label,
                  (unsigned)total,
                  preview,
                  (shown < total) ? " ..." : "");
    }
    else
    {
        AGENT_LOG("%s (%u bytes)", label, (unsigned)total);
    }
#endif
}

static void llm_log_tlsf_stats(const char *label)
{
    tlsf_stats_t stats;

    if(agent_tlsf_get_stats(&stats) == 0)
    {
        AGENT_LOG("%s: tlsf free=%u largest_free=%u used=%u blocks=%u free_blocks=%u overhead=%u",
                  label,
                  (unsigned)stats.total_free,
                  (unsigned)stats.largest_free,
                  (unsigned)stats.total_used,
                  (unsigned)stats.block_count,
                  (unsigned)stats.free_count,
                  (unsigned)stats.overhead);
    }
    else
    {
        AGENT_LOG("%s: tlsf stats unavailable", label);
    }
}

typedef struct
{
    char *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static int resp_buf_init(resp_buf_t *rb, size_t initial_cap)
{
    rb->data = agent_calloc(1, initial_cap);
    if(!rb->data)
    {
        return -1;
    }
    rb->len = 0;
    rb->cap = initial_cap;
    return 0;
}

static void resp_buf_free(resp_buf_t *rb)
{
    agent_free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

#ifndef LLM_PROXY_NO_HTTP
/*
 * HTTP body callback: append response bytes to a dynamic buffer.
 * Unit tests define LLM_PROXY_NO_HTTP and verify provider JSON logic only.
 */
static int llm_http_body_cb(const void *contents, size_t realsize, void *userp)
{
    resp_buf_t *rb = (resp_buf_t *)userp;

    while(rb->len + realsize >= rb->cap)
    {
        size_t new_cap = rb->cap * 2;
        char *tmp = agent_realloc(rb->data, new_cap);
        if(!tmp)
        {
            AGENT_LOG("llm_http_body_cb: realloc failed (cap=%u -> %u)",
                      (unsigned)rb->cap, (unsigned)new_cap);
            return -1;
        }
        rb->data = tmp;
        rb->cap = new_cap;
    }

    memcpy(rb->data + rb->len, contents, realsize);
    rb->len += realsize;
    rb->data[rb->len] = '\0';

    return 0;
}
#endif

static const char *llm_api_url(void)
{
    return s_provider->api_url;
}

int llm_proxy_init(void)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }

    /*
     * 初始化时根据配置名选择 provider。
     * 因为注册表当前只包含 deepseek，所以这里也起到“去掉 openai/anthropic 支持”的
     * 防线：配置成其他名字会直接失败，而不是悄悄 fallback。
     */
    const llm_provider_def_t *configured_provider = find_provider(AGENT_LLM_PROVIDER_NAME);
    if(!configured_provider)
    {
        AGENT_LOG("Unsupported LLM provider: %s", AGENT_LLM_PROVIDER_NAME);
        return -1;
    }
    s_provider = configured_provider;

    /*
     * 维持当前产品行为：优先使用编译期 AGENT_SECRET_API_KEY。
     * 只有编译期 key 为空时，才从 AGENT_DEEPSEEK_API_KEY_ENV 指定的
     * 环境变量读取，避免改变既有固件部署方式。
     */
    if(AGENT_SECRET_API_KEY[0] != '\0')
    {
        safe_copy(s_api_key, sizeof(s_api_key), AGENT_SECRET_API_KEY);
        s_api_key_source = "AGENT_SECRET_API_KEY";
    }
    else
    {
        const char *env_key = getenv(s_provider->api_key_env);
        if(env_key && env_key[0])
        {
            safe_copy(s_api_key, sizeof(s_api_key), env_key);
            s_api_key_source = s_provider->api_key_env;
        }
    }

    if(AGENT_LLM_DEFAULT_MODEL[0] != '\0')
    {
        safe_copy(s_model, sizeof(s_model), AGENT_LLM_DEFAULT_MODEL);
    }

    if(s_api_key[0])
    {
        AGENT_LOG("LLM proxy initialized (provider: %s, model: %s, url: %s, api_key_source: %s, api_key_len: %d)",
                  s_provider->name, s_model, llm_api_url(),
                  s_api_key_source, (int)strlen(s_api_key));
    }
    else
    {
        AGENT_LOG("No %s API key. Set %s or AGENT_SECRET_API_KEY.",
                  s_provider->name, s_provider->api_key_env);
    }
    return 0;
}

/* DeepSeek 使用标准 Bearer token 鉴权。兼容 provider 可复用这个函数。 */
static int format_bearer_auth_header(char *out,
                                     size_t out_size,
                                     const char *api_key)
{
    if(!out || out_size == 0)
    {
        return -1;
    }
    out[0] = '\0';

    if(!api_key || !api_key[0])
    {
        return 0;
    }

    int n = snprintf(out, out_size, "Bearer %s", api_key);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

static int llm_http_direct(const char *post_data, resp_buf_t *rb, int *out_status)
{
#ifdef LLM_PROXY_NO_HTTP
    /*
     * Tests target provider request/response conversion and skip real network I/O.
     * 如果误调用到这里，返回失败可以让测试立刻暴露问题。
     */
    (void)post_data;
    (void)rb;
    (void)out_status;
    return -1;
#else
    char auth_value[LLM_API_KEY_MAX_LEN + 32] = {0};
    agent_http_header_t headers[2];
    size_t header_count = 0;

    headers[header_count++] = (agent_http_header_t)
    {
        "Content-Type",
        "application/json"
    };

    if(s_provider->format_auth_header &&
            s_provider->format_auth_header(auth_value, sizeof(auth_value),
                                           s_api_key) != 0)
    {
        return -1;
    }
    if(auth_value[0])
    {
        headers[header_count++] = (agent_http_header_t)
        {
            "Authorization",
            auth_value
        };
    }

    AGENT_LOG("POST %s (%d bytes)", llm_api_url(), (int)strlen(post_data));

    int err = agent_http_request("POST",
                                 llm_api_url(),
                                 headers,
                                 header_count,
                                 post_data,
                                 120,
                                 llm_http_body_cb,
                                 rb,
                                 out_status);
    if(err == 0)
    {
        AGENT_LOG("HTTP %d - response %u bytes",
                  out_status ? *out_status : 0,
                  (unsigned)rb->len);
    }
    else
    {
        AGENT_LOG("native HTTP request failed: %d", err);
    }

    return err == 0 ? 0 : -1;
#endif
}

static int llm_http_call(const char *post_data, resp_buf_t *rb, int *out_status)
{
    return llm_http_direct(post_data, rb, out_status);
}

/*
 * 工具 schema 转换：
 *
 * agent 内部沿用 Anthropic 风格的工具描述：
 *   { name, description, input_schema }
 *
 * DeepSeek 采用 OpenAI-compatible 工具格式：
 *   { type: "function", function: { name, description, parameters } }
 *
 * 这里故意写成 openai_compat，而不是 deepseek 专用函数。未来如果增加 Kimi、
 * OpenRouter、Groq 等同协议 provider，可以直接复用这段转换逻辑。
 */
static cJSON *convert_tools_openai_compat(const char *tools_json)
{
    if(!tools_json)
    {
        return NULL;
    }

    cJSON *arr = cJSON_Parse(tools_json);
    if(!arr || !cJSON_IsArray(arr))
    {
        cJSON_Delete(arr);
        return NULL;
    }

    cJSON *out = cJSON_CreateArray();
    if(!out)
    {
        cJSON_Delete(arr);
        return NULL;
    }

    cJSON *tool;
    cJSON_ArrayForEach(tool, arr)
    {
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "input_schema");
        if(!name || !cJSON_IsString(name))
        {
            continue;
        }

        cJSON *func = cJSON_CreateObject();
        cJSON *wrap = cJSON_CreateObject();
        if(!func || !wrap)
        {
            cJSON_Delete(func);
            cJSON_Delete(wrap);
            continue;
        }

        cJSON_AddStringToObject(func, "name", name->valuestring);
        if(desc && cJSON_IsString(desc))
        {
            cJSON_AddStringToObject(func, "description", desc->valuestring);
        }
        if(schema)
        {
            cJSON *schema_copy = cJSON_Duplicate(schema, 1);
            if(schema_copy)
            {
                cJSON_AddItemToObject(func, "parameters", schema_copy);
            }
        }
        if(!cJSON_GetObjectItem(func, "parameters"))
        {
            cJSON_AddItemToObject(func, "parameters", cJSON_CreateObject());
        }

        cJSON_AddStringToObject(wrap, "type", "function");
        cJSON_AddItemToObject(wrap, "function", func);
        cJSON_AddItemToArray(out, wrap);
    }

    cJSON_Delete(arr);
    return out;
}

/*
 * 消息历史转换：
 *
 * agent_loop 里为了保存工具调用历史，使用 content block：
 * - assistant: [{type:"text"}, {type:"tool_use"}]
 * - user:      [{type:"tool_result"}, {type:"text"}]
 *
 * DeepSeek/OpenAI-compatible API 需要扁平 messages：
 * - system prompt 变成第一条 role=system 消息；
 * - assistant 的 tool_use block 变成 message.tool_calls；
 * - user 的 tool_result block 变成独立 role=tool 消息；
 * - 普通文本仍保留 role=user/assistant。
 *
 * 这层转换让 agent_loop 不必关心具体 provider 的 wire format。
 */
static cJSON *convert_messages_openai_compat(const char *system_prompt, cJSON *messages)
{
    cJSON *out = cJSON_CreateArray();
    if(!out)
    {
        llm_log_tlsf_stats("LLM convert messages: cJSON_CreateArray failed");
        return NULL;
    }

    if(system_prompt && system_prompt[0])
    {
        cJSON *sys = cJSON_CreateObject();
        if(sys)
        {
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", system_prompt);
            cJSON_AddItemToArray(out, sys);
        }
    }

    if(!messages || !cJSON_IsArray(messages))
    {
        return out;
    }

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages)
    {
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if(!role || !cJSON_IsString(role))
        {
            continue;
        }

        if(content && cJSON_IsString(content))
        {
            cJSON *m = cJSON_CreateObject();
            if(m)
            {
                cJSON_AddStringToObject(m, "role", role->valuestring);
                cJSON_AddStringToObject(m, "content", content->valuestring);
                cJSON_AddItemToArray(out, m);
            }
            continue;
        }

        if(!content || !cJSON_IsArray(content))
        {
            continue;
        }

        if(strcmp(role->valuestring, "assistant") == 0)
        {
            cJSON *m = cJSON_CreateObject();
            cJSON *tool_calls = NULL;
            char *text_buf = NULL;
            size_t text_len = 0;
            char *reasoning_buf = NULL;     /* DeepSeek 思考模式的推理内容 */
            size_t reasoning_len = 0;

            if(!m)
            {
                continue;
            }
            cJSON_AddStringToObject(m, "role", "assistant");

            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if(btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0)
                {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if(text && cJSON_IsString(text))
                    {
                        append_text(&text_buf, &text_len, text->valuestring);
                    }
                }
                else if(btype && cJSON_IsString(btype) &&
                        strcmp(btype->valuestring, "reasoning") == 0)
                {
                    /*
                     * DeepSeek 思考模式的推理内容。
                     *
                     * 官方文档要求：带 reasoning_content 的 assistant 消息，
                     * reasoning_content 必须放在消息对象的顶层字段（与 role、
                     * content 同级），而不是嵌套在 content 数组里。
                     * 这里从 content block 中提取并累积，稍后设置到顶层。
                     */
                    cJSON *rc = cJSON_GetObjectItem(block, "reasoning_content");
                    if(rc && cJSON_IsString(rc) && rc->valuestring[0])
                    {
                        append_text(&reasoning_buf, &reasoning_len, rc->valuestring);
                    }
                }
                else if(btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0)
                {
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    if(!name || !cJSON_IsString(name))
                    {
                        continue;
                    }

                    if(!tool_calls)
                    {
                        tool_calls = cJSON_CreateArray();
                    }
                    if(!tool_calls)
                    {
                        continue;
                    }

                    cJSON *tc = cJSON_CreateObject();
                    cJSON *func = cJSON_CreateObject();
                    if(!tc || !func)
                    {
                        cJSON_Delete(tc);
                        cJSON_Delete(func);
                        continue;
                    }

                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    if(id && cJSON_IsString(id))
                    {
                        cJSON_AddStringToObject(tc, "id", id->valuestring);
                    }
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON_AddStringToObject(func, "name", name->valuestring);

                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    if(input)
                    {
                        char *args = cJSON_PrintUnformatted(input);
                        if(args)
                        {
                            cJSON_AddStringToObject(func, "arguments", args);
                            agent_free(args);
                        }
                    }
                    if(!cJSON_GetObjectItem(func, "arguments"))
                    {
                        cJSON_AddStringToObject(func, "arguments", "{}");
                    }

                    cJSON_AddItemToObject(tc, "function", func);
                    cJSON_AddItemToArray(tool_calls, tc);
                }
            }

            cJSON_AddStringToObject(m, "content", text_buf ? text_buf : "");
            if(tool_calls)
            {
                cJSON_AddItemToObject(m, "tool_calls", tool_calls);
            }
            /*
             * DeepSeek 思考模式：reasoning_content 作为顶层字段。
             * 根据 DeepSeek 官方文档，后续请求必须完整回传
             * assistant 消息的 reasoning_content，否则返回 400。
             */
            if(reasoning_buf)
            {
                cJSON_AddStringToObject(m, "reasoning_content", reasoning_buf);
            }
            cJSON_AddItemToArray(out, m);
            agent_free(text_buf);
            agent_free(reasoning_buf);
        }
        else if(strcmp(role->valuestring, "user") == 0)
        {
            cJSON *block;
            char *text_buf = NULL;
            size_t text_len = 0;

            cJSON_ArrayForEach(block, content)
            {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if(btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_result") == 0)
                {
                    cJSON *tool_id = cJSON_GetObjectItem(block, "tool_use_id");
                    if(!tool_id || !cJSON_IsString(tool_id))
                    {
                        continue;
                    }

                    cJSON *tm = cJSON_CreateObject();
                    if(!tm)
                    {
                        continue;
                    }
                    cJSON_AddStringToObject(tm, "role", "tool");
                    cJSON_AddStringToObject(tm, "tool_call_id", tool_id->valuestring);

                    cJSON *tcontent = cJSON_GetObjectItem(block, "content");
                    if(tcontent && cJSON_IsString(tcontent))
                    {
                        cJSON_AddStringToObject(tm, "content", tcontent->valuestring);
                    }
                    else
                    {
                        cJSON_AddStringToObject(tm, "content", "");
                    }
                    cJSON_AddItemToArray(out, tm);
                }
                else if(btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0)
                {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if(text && cJSON_IsString(text))
                    {
                        append_text(&text_buf, &text_len, text->valuestring);
                    }
                }
            }

            if(text_buf)
            {
                cJSON *um = cJSON_CreateObject();
                if(um)
                {
                    cJSON_AddStringToObject(um, "role", "user");
                    cJSON_AddStringToObject(um, "content", text_buf);
                    cJSON_AddItemToArray(out, um);
                }
            }
            agent_free(text_buf);
        }
    }

    return out;
}

/*
 * OpenAI-compatible 请求体构造。
 * DeepSeek 的 /chat/completions 接受 model/messages/tools/tool_choice/max_tokens。
 * 注意：这里不做 HTTP，也不读取 API key，因此可以被单元测试直接调用。
 */
static char *build_openai_compat_request(const llm_provider_def_t *provider,
                                         const char *system_prompt,
                                         cJSON *messages,
                                         const char *tools_json)
{
    (void)provider;

    cJSON *body = cJSON_CreateObject();
    if(!body)
    {
        AGENT_LOG("LLM request build: failed to allocate root JSON object");
        return NULL;
    }

    AGENT_LOG("LLM request build: model=%s messages=%d system_prompt_len=%d tools_json_len=%d",
              s_model,
              (messages && cJSON_IsArray(messages)) ? cJSON_GetArraySize(messages) : -1,
              system_prompt ? (int)strlen(system_prompt) : 0,
              tools_json ? (int)strlen(tools_json) : 0);

    if(!cJSON_AddStringToObject(body, "model", s_model))
    {
        llm_log_tlsf_stats("LLM request build: failed to add model");
        cJSON_Delete(body);
        return NULL;
    }
    if(!cJSON_AddNumberToObject(body, "max_tokens", AGENT_LLM_MAX_TOKENS))
    {
        llm_log_tlsf_stats("LLM request build: failed to add max_tokens");
        cJSON_Delete(body);
        return NULL;
    }

    cJSON *api_messages = convert_messages_openai_compat(system_prompt, messages);
    if(!api_messages)
    {
        AGENT_LOG("LLM request build: failed to convert messages");
        cJSON_Delete(body);
        return NULL;
    }
    cJSON_AddItemToObject(body, "messages", api_messages);

    if(tools_json)
    {
        cJSON *tools = convert_tools_openai_compat(tools_json);
        if(tools)
        {
            cJSON_AddItemToObject(body, "tools", tools);
            cJSON_AddStringToObject(body, "tool_choice", "auto");
        }
        else
        {
            AGENT_LOG("LLM request build: tools_json is empty or invalid, continuing without tools");
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    if(!post_data)
    {
        AGENT_LOG("LLM request build: failed to serialize JSON body");
    }
    cJSON_Delete(body);
    return post_data;
}

void llm_response_free(llm_response_t *resp)
{
    if(!resp)
    {
        return;
    }

    agent_free(resp->text);
    resp->text = NULL;
    resp->text_len = 0;
    agent_free(resp->reasoning);
    resp->reasoning = NULL;
    resp->reasoning_len = 0;
    for(int i = 0; i < resp->call_count; i++)
    {
        agent_free(resp->calls[i].input);
        resp->calls[i].input = NULL;
        resp->calls[i].input_len = 0;
    }
    resp->call_count = 0;
    resp->tool_use = false;
}

/*
 * 解析标准 tool_calls 数组。
 * arguments 在 DeepSeek 响应里通常是 JSON 字符串；如果某些兼容实现返回对象，
 * 这里也会序列化成字符串，保持 llm_tool_call_t.input 的既有契约不变。
 */
static int parse_tool_calls(cJSON *tool_calls, llm_response_t *resp)
{
    if(!tool_calls)
    {
        return 0;
    }
    if(!cJSON_IsArray(tool_calls))
    {
        AGENT_LOG("LLM parse: tool_calls exists but is not an array");
        return -1;
    }

    /*
     * 工具调用必须按“模型请求的全集”执行。
     *
     * 旧逻辑在超过 AGENT_MAX_TOOL_CALLS 时直接 break，或遇到缺 name 的条目
     * 时 continue。这样会把模型请求的 tool_calls 静默缩小成一个子集：
     * agent_loop 只执行前几个工具，并把这个子集持久化到 session。后续请求
     * 回传的上下文看似合法，但语义已经被改写，模型以为自己请求的所有工具
     * 都被处理过，实际上有一部分被本地丢弃。
     *
     * 更安全的策略是：只要 API 返回的工具调用超出本地能力或格式不完整，
     * 就拒绝整轮响应，让上层把本轮视为失败，而不是保存一个被截断的工具链。
     */
    int total_calls = cJSON_GetArraySize(tool_calls);
    if(total_calls > AGENT_MAX_TOOL_CALLS)
    {
        AGENT_LOG("LLM parse: tool_calls count %d exceeds local limit %d",
                  total_calls, AGENT_MAX_TOOL_CALLS);
        return -1;
    }

    cJSON *tc;
    int tool_index = 0;
    cJSON_ArrayForEach(tc, tool_calls)
    {
        if(!tc || !cJSON_IsObject(tc))
        {
            AGENT_LOG("LLM parse: tool_calls[%d] is not an object", tool_index);
            return -1;
        }

        cJSON *func = cJSON_GetObjectItem(tc, "function");
        cJSON *name = func ? cJSON_GetObjectItem(func, "name") : NULL;
        if(!func || !cJSON_IsObject(func) ||
                !name || !cJSON_IsString(name) || !name->valuestring[0])
        {
            AGENT_LOG("LLM parse: tool_calls[%d] missing function.name",
                      tool_index);
            return -1;
        }

        llm_tool_call_t *call = &resp->calls[resp->call_count];
        memset(call, 0, sizeof(*call));

        cJSON *id = cJSON_GetObjectItem(tc, "id");
        if(id && cJSON_IsString(id) && id->valuestring[0])
        {
            safe_copy(call->id, sizeof(call->id), id->valuestring);
        }
        else
        {
            /*
             * DeepSeek/OpenAI-compatible 的 tool_result 需要用 tool_call_id
             * 精确配对前一条 assistant.tool_calls[].id。这里不能随便造一个
             * id：造出来的 id 虽然能让本地 JSON 自洽，却不一定符合服务端
             * 对工具调用链的校验，也会掩盖上游响应损坏。
             */
            AGENT_LOG("LLM parse: tool_calls[%d] missing id", tool_index);
            return -1;
        }

        safe_copy(call->name, sizeof(call->name), name->valuestring);

        cJSON *args = cJSON_GetObjectItem(func, "arguments");
        if(args && cJSON_IsString(args))
        {
            call->input = llm_strdup(args->valuestring);
        }
        else if(args)
        {
            call->input = cJSON_PrintUnformatted(args);
        }
        else
        {
            AGENT_LOG("LLM parse: tool_calls[%d] missing function.arguments",
                      tool_index);
            return -1;
        }

        if(!call->input)
        {
            return -1;
        }
        call->input_len = strlen(call->input);
        resp->call_count++;
        tool_index++;
    }

    if(resp->call_count > 0)
    {
        resp->tool_use = true;
    }
    return 0;
}

/*
 * OpenAI-compatible 响应解析。
 * 解析结果仍然落到 llm_response_t，agent_loop 后续可以继续按原来的
 * text/tool_use/tool calls 结构处理，无需知道 DeepSeek 的 choices/message 包装。
 */
static int parse_openai_compat_response(const llm_provider_def_t *provider,
                                        const char *json,
                                        llm_response_t *resp)
{
    int rc = -1;
    cJSON *root = NULL;
    const char *provider_name = provider ? provider->name : "llm";

    memset(resp, 0, sizeof(*resp));
    if(!json)
    {
        return -1;
    }

    root = cJSON_Parse(json);
    if(!root)
    {
        AGENT_LOG("Failed to parse %s response JSON", provider_name);
        return -1;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    if(!message)
    {
        AGENT_LOG("%s response missing choices[0].message", provider_name);
        goto cleanup;
    }

    /* 校验 finish_reason：与流式路径 parse_sse_event() 保持一致。
     *
     * 非 "stop" / "tool_calls" 的 finish_reason 表示异常结束：
     * - "length": 输出被 max_tokens 截断，内容不完整
     * - "content_filter": 输出被内容安全过滤拦截
     * 这些不应被当作正常响应持久化或发送。
     */
    cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
    if(finish && cJSON_IsString(finish) && finish->valuestring[0])
    {
        if(strcmp(finish->valuestring, "tool_calls") == 0)
        {
            resp->tool_use = true;
        }
        else if(strcmp(finish->valuestring, "stop") != 0)
        {
            AGENT_LOG("%s: abnormal finish_reason '%s', response incomplete",
                      provider_name, finish->valuestring);
            goto cleanup;  /* rc = -1, llm_response_free 在 cleanup 中调用 */
        }
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if(content && cJSON_IsString(content))
    {
        resp->text = llm_strdup(content->valuestring);
        if(!resp->text)
        {
            goto cleanup;
        }
        resp->text_len = strlen(resp->text);
    }

    /* DeepSeek 思考模式推理内容（非流式路径）。
     *
     * parse_sse_event() 已在流式路径中解析 delta.reasoning_content，
     * 但非流式路径（llm_chat_tools → parse_openai_compat_response）
     * 之前遗漏了 message.reasoning_content。
     * DeepSeek 要求后续请求完整回传 assistant 的 reasoning_content，
     * 缺失时 API 返回 400。 */
    cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
    if(reasoning && cJSON_IsString(reasoning) && reasoning->valuestring[0])
    {
        resp->reasoning = llm_strdup(reasoning->valuestring);
        if(resp->reasoning)
        {
            resp->reasoning_len = strlen(resp->reasoning);
        }
    }

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if(parse_tool_calls(tool_calls, resp) != 0)
    {
        goto cleanup;
    }

    /*
     * 非流式路径的空工具调用防御。
     *
     * 上面的 finish_reason 检查在看到 "tool_calls" 时已经将
     * resp->tool_use 置为 true（第 991 行）。但 parse_tool_calls()
     * 在以下情况下仍会返回 0（成功），但 call_count 为 0：
     *
     * 1. tool_calls 字段缺失（message 中没有 tool_calls 数组）
     * 2. tool_calls 为空数组（API 返回了 "tool_calls" 但列表为空）
     * 3. 所有 tool_call 条目都被跳过（index 越界等）
     *
     * 如果不加校验，agent_loop 会进入工具执行分支但无工具可执行，
     * 持久化空的 assistant(tool_calls) 和空的 tool_result 到 session。
     * 下一轮 DeepSeek API 会因不配对的 tool_calls 而返回 400。
     *
     * 同样的校验已在流式路径（parse_sse_event 的 finish_reason 分支）
     * 中实现。此处保持非流式路径与流式路径的一致性。
     */
    if(resp->tool_use && resp->call_count <= 0)
    {
        AGENT_LOG("%s: finish_reason=tool_calls but call_count=%d, "
                  "rejecting as invalid response", provider_name, resp->call_count);
        goto cleanup;
    }

    rc = 0;

cleanup:
    cJSON_Delete(root);
    if(rc != 0)
    {
        llm_response_free(resp);
    }
    return rc;
}

#ifdef LLM_PROXY_TEST
/*
 * 单元测试导出函数：
 * 这些 helper 只暴露 provider 元信息、请求构造和响应解析，不触发 HTTP。
 * 这样测试可以覆盖 DeepSeek 功能，又不会依赖真实 API key 或网络。
 */
const char *llm_proxy_test_provider(void)
{
    return s_provider->name;
}

const char *llm_proxy_test_model(void)
{
    return s_model;
}

const char *llm_proxy_test_api_url(void)
{
    return llm_api_url();
}

char *llm_proxy_test_build_request_json(const char *system_prompt,
                                        cJSON *messages,
                                        const char *tools_json)
{
    return s_provider->build_request(s_provider, system_prompt, messages, tools_json);
}

int llm_proxy_test_parse_response_json(const char *json, llm_response_t *resp)
{
    return s_provider->parse_response(s_provider, json, resp);
}
#endif

int llm_chat_tools(const char *system_prompt,
                   cJSON *messages,
                   const char *tools_json,
                   llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if(agent_tlsf_init() != 0)
    {
        AGENT_LOG("llm_chat_tools: agent_tlsf_init failed");
        return -1;
    }

    if(s_api_key[0] == '\0')
    {
        AGENT_LOG("llm_chat_tools: missing API key (provider: %s, env: %s)",
                  s_provider ? s_provider->name : "(null)",
                  s_provider ? s_provider->api_key_env : "(null)");
        return -1;
    }

    char *post_data = s_provider->build_request(s_provider, system_prompt, messages, tools_json);
    if(!post_data)
    {
        AGENT_LOG("llm_chat_tools: failed to build request (provider: %s, system_prompt_len: %d, tools_json_len: %d)",
                  s_provider ? s_provider->name : "(null)",
                  system_prompt ? (int)strlen(system_prompt) : 0,
                  tools_json ? (int)strlen(tools_json) : 0);
        return -1;
    }

    AGENT_LOG("Calling LLM API with tools (provider: %s, model: %s, body: %d bytes)",
              s_provider->name, s_model, (int)strlen(post_data));
    llm_log_payload("LLM tools request", post_data);

    resp_buf_t rb;
    if(resp_buf_init(&rb, AGENT_LLM_STREAM_BUF_SIZE) != 0)
    {
        AGENT_LOG("llm_chat_tools: failed to allocate response buffer (%d bytes)",
                  AGENT_LLM_STREAM_BUF_SIZE);
        agent_free(post_data);
        return -1;
    }

    int status = 0;
    int err = llm_http_call(post_data, &rb, &status);
    agent_free(post_data);

    if(err != 0)
    {
        AGENT_LOG("llm_chat_tools: HTTP request failed: err=%d status=%d response_bytes=%u",
                  err, status, (unsigned)rb.len);
        llm_log_payload("LLM tools partial response", rb.data);
        resp_buf_free(&rb);
        return err;
    }

    llm_log_payload("LLM tools raw response", rb.data);

    if(status != 200)
    {
        AGENT_LOG("%s API error %d: %.500s",
                  s_provider->name, status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return -1;
    }

    err = s_provider->parse_response(s_provider, rb.data, resp);
    resp_buf_free(&rb);
    if(err != 0)
    {
        AGENT_LOG("llm_chat_tools: failed to parse %s response: %d",
                  s_provider->name, err);
        return err;
    }

    AGENT_LOG("Response: %d bytes text, %d tool calls, stop=%s",
              (int)resp->text_len, resp->call_count,
              resp->tool_use ? "tool_use" : "end_turn");

    return 0;
}

/* ================================================================
 * 流式 LLM 调用（SSE）
 *
 * 流程：
 * 1. 构建带 "stream": true 的请求 JSON
 * 2. 通过 agent_http_stream_open() 建立 SSE 连接
 * 3. 循环调用 agent_http_stream_read() 读取数据
 * 4. 逐行解析 SSE 事件
 * 5. 每解析出 delta.content 调用 on_chunk 回调
 * 6. 累积完整文本和工具调用到 resp
 * ================================================================ */

/**
 * 构建流式请求 JSON。
 *
 * 与 build_openai_compat_request() 的唯一区别是增加了 "stream": true。
 * DeepSeek 的 SSE 流式响应格式：
 *   data: {"choices":[{"delta":{"content":"Hello"}}]}
 *   data: {"choices":[{"delta":{"content":" world"}}]}
 *   data: [DONE]
 */
static char *build_stream_request(const char *system_prompt,
                                  cJSON *messages,
                                  const char *tools_json)
{
    cJSON *body = cJSON_CreateObject();
    if(!body)
    {
        AGENT_LOG("LLM stream request: failed to allocate root JSON object");
        return NULL;
    }

    AGENT_LOG("LLM stream request: model=%s messages=%d system_prompt_len=%d tools_json_len=%d",
              s_model,
              (messages && cJSON_IsArray(messages)) ? cJSON_GetArraySize(messages) : -1,
              system_prompt ? (int)strlen(system_prompt) : 0,
              tools_json ? (int)strlen(tools_json) : 0);

    if(!cJSON_AddStringToObject(body, "model", s_model))
    {
        AGENT_LOG("LLM stream request: failed to add model");
        cJSON_Delete(body);
        return NULL;
    }
    if(!cJSON_AddNumberToObject(body, "max_tokens", AGENT_LLM_MAX_TOKENS))
    {
        AGENT_LOG("LLM stream request: failed to add max_tokens");
        cJSON_Delete(body);
        return NULL;
    }

    /* 关键：启用 SSE 流式传输 */
    cJSON_AddBoolToObject(body, "stream", 1);

    /* 消息历史转换（复用已有的 convert_messages_openai_compat） */
    cJSON *api_messages = convert_messages_openai_compat(system_prompt, messages);
    if(!api_messages)
    {
        AGENT_LOG("LLM stream request: failed to convert messages");
        cJSON_Delete(body);
        return NULL;
    }
    cJSON_AddItemToObject(body, "messages", api_messages);

    /* 工具定义转换（复用已有的 convert_tools_openai_compat） */
    if(tools_json)
    {
        cJSON *tools = convert_tools_openai_compat(tools_json);
        if(tools)
        {
            cJSON_AddItemToObject(body, "tools", tools);
            cJSON_AddStringToObject(body, "tool_choice", "auto");
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if(!post_data)
    {
        AGENT_LOG("LLM stream request: failed to serialize JSON body");
    }
    return post_data;
}

/**
 * 从 SSE 事件 JSON 中解析流式增量数据。
 *
 * DeepSeek SSE 事件格式：
 *   {"id":"xxx","choices":[{"index":0,
 *     "delta":{"content":"增量文本"},
 *     "finish_reason":null}]}
 *
 * 对于思考模式，delta 中还可能包含：
 *   "reasoning_content":"思考过程文本"
 *
 * 对于工具调用，delta 中包含：
 *   "tool_calls":[{"function":{"name":"xxx","arguments":"{}"}}]
 *
 * @param json_line     一行 SSE 数据（不含 "data: " 前缀）
 * @param on_chunk      文本增量回调
 * @param userp         回调上下文
 * @param resp          累积的响应结构体
 * @param done          [out] 是否收到 finish_reason（流结束标志）
 * @return 0 成功继续，1 流结束（finish_reason 非空），-1 错误
 */
static int parse_sse_event(const char *json_line,
                           llm_stream_chunk_cb on_chunk,
                           void *userp,
                           llm_response_t *resp,
                           bool *done)
{
    cJSON *root = cJSON_Parse(json_line);
    if(!root)
    {
        /* 非 JSON 的 data 内容应报错而非静默忽略。
         *
         * parse_sse_stream() 已在 SSE 行解析层过滤了空行和注释行，
         * 到达此函数的 payload 都是 "data: " 后的内容，应为合法 JSON。
         * 解析失败意味着服务端返回了损坏的事件或错误信息（如 HTML 错误页），
         * 静默忽略会导致后续上下文缺失。 */
        AGENT_LOG("SSE event: JSON parse failed: %.80s", json_line);
        return -1;
    }

    *done = false;

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if(!choices || !cJSON_IsArray(choices))
    {
        /*
         * 合法 JSON 但缺少 choices 字段。
         *
         * 可能的原因：
         * 1. API 在 SSE 流中返回错误：
         *    data: {"error":{"message":"Rate limit exceeded","type":"rate_limit_error"}}
         *    这种情况下 choices 不存在，但有 error 对象。
         *    旧实现静默返回 0（成功），error 被吞掉后，如果后续收到 [DONE]，
         *    之前的部分响应会被当作完整响应持久化——实际上可能是错误的。
         *
         * 2. 心跳或空事件（少数 API 实现发送 data: {}）：
         *    无 choices 也无 error，可以安全忽略。
         *
         * 修复策略：
         * - 有 error 对象 → 返回错误（-1），中断流并记录错误信息
         * - 无 error 也无 choices → 静默忽略（可能是心跳），返回 0
         */
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if(error)
        {
            cJSON *err_msg = cJSON_GetObjectItem(error, "message");
            AGENT_LOG("SSE event: API error in stream: %s",
                      (err_msg && cJSON_IsString(err_msg))
                      ? err_msg->valuestring
                      : "(unknown error)");
            cJSON_Delete(root);
            return -1;
        }

        /* 无 choices 也无 error：可能是心跳/空事件，安全跳过 */
        cJSON_Delete(root);
        return 0;
    }

    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    if(!choice0)
    {
        cJSON_Delete(root);
        return 0;
    }

    /* ── 解析 delta 对象 ────────────────────────────────────── */
    cJSON *delta = cJSON_GetObjectItem(choice0, "delta");
    if(delta)
    {
        /* 1. 文本内容增量 */
        cJSON *content = cJSON_GetObjectItem(delta, "content");
        if(content && cJSON_IsString(content) && content->valuestring[0])
        {
            const char *text = content->valuestring;
            size_t text_len = strlen(text);

            /* 累积到 resp->text（用于后续 session 保存和 outbound 推送） */
            if(!append_text(&resp->text, &resp->text_len, text))
            {
                AGENT_LOG("SSE event: append_text failed (total=%u, delta=%u)",
                          (unsigned)resp->text_len, (unsigned)text_len);
                cJSON_Delete(root);
                return -1;
            }

            /* 调用回调，让调用者实时输出。
             *
             * 注意：此处立即将 content delta 推送到 CLI（stdout），
             * 如果后续 finish_reason 为 "length"/"content_filter" 或
             * 流内出现 error，已经输出的内容无法撤回。
             * stream_had_output 标记用于让 agent_loop 在流式失败时
             * 打印错误指示，告知用户之前的内容不可用。 */
            if(on_chunk)
            {
                int cb_rc = on_chunk(text, text_len, userp);
                if(cb_rc != 0)
                {
                    /* 回调返回非 0，表示调用者希望中断流式读取 */
                    AGENT_LOG("SSE event: chunk callback returned %d, aborting stream", cb_rc);
                    cJSON_Delete(root);
                    return -1;
                }
                /* 标记本轮已向 CLI 输出过内容，用于后续错误处理 */
                resp->stream_had_output = true;
            }
        }

        /* 2. DeepSeek 思考模式推理内容增量。
         *
         * DeepSeek 思考模式在 delta 中通过 reasoning_content 字段返回
         * 模型的推理（思维链）文本。这些内容不等于 content：
         * - reasoning_content：模型的内部思考过程
         * - content：最终输出给用户的文本
         *
         * 官方文档要求：发生工具调用的轮次，后续请求必须完整回传
         * assistant 消息的 reasoning_content，否则 API 返回 400。
         * 因此必须在这里累积，并在 agent_loop 构建消息历史时携带。
         */
        {
            cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
            if(reasoning && cJSON_IsString(reasoning) && reasoning->valuestring[0])
            {
                if(!append_text(&resp->reasoning, &resp->reasoning_len,
                                reasoning->valuestring))
                {
                    AGENT_LOG("SSE event: reasoning append_text failed "
                              "(total=%u, delta=%u)",
                              (unsigned)resp->reasoning_len,
                              (unsigned)strlen(reasoning->valuestring));
                    cJSON_Delete(root);
                    return -1;
                }
            }
        }

        /* 3. 工具调用增量（流式场景下工具调用也是增量发送的） */
        cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
        if(tool_calls && cJSON_IsArray(tool_calls))
        {
            cJSON *tc;
            cJSON_ArrayForEach(tc, tool_calls)
            {
                cJSON *tc_index = cJSON_GetObjectItem(tc, "index");
                int idx = cJSON_IsNumber(tc_index) ? tc_index->valueint : 0;

                /* 拒绝非法索引：负数或超出数组容量。
                 * API 返回的 index 应从 0 递增，负数或超大值
                 * 会导致 resp->calls[] 数组越界访问。 */
                if(idx < 0 || idx >= AGENT_MAX_TOOL_CALLS)
                {
                    /*
                     * 不能跳过超范围工具调用。
                     *
                     * SSE 中的 index 是模型本轮 tool_calls 的真实序号。如果第 5 个
                     * 调用超过本地 AGENT_MAX_TOOL_CALLS 后被静默跳过，agent_loop
                     * 只会执行前 4 个工具，并把这 4 个结果回传给模型；模型请求的
                     * 第 5 个工具永远得不到 tool_result，语义上等价于篡改了本轮
                     * assistant(tool_calls)。因此这里把超限视为响应不可处理，直接
                     * 中断整条流，让上层丢弃本轮部分数据并重试/报错。
                     */
                    AGENT_LOG("SSE event: tool call index out of range (%d), "
                              "treating as unsupported response",
                              idx);
                    cJSON_Delete(root);
                    return -1;
                }

                /*
                 * 工具调用槽位初始化与跳号防护。
                 *
                 * OpenAI-compatible API 的 SSE 流中，每个 tool_call 增量通过
                 * index 字段标识属于哪个调用。正常情况下 index 从 0 严格递增：
                 *   第1个增量: index=0 (含 id, name, arguments 开头)
                 *   第2个增量: index=0 (arguments 追加)
                 *   ...
                 *   第N个增量: index=1 (新的工具调用)
                 *
                 * 但某些异常场景会出现跳号：
                 * - SSE 首个 tool_call index 不是 0（服务端 bug 或中间代理丢事件）
                 * - 中间某个增量丢失，index 直接跳到更大值
                 *
                 * 旧实现中 idx >= call_count 时会把中间所有槽位全部纳入 call_count，
                 * 但这些槽位的 id/name/input 全部为零值（空字符串/NULL）。
                 * 后果：
                 * 1. agent_loop 会对空工具名执行 tool_registry_execute，返回错误
                 * 2. 空 tool_use 被持久化到 session，污染后续请求上下文
                 * 3. DeepSeek API 因缺少 tool_calls 配对而返回 400
                 *
                 * 修复策略：
                 * - idx == call_count → 正常新增下一个槽位，允许初始化
                 * - idx <  call_count → 正常追加到已有槽位（arguments 增量）
                 * - idx >  call_count → 跳号，拒绝并跳过此增量事件
                 */
                if(idx == resp->call_count)
                {
                    /* 按序新增：初始化下一个槽位 */
                    memset(&resp->calls[idx], 0, sizeof(resp->calls[idx]));
                    resp->call_count = idx + 1;
                    resp->tool_use = true;
                }
                else if(idx > resp->call_count)
                {
                    /*
                     * 跳号：index 超出当前已初始化的范围。
                     *
                     * 说明 SSE 流中存在增量丢失或服务端返回了非法 index。
                     * 这种情况下整个流已被污染，不能继续解析：
                     *
                     * 1. 如果只有跳号的 call（call_count 仍为 0）：
                     *    后续 finish_reason="tool_calls" 时 all_valid 循环
                     *    不会执行（循环条件 i < 0 为 false），直接通过，
                     *    导致 agent_loop 执行 0 个工具调用 → 异常行为。
                     *
                     * 2. 如果已有 index 0 但跳到 index 2（缺少 index 1）：
                     *    calls[1] 为零值，agent_loop 会以空名字执行工具。
                     *
                     * 旧实现用 continue 静默跳过，只是忽略了当前增量，
                     * 但后续 finish 仍可能成功，导致不完整的工具调用链。
                     * 正确做法：把跳号视为流损坏，直接 return -1 中断整个流，
                     * 让 agent_loop 丢弃本轮所有数据，重新请求。
                     */
                    AGENT_LOG("SSE event: tool call index jumped (idx=%d, call_count=%d), "
                              "treating as corrupted stream",
                              idx, resp->call_count);
                    cJSON_Delete(root);
                    return -1;
                }
                /* else: idx < call_count → 追加到已有槽位，正常处理 */

                llm_tool_call_t *call = &resp->calls[idx];

                /* 工具调用 ID（通常在第一个 delta 中出现） */
                cJSON *id = cJSON_GetObjectItem(tc, "id");
                if(id && cJSON_IsString(id) && id->valuestring[0])
                {
                    safe_copy(call->id, sizeof(call->id), id->valuestring);
                }

                /* 函数名和参数（增量追加） */
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                if(func)
                {
                    cJSON *name = cJSON_GetObjectItem(func, "name");
                    if(name && cJSON_IsString(name) && name->valuestring[0])
                    {
                        safe_copy(call->name, sizeof(call->name), name->valuestring);
                    }

                    cJSON *args = cJSON_GetObjectItem(func, "arguments");
                    if(args && cJSON_IsString(args) && args->valuestring[0])
                    {
                        /* 参数是增量字符串，需要追加到已有的参数后面 */
                        if(!append_text(&call->input, &call->input_len,
                                        args->valuestring))
                        {
                            AGENT_LOG("SSE event: tool args append failed (tool=%s, idx=%d)",
                                      call->name[0] ? call->name : "?", idx);
                            cJSON_Delete(root);
                            return -1;
                        }
                    }
                }
            }
        }
    }

    /* ── 检查 finish_reason ───────────────────────────────────
     *
     * OpenAI-compatible API 的 finish_reason 取值：
     *   "stop"           — 正常完成（模型自然结束或遇到 stop 序列）
     *   "tool_calls"     — 模型请求执行工具调用
     *   "length"         — 输出因 max_tokens 限制被截断，内容不完整
     *   "content_filter" — 输出被内容安全过滤拦截，内容不可用
     *
     * 旧实现把任意非空 finish_reason 都当作成功（*done = true），
     * 导致 length/content_filter 等异常结束也被当成正常响应：
     * - length: 截断的半句话被持久化到 session 并发送给用户
     * - content_filter: 空响应或被拦截的内容被当作完整回复
     *
     * 修复：只有 "stop" 和 "tool_calls" 是正常结束，
     * 其他值返回错误，由调用者（agent_loop）丢弃该轮响应。
     */
    cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
    if(finish && cJSON_IsString(finish) && finish->valuestring[0])
    {
        if(strcmp(finish->valuestring, "stop") == 0)
        {
            /* 正常完成 */
            *done = true;
        }
        else if(strcmp(finish->valuestring, "tool_calls") == 0)
        {
            /*
             * 工具调用完成校验：确保所有 tool_call 槽位都是完整的。
             *
             * 正常情况下，finish_reason="tool_calls" 时 resp->calls[] 中的
             * 每个槽位都应有完整的 id、name 和 input（arguments）。
             * 但如果 SSE 流中存在增量丢失、跳号被拒绝等异常，
             * 可能出现缺少 id/name 或 arguments 为空的工具调用。
             *
             * 不完整的工具调用会导致：
             * 1. agent_loop 中 tool_registry_execute 以空名字查找工具 → 失败
             * 2. 持久化空 tool_use 到 session → 后续请求 API 400
             * 3. 后续 LLM 请求缺少 tool_call_id 配对 → 上下文错误
             *
             * 校验规则：
             * - call_count 必须 > 0（至少有一个工具调用）
             * - call->id[0] 必须非空（API 应返回 "call_xxx" 格式的 ID）
             * - call->name[0] 必须非空（函数名必须存在）
             * - call->input 必须非 NULL（arguments 至少是 "{}"）
             *
             * call_count == 0 的防御：
             * 如果所有 tool_call 增量都因跳号被丢弃（已改为 return -1），
             * 或 API 异常返回了 finish_reason="tool_calls" 但无实际 tool_call，
             * call_count 为 0 时 for 循环不执行，all_valid 仍为 true，
             * 会把一个空工具调用当成成功。必须显式检查 call_count > 0。
             */
            if(resp->call_count <= 0)
            {
                AGENT_LOG("SSE event: finish_reason=tool_calls but call_count=%d, "
                          "rejecting as corrupted stream", resp->call_count);
                cJSON_Delete(root);
                return -1;
            }

            bool all_valid = true;
            for(int i = 0; i < resp->call_count; i++)
            {
                const llm_tool_call_t *call = &resp->calls[i];
                if(!call->id[0] || !call->name[0] || !call->input)
                {
                    AGENT_LOG("SSE event: tool call[%d] incomplete "
                              "(id='%s', name='%s', input=%s), "
                              "rejecting tool_calls finish",
                              i,
                              call->id[0]  ? call->id  : "(empty)",
                              call->name[0] ? call->name : "(empty)",
                              call->input   ? "present" : "NULL");
                    all_valid = false;
                    break;
                }
            }

            if(!all_valid)
            {
                /* 校验失败：丢弃本轮工具调用，返回错误让 agent_loop 忽略 */
                cJSON_Delete(root);
                return -1;
            }

            /* 校验通过：模型请求执行工具，可以继续 ReAct 循环 */
            resp->tool_use = true;
            *done = true;
        }
        else
        {
            /* 异常结束：length（截断）、content_filter（过滤）等。
             * 这些不是正常响应，不应被持久化或发送给用户。
             * 返回 -1 让 parse_sse_stream 和 agent_loop 丢弃本轮数据。 */
            AGENT_LOG("SSE event: abnormal finish_reason '%s', treating as error",
                      finish->valuestring);
            cJSON_Delete(root);
            return -1;
        }
    }

    cJSON_Delete(root);
    return 0;
}

/**
 * 从 SSE 流中逐行读取并解析事件。
 *
 * SSE 协议格式：
 *   event: message
 *   data: {"choices":[...]}
 *
 *   data: {"choices":[...]}
 *
 *   data: [DONE]
 *
 * 每个事件以空行（\n\n）分隔。data 行以 "data: " 开头。
 * 我们只需要关心 data 行，忽略 event: 和其他字段。
 *
 * @param stream        HTTP 流句柄
 * @param on_chunk      文本增量回调
 * @param userp         回调上下文
 * @param resp          累积的响应结构体
 * @return 0 成功，-1 失败
 */
static int parse_sse_stream(agent_http_stream_t *stream,
                            llm_stream_chunk_cb on_chunk,
                            void *userp,
                            llm_response_t *resp)
{
    /*
     * 行缓冲区：SSE 数据按行组织，我们需要收集完整的行再解析。
     * 初始分配 256 字节，不够时通过 agent_realloc 动态扩容，
     * 由 TLSF 分配器管理内存，不做硬性上限截断。
     */
    size_t line_cap = 256;
    char *line_buf = agent_malloc(line_cap);
    size_t line_len = 0;

    /*
     * 读缓冲区：每次从流中读取一块原始数据。
     * 初始 1KB，同样由 TLSF 管理而非栈上固定数组。
     */
    size_t read_buf_size = 1024;
    char *read_buf = agent_malloc(read_buf_size);

    if(!line_buf || !read_buf)
    {
        AGENT_LOG("SSE parse: failed to allocate buffers");
        agent_free(line_buf);
        agent_free(read_buf);
        return -1;
    }

    /*
     * rc 初始为 -1（失败），只有收到 [DONE] 或 finish_reason 时才设为 0。
     * 这样 EOF（网络中断）在没有收到终止事件的情况下不会把半截回答当成成功。
     *
     * done_received 跟踪是否收到了 SSE 规范要求的终止事件：
     * - [DONE] 标记：DeepSeek/OpenAI 流结束信号
     * - finish_reason 非空：说明 LLM 已完成本轮生成
     * 两者满足任一即为正常结束。
     */
    int rc = -1;
    bool done_received = false;

    for(;;)
    {
        /* 从 HTTP 流中读取一块数据 */
        int n = agent_http_stream_read(stream, read_buf, read_buf_size);
        if(n < 0)
        {
            AGENT_LOG("SSE parse: stream read error");
            goto done;
        }
        if(n == 0)
        {
            /*
             * 服务端关闭连接（EOF）。
             *
             * 正常的 SSE 流应该在关闭前发送 [DONE] 或带 finish_reason 的事件。
             * 如果没有收到终止事件就遇到 EOF，说明是异常断开（网络中断、
             * 服务端崩溃等），此时 resp 中的内容可能是不完整的。
             *
             * 不再把有部分内容就当作成功：半截回答持久化到 session 后，
             * 后续 API 调用可能因损坏上下文而级联失败。
             *
             * rc 保持 -1（失败），llm_chat_stream() 的调用者会收到错误码，
             * 不再持久化不完整的响应。
             */
            if(!done_received)
            {
                AGENT_LOG("SSE parse: stream EOF without termination event "
                          "(text=%u bytes, reasoning=%u bytes, calls=%d)",
                          (unsigned)resp->text_len,
                          (unsigned)resp->reasoning_len,
                          resp->call_count);
            }
            else
            {
                AGENT_LOG("SSE parse: stream EOF (after termination event)");
            }
            break;
        }

        /* 将读到的数据逐字节处理，按 \n 分行 */
        for(int i = 0; i < n; i++)
        {
            char c = read_buf[i];

            if(c == '\n')
            {
                /* 一行结束，处理这行 SSE 数据 */
                line_buf[line_len] = '\0';

                /* 跳过空行（SSE 事件之间的分隔符） */
                if(line_len == 0)
                {
                    line_len = 0;
                    continue;
                }

                /* 只处理 "data: " 开头的行，忽略 event:、id: 等其他 SSE 字段 */
                if(line_len > 6 && strncmp(line_buf, "data: ", 6) == 0)
                {
                    const char *payload = line_buf + 6;

                    /* 检查流结束标记 [DONE]。
                     * 这是 DeepSeek/OpenAI SSE 协议的正常结束信号，
                     * 表示服务端已完成所有事件推送。 */
                    if(strcmp(payload, "[DONE]") == 0)
                    {
                        AGENT_LOG("SSE parse: received [DONE]");
                        done_received = true;
                        rc = 0;
                        goto done;
                    }

                    /* 解析 SSE 事件 JSON */
                    bool done_flag = false;
                    int err = parse_sse_event(payload, on_chunk, userp,
                                              resp, &done_flag);
                    if(err != 0)
                    {
                        AGENT_LOG("SSE parse: error parsing event");
                        goto done;
                    }

                    if(done_flag)
                    {
                        /* 收到 finish_reason，表示 LLM 已完成本轮生成。
                         * finish_reason 可能是 "stop"（正常完成）、
                         * "tool_calls"（需要执行工具）等。
                         * 无论哪种，都标志着流式响应的正常终止。 */
                        AGENT_LOG("SSE parse: finish_reason received");
                        done_received = true;
                        rc = 0;
                        goto done;
                    }
                }
                /* 处理 "data:"（不带空格）的情况，某些实现可能省略空格 */
                else if(line_len > 5 && strncmp(line_buf, "data:", 5) == 0)
                {
                    const char *payload = line_buf + 5;
                    /* 跳过可能的前导空格 */
                    while(*payload == ' ')
                    {
                        payload++;
                    }

                    if(strcmp(payload, "[DONE]") == 0)
                    {
                        AGENT_LOG("SSE parse: received [DONE]");
                        done_received = true;
                        rc = 0;
                        goto done;
                    }

                    bool done_flag = false;
                    int err = parse_sse_event(payload, on_chunk, userp,
                                              resp, &done_flag);
                    if(err != 0)
                    {
                        goto done;
                    }
                    if(done_flag)
                    {
                        done_received = true;
                        rc = 0;
                        goto done;
                    }
                }

                /* 重置行缓冲区，准备接收下一行 */
                line_len = 0;
            }
            else if(c != '\r')
            {
                /*
                 * 将非换行符的字符加入行缓冲区。
                 * 如果当前行超过了缓冲区容量，通过 agent_realloc 扩容。
                 * 每次 2 倍增长，不截断、不丢失数据，由 TLSF 管理内存。
                 */
                if(line_len + 1 >= line_cap)
                {
                    size_t new_cap = line_cap * 2;
                    char *tmp = agent_realloc(line_buf, new_cap);
                    if(!tmp)
                    {
                        AGENT_LOG("SSE parse: line buffer realloc failed (%u -> %u)",
                                  (unsigned)line_cap, (unsigned)new_cap);
                        goto done;
                    }
                    line_buf = tmp;
                    line_cap = new_cap;
                }
                line_buf[line_len++] = c;
            }
            /* '\r' 被忽略（SSE 行以 \r\n 或 \n 结尾） */
        }
    }

    /*
     * for 循环结束（EOF），此时检查是否收到了终止事件：
     * - done_received == true: 正常结束，rc 已被设为 0
     * - done_received == false: 异常断开，rc 保持 -1
     *
     * 旧逻辑在这里把 resp 有内容就算成功（rc=0），这会导致：
     * 网络中断时半截回答被持久化到 session，后续 API 调用
     * 因损坏上下文级联失败。现在只有收到 [DONE] 或 finish_reason
     * 才算成功，EOF 无终止事件保持 rc=-1 返回错误。
     */
done:
    agent_free(line_buf);
    agent_free(read_buf);
    return rc;
}

int llm_chat_stream(const char *system_prompt,
                    cclaw_cJSON *messages,
                    const char *tools_json,
                    llm_stream_chunk_cb on_chunk,
                    void *userp,
                    llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if(agent_tlsf_init() != 0)
    {
        AGENT_LOG("llm_chat_stream: agent_tlsf_init failed");
        return -1;
    }

    /* 检查 API key */
    if(s_api_key[0] == '\0')
    {
        AGENT_LOG("llm_chat_stream: missing API key (provider: %s, env: %s)",
                  s_provider ? s_provider->name : "(null)",
                  s_provider ? s_provider->api_key_env : "(null)");
        return -1;
    }

    /* 1. 构建流式请求 JSON（带 "stream": true） */
    char *post_data = build_stream_request(system_prompt, messages, tools_json);
    if(!post_data)
    {
        AGENT_LOG("llm_chat_stream: failed to build stream request");
        return -1;
    }

    AGENT_LOG("llm_chat_stream: calling %s (model: %s, body: %d bytes)",
              s_provider->name, s_model, (int)strlen(post_data));
    llm_log_payload("LLM stream request", post_data);

    /* 2. 构建鉴权 header */
    char auth_value[LLM_API_KEY_MAX_LEN + 32] = {0};
    agent_http_header_t headers[2];
    size_t header_count = 0;

    headers[header_count++] = (agent_http_header_t)
    {
        "Content-Type",
        "application/json"
    };

    if(s_provider->format_auth_header &&
            s_provider->format_auth_header(auth_value, sizeof(auth_value),
                                           s_api_key) != 0)
    {
        AGENT_LOG("llm_chat_stream: failed to format auth header");
        agent_free(post_data);
        return -1;
    }
    if(auth_value[0])
    {
        headers[header_count++] = (agent_http_header_t)
        {
            "Authorization",
            auth_value
        };
    }

    /* 3. 打开 HTTP 流（发送请求，读取响应头） */
    agent_http_stream_t *stream = NULL;
    int status = 0;
    int err = agent_http_stream_open("POST",
                                     llm_api_url(),
                                     headers,
                                     header_count,
                                     post_data,
                                     120,
                                     &stream,
                                     &status);
    agent_free(post_data);

    if(err != 0)
    {
        AGENT_LOG("llm_chat_stream: failed to open stream");
        return -1;
    }

    if(status != 200)
    {
        AGENT_LOG("llm_chat_stream: %s API error %d", s_provider->name, status);
        agent_http_stream_close(stream);
        return -1;
    }

    AGENT_LOG("llm_chat_stream: SSE stream opened, parsing events...");

    /* 4. 解析 SSE 事件流 */
    err = parse_sse_stream(stream, on_chunk, userp, resp);

    /* 5. 关闭流 */
    agent_http_stream_close(stream);

    if(err != 0)
    {
        AGENT_LOG("llm_chat_stream: SSE parse failed, partial text=%d bytes, reasoning=%d bytes",
                  (int)resp->text_len, (int)resp->reasoning_len);
        /* 不再伪装成功：网络中断或 JSON/分块错误产生的部分文本可能不完整，
         * 持久化到 session 会导致后续 API 调用因损坏上下文失败。
         *
         * 返回 -1 告知调用者这不是完整响应。resp 中的部分数据（text、reasoning）
         * 仍可被调用者读取（如 UI 展示），但不应持久化。
         * 调用者在退出作用域前应调用 llm_response_free() 释放。 */
        return -1;
    }

    AGENT_LOG("llm_chat_stream: done, %d bytes text, %d tool calls, stop=%s",
              (int)resp->text_len, resp->call_count,
              resp->tool_use ? "tool_use" : "end_turn");

    return 0;
}
