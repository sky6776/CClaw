#define _POSIX_C_SOURCE 200809L

#include "weixin_channel.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "agent_http.h"
#include "agent_tlsf.h"
#include "cJSON/cJSON.h"

#define WEIXIN_TOKEN_MAX_LEN     512
#define WEIXIN_ALLOW_MAX_LEN     1024
#define WEIXIN_BUF_INIT_SIZE     (32 * 1024)
#define WEIXIN_CLIENT_ID_LEN     64
#define WEIXIN_UIN_LEN           64

static char s_token[WEIXIN_TOKEN_MAX_LEN] = {0};
static char s_allow_from[WEIXIN_ALLOW_MAX_LEN] = {0};
static bool weixin_enabled = false;
static pthread_t weixin_poll_thread = 0;

typedef struct
{
    char *data;
    size_t len;
    size_t cap;
} weixin_buf_t;

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

static bool env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    if(!value || !value[0])
    {
        return false;
    }

    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0;
}

static bool append_text(char **dst, size_t *dst_len, const char *src)
{
    if(!src || !src[0])
    {
        return true;
    }

    size_t src_len = strlen(src);
    size_t sep_len = (*dst_len > 0) ? 1 : 0;
    char *tmp = agent_realloc(*dst, *dst_len + sep_len + src_len + 1);
    if(!tmp)
    {
        return false;
    }

    if(sep_len)
    {
        tmp[*dst_len] = '\n';
        *dst_len += 1;
    }
    memcpy(tmp + *dst_len, src, src_len);
    *dst_len += src_len;
    tmp[*dst_len] = '\0';
    *dst = tmp;
    return true;
}

static int weixin_buf_init(weixin_buf_t *buf, size_t initial_cap)
{
    buf->data = agent_calloc(1, initial_cap);
    if(!buf->data)
    {
        return -1;
    }
    buf->len = 0;
    buf->cap = initial_cap;
    return 0;
}

static void weixin_buf_free(weixin_buf_t *buf)
{
    agent_free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int weixin_body_cb(const void *contents, size_t realsize, void *userp)
{
    weixin_buf_t *buf = (weixin_buf_t *)userp;

    while(buf->len + realsize >= buf->cap)
    {
        size_t new_cap = buf->cap * 2;
        char *tmp = agent_realloc(buf->data, new_cap);
        if(!tmp)
        {
            return -1;
        }
        buf->data = tmp;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, contents, realsize);
    buf->len += realsize;
    buf->data[buf->len] = '\0';
    return 0;
}

/*
 * 这里实现一个很小的 base64 编码器，只用于这个 header，避免再引入额外依赖。
 */
static void base64_encode_small(const unsigned char *input, size_t len,
                                char *out, size_t out_size)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t off = 0;
    size_t i = 0;

    if(!out || out_size == 0)
    {
        return;
    }

    while(i < len && off + 4 < out_size)
    {
        size_t start = i;
        unsigned int a = input[i++];
        unsigned int b = (i < len) ? input[i++] : 0;
        unsigned int c = (i < len) ? input[i++] : 0;
        size_t remain = len - start;

        out[off++] = table[(a >> 2) & 0x3f];
        out[off++] = table[((a & 0x03) << 4) | ((b >> 4) & 0x0f)];
        out[off++] = (remain > 1) ? table[((b & 0x0f) << 2) | ((c >> 6) & 0x03)] : '=';
        out[off++] = (remain > 2) ? table[c & 0x3f] : '=';
    }
    out[off] = '\0';
}

static void generate_uin(char *out, size_t out_size)
{
    char number[32];
    unsigned int rnd = ((unsigned int)rand() << 16) ^ (unsigned int)time(NULL);
    snprintf(number, sizeof(number), "%u", rnd);
    base64_encode_small((const unsigned char *)number, strlen(number), out, out_size);
}

static void generate_client_id(char *out, size_t out_size)
{
    unsigned int rnd = ((unsigned int)rand() << 16) ^ (unsigned int)time(NULL);
    snprintf(out, out_size, "wcc-%ld-%08x", (long)time(NULL), rnd);
}

static bool is_sender_allowed_with_list(const char *allow_list, const char *sender)
{
    if(!sender || !sender[0])
    {
        return false;
    }
    if(!allow_list || !allow_list[0])
    {
        return true;
    }

    char copy[WEIXIN_ALLOW_MAX_LEN];
    safe_copy(copy, sizeof(copy), allow_list);

    char *token = strtok(copy, ",");
    while(token)
    {
        while(*token == ' ' || *token == '\t')
        {
            token++;
        }

        char *end = token + strlen(token);
        while(end > token && (end[-1] == ' ' || end[-1] == '\t' ||
                              end[-1] == '\n' || end[-1] == '\r'))
        {
            end--;
        }
        *end = '\0';

        if(strcmp(token, sender) == 0)
        {
            return true;
        }

        token = strtok(NULL, ",");
    }

    return false;
}

static bool is_sender_allowed(const char *sender)
{
    return is_sender_allowed_with_list(s_allow_from, sender);
}

/*
 * UTF-8 安全切分：微信发送接口对单条文本长度有限制，长回复按字节切块。
 * C 字符串没有内建 UTF-8 边界信息，所以这里只回退掉 continuation byte，
 * 确保不会把一个多字节中文字符切碎。
 */
static size_t safe_split_len(const char *text, size_t max_bytes)
{
    size_t len = strlen(text);
    size_t split = max_bytes;
    unsigned char first = (unsigned char)text[0];
    size_t first_char_len = 1;

    if(len <= max_bytes)
    {
        return len;
    }

    if(split == 0)
    {
        return 0;
    }

    if((first & 0x80) == 0)
    {
        first_char_len = 1;
    }
    else if((first & 0xe0) == 0xc0)
    {
        first_char_len = 2;
    }
    else if((first & 0xf0) == 0xe0)
    {
        first_char_len = 3;
    }
    else if((first & 0xf8) == 0xf0)
    {
        first_char_len = 4;
    }

    while(split > 0 && (((unsigned char)text[split]) & 0xc0) == 0x80)
    {
        split--;
    }

    for(size_t i = split; i > 0; i--)
    {
        if(text[i - 1] == '\n')
        {
            return i;
        }
    }

    return split > 0 ? split : first_char_len;
}

static char *build_getupdates_body(const char *sync_buf)
{
    cJSON *root = cJSON_CreateObject();
    if(!root)
    {
        return NULL;
    }
    cJSON_AddStringToObject(root, "get_updates_buf", sync_buf ? sync_buf : "");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static char *build_send_body(const char *to_user_id,
                             const char *text,
                             const char *context_token)
{
    char client_id[WEIXIN_CLIENT_ID_LEN];
    generate_client_id(client_id, sizeof(client_id));

    cJSON *root = cJSON_CreateObject();
    cJSON *msg = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON *text_item = cJSON_CreateObject();

    if(!root || !msg || !items || !item || !text_item)
    {
        cJSON_Delete(root);
        cJSON_Delete(msg);
        cJSON_Delete(items);
        cJSON_Delete(item);
        cJSON_Delete(text_item);
        return NULL;
    }

    cJSON_AddStringToObject(msg, "from_user_id", "");
    cJSON_AddStringToObject(msg, "to_user_id", to_user_id ? to_user_id : "");
    cJSON_AddStringToObject(msg, "client_id", client_id);
    cJSON_AddNumberToObject(msg, "message_type", 2);
    cJSON_AddNumberToObject(msg, "message_state", 2);
    cJSON_AddStringToObject(msg, "context_token", context_token ? context_token : "");

    cJSON_AddNumberToObject(item, "type", 1);
    cJSON_AddStringToObject(text_item, "text", text ? text : "");
    cJSON_AddItemToObject(item, "text_item", text_item);
    cJSON_AddItemToArray(items, item);
    cJSON_AddItemToObject(msg, "item_list", items);
    cJSON_AddItemToObject(root, "msg", msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/*
 * 从 iLink 消息对象中抽取 agent 可处理的纯文本。
 * - type=1 使用 text_item.text；
 * - type=3 使用 voice_item.text，并加上语音标记；
 * - ref_msg.title 作为引用内容追加。
 */
static char *extract_message_content(cJSON *msg)
{
    char *content = NULL;
    size_t content_len = 0;
    cJSON *items = cJSON_GetObjectItem(msg, "item_list");
    if(!items || !cJSON_IsArray(items))
    {
        return NULL;
    }

    cJSON *item;
    cJSON_ArrayForEach(item, items)
    {
        cJSON *type = cJSON_GetObjectItem(item, "type");
        int item_type = cJSON_IsNumber(type) ? type->valueint : 0;

        if(item_type == 1)
        {
            cJSON *text_item = cJSON_GetObjectItem(item, "text_item");
            cJSON *text = text_item ? cJSON_GetObjectItem(text_item, "text") : NULL;
            if(cJSON_IsString(text) && text->valuestring[0])
            {
                append_text(&content, &content_len, text->valuestring);
            }
        }
        else if(item_type == 3)
        {
            cJSON *voice_item = cJSON_GetObjectItem(item, "voice_item");
            cJSON *text = voice_item ? cJSON_GetObjectItem(voice_item, "text") : NULL;
            if(cJSON_IsString(text) && text->valuestring[0])
            {
                char voice_buf[1024];
                snprintf(voice_buf, sizeof(voice_buf), "[语音] %s", text->valuestring);
                append_text(&content, &content_len, voice_buf);
            }
        }

        cJSON *ref_msg = cJSON_GetObjectItem(item, "ref_msg");
        cJSON *title = ref_msg ? cJSON_GetObjectItem(ref_msg, "title") : NULL;
        if(cJSON_IsString(title) && title->valuestring[0])
        {
            char ref_buf[1024];
            snprintf(ref_buf, sizeof(ref_buf), "[引用] %s", title->valuestring);
            append_text(&content, &content_len, ref_buf);
        }
    }

    return content;
}

static int push_weixin_message(cJSON *msg)
{
    cJSON *message_type = cJSON_GetObjectItem(msg, "message_type");
    if(!cJSON_IsNumber(message_type) || message_type->valueint != 1)
    {
        return 0;
    }

    cJSON *from = cJSON_GetObjectItem(msg, "from_user_id");
    if(!cJSON_IsString(from) || !from->valuestring[0])
    {
        return 0;
    }

    if(!is_sender_allowed(from->valuestring))
    {
        AGENT_LOG("Weixin: ignored message from non-allowlisted user %s", from->valuestring);
        return 0;
    }

    char *content = extract_message_content(msg);
    if(!content || !content[0])
    {
        agent_free(content);
        return 0;
    }

    bus_msg_t inbound;
    /*
     * message_bus_push_inbound() 会同步复制 content 到内部 ring buffer。
     * 因此 push 返回后可以安全 free extract_message_content() 申请的临时字符串。
     */
    bus_msg_set(&inbound, CHANNEL_WEIXIN, from->valuestring, content);

    AGENT_LOG("Weixin: received message from %s (%u bytes)",
              inbound.chat_id, (unsigned)inbound.content_len);
    int err = message_bus_push_inbound(&inbound);
    agent_free(content);
    return err;
}

static int handle_updates_response(const char *json, char *sync_buf, size_t sync_buf_size)
{
    cJSON *root = cJSON_Parse(json);
    if(!root)
    {
        AGENT_LOG("Weixin: failed to parse getupdates response");
        return -1;
    }

    cJSON *ret = cJSON_GetObjectItem(root, "ret");
    cJSON *errcode = cJSON_GetObjectItem(root, "errcode");
    int ret_code = cJSON_IsNumber(ret) ? ret->valueint : 0;
    int err_code = cJSON_IsNumber(errcode) ? errcode->valueint : 0;
    if(ret_code != 0 || err_code != 0)
    {
        AGENT_LOG("Weixin: getupdates returned ret=%d errcode=%d", ret_code, err_code);
        cJSON_Delete(root);
        return -1;
    }

    cJSON *new_buf = cJSON_GetObjectItem(root, "get_updates_buf");
    if(cJSON_IsString(new_buf) && new_buf->valuestring[0])
    {
        safe_copy(sync_buf, sync_buf_size, new_buf->valuestring);
    }

    cJSON *msgs = cJSON_GetObjectItem(root, "msgs");
    if(msgs && cJSON_IsArray(msgs))
    {
        cJSON *msg;
        cJSON_ArrayForEach(msg, msgs)
        {
            push_weixin_message(msg);
        }
    }

    cJSON_Delete(root);
    return 0;
}

#ifndef WEIXIN_CHANNEL_NO_HTTP
static int http_post_json(const char *url, const char *body,
                          long timeout_sec, weixin_buf_t *out, int *status)
{
    char auth[WEIXIN_TOKEN_MAX_LEN + 32];
    char uin[WEIXIN_UIN_LEN];
    agent_http_header_t headers[4];
    size_t header_count = 0;

    snprintf(auth, sizeof(auth), "Bearer %s", s_token);
    generate_uin(uin, sizeof(uin));

    headers[header_count++] = (agent_http_header_t)
    {
        "Content-Type",
        "application/json"
    };
    headers[header_count++] = (agent_http_header_t)
    {
        "Authorization",
        auth
    };
    headers[header_count++] = (agent_http_header_t)
    {
        "AuthorizationType",
        "ilink_bot_token"
    };
    headers[header_count++] = (agent_http_header_t)
    {
        "X-WECHAT-UIN",
        uin
    };

    return agent_http_request("POST",
                              url,
                              headers,
                              header_count,
                              body,
                              timeout_sec,
                              weixin_body_cb,
                              out,
                              status);
}
#endif

static int weixin_getupdates(char *sync_buf, size_t sync_buf_size)
{
#ifdef WEIXIN_CHANNEL_NO_HTTP
    (void)sync_buf;
    (void)sync_buf_size;
    return -1;
#else
    char url[256];
    snprintf(url, sizeof(url), "%s/ilink/bot/getupdates", AGENT_WEIXIN_API_BASE);

    char *body = build_getupdates_body(sync_buf);
    if(!body)
    {
        return -1;
    }

    weixin_buf_t resp;
    if(weixin_buf_init(&resp, WEIXIN_BUF_INIT_SIZE) != 0)
    {
        agent_free(body);
        return -1;
    }

    int status = 0;
    int err = http_post_json(url, body, AGENT_WEIXIN_POLL_TIMEOUT_SEC, &resp, &status);
    agent_free(body);

    if(err == 0 && status >= 200 && status < 300)
    {
        err = handle_updates_response(resp.data, sync_buf, sync_buf_size);
    }
    else if(err != 0)
    {
        AGENT_LOG("Weixin: getupdates request failed status=%d %.200s",
                  status, resp.data ? resp.data : "");
        err = -1;
    }
    else
    {
        AGENT_LOG("Weixin: getupdates HTTP %d %.200s", status, resp.data ? resp.data : "");
        err = -1;
    }

    weixin_buf_free(&resp);
    return err;
#endif
}

static void *weixin_poll_task(void *arg)
{
    (void)arg;

    char sync_buf[1024] = {0};
    unsigned int failures = 0;

    AGENT_LOG("Weixin: polling started");
    while(weixin_enabled)
    {
        if(weixin_getupdates(sync_buf, sizeof(sync_buf)) == 0)
        {
            failures = 0;
        }
        else
        {
            failures++;
            if(failures >= AGENT_WEIXIN_MAX_FAILURES)
            {
                AGENT_LOG("Weixin: backing off after %u failures", failures);
                sleep(AGENT_WEIXIN_BACKOFF_SEC);
                failures = 0;
            }
            else
            {
                sleep(AGENT_WEIXIN_RETRY_SEC);
            }
        }
    }

    AGENT_LOG("Weixin: polling stopped");
    return NULL;
}

int weixin_channel_init(void)
{
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }

    srand((unsigned int)time(NULL));

    if(AGENT_WEIXIN_BOT_TOKEN[0])
    {
        safe_copy(s_token, sizeof(s_token), AGENT_WEIXIN_BOT_TOKEN);
    }
    else
    {
        const char *env_token = getenv(AGENT_WEIXIN_BOT_TOKEN_ENV);
        if(env_token && env_token[0])
        {
            safe_copy(s_token, sizeof(s_token), env_token);
        }
    }

    if(AGENT_WEIXIN_ALLOW_FROM[0])
    {
        safe_copy(s_allow_from, sizeof(s_allow_from), AGENT_WEIXIN_ALLOW_FROM);
    }
    else
    {
        const char *env_allow = getenv(AGENT_WEIXIN_ALLOW_FROM_ENV);
        if(env_allow && env_allow[0])
        {
            safe_copy(s_allow_from, sizeof(s_allow_from), env_allow);
        }
    }

    weixin_enabled = (AGENT_WEIXIN_ENABLED != 0) || env_flag_enabled(AGENT_WEIXIN_ENABLED_ENV);

    if(!weixin_enabled)
    {
        AGENT_LOG("Weixin channel disabled. Set %s=1 to enable.", AGENT_WEIXIN_ENABLED_ENV);
        return 0;
    }
    if(!s_token[0])
    {
        AGENT_LOG("Weixin channel enabled but token is empty. Set %s.", AGENT_WEIXIN_BOT_TOKEN_ENV);
        weixin_enabled = false;
        return -1;
    }

    AGENT_LOG("Weixin channel initialized%s",
              s_allow_from[0] ? " with allowlist" : "");
    return 0;
}

int weixin_channel_start(void)
{
    if(!weixin_enabled)
    {
        return 0;
    }
    if(weixin_poll_thread != 0)
    {
        return 0;
    }

    if(pthread_create(&weixin_poll_thread, NULL, weixin_poll_task, NULL) != 0)
    {
        AGENT_LOG("Weixin: failed to create polling thread");
        weixin_poll_thread = 0;
        return -1;
    }

    return 0;
}

int weixin_channel_is_enabled(void)
{
    return weixin_enabled ? 1 : 0;
}

int weixin_channel_send_message(const char *chat_id, const char *text)
{
#ifdef WEIXIN_CHANNEL_NO_HTTP
    (void)chat_id;
    (void)text;
    return -1;
#else
    if(!weixin_enabled || !s_token[0])
    {
        AGENT_LOG("Weixin: send skipped, channel disabled or token empty");
        return -1;
    }
    if(agent_tlsf_init() != 0)
    {
        return -1;
    }
    if(!chat_id || !chat_id[0] || !text)
    {
        return -1;
    }

    const char *remaining = text;
    while(*remaining)
    {
        size_t chunk_len = safe_split_len(remaining, AGENT_WEIXIN_SEND_CHUNK_BYTES);
        char *chunk = agent_calloc(1, chunk_len + 1);
        if(!chunk)
        {
            return -1;
        }
        memcpy(chunk, remaining, chunk_len);

        char *body = build_send_body(chat_id, chunk, "");
        agent_free(chunk);
        if(!body)
        {
            return -1;
        }

        char url[256];
        snprintf(url, sizeof(url), "%s/ilink/bot/sendmessage", AGENT_WEIXIN_API_BASE);

        weixin_buf_t resp;
        if(weixin_buf_init(&resp, WEIXIN_BUF_INIT_SIZE) != 0)
        {
            agent_free(body);
            return -1;
        }

        int status = 0;
        int err = http_post_json(url, body, 15, &resp, &status);
        agent_free(body);

        if(err != 0 || status < 200 || status >= 300)
        {
            if(err != 0)
            {
                AGENT_LOG("Weixin: sendmessage request failed status=%d %.200s",
                          status, resp.data ? resp.data : "");
            }
            else
            {
                AGENT_LOG("Weixin: sendmessage HTTP %d %.200s",
                          status, resp.data ? resp.data : "");
            }
            weixin_buf_free(&resp);
            return -1;
        }

        weixin_buf_free(&resp);
        remaining += chunk_len;
    }

    return 0;
#endif
}
