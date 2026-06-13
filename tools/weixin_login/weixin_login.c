#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cJSON/cJSON.h"

#define DEFAULT_API_BASE        "https://ilinkai.weixin.qq.com"
#define LOGIN_TIMEOUT_SEC       300
#define REQUEST_TIMEOUT_SEC     35
#define LOGIN_MAX_REFRESHES     3

#define QRCODE_MAX_LEN          1024
#define QRCODE_IMG_MAX_LEN      4096
#define STATUS_MAX_LEN          32
#define TOKEN_MAX_LEN           512
#define ID_MAX_LEN              128
#define BASEURL_MAX_LEN         256
#define UIN_MAX_LEN             64

#ifndef WEIXIN_LOGIN_CURL_CMD
#define WEIXIN_LOGIN_CURL_CMD   "curl"
#endif

/*
 * The bundled CClaw cJSON is wired to the agent allocator symbols. This
 * standalone helper keeps those symbols local to libc allocation instead of
 * pulling in the agent TLSF runtime.
 */
void *agent_malloc(size_t size)
{
    return malloc(size);
}

void *agent_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

void agent_free(void *ptr)
{
    free(ptr);
}

typedef struct
{
    char *data;
    size_t len;
    size_t cap;
} text_buf_t;

typedef struct
{
    const char *name;
    const char *value;
} http_header_t;

typedef struct
{
    char qrcode[QRCODE_MAX_LEN];
    char qrcode_img_content[QRCODE_IMG_MAX_LEN];
} login_qrcode_t;

typedef struct
{
    char status[STATUS_MAX_LEN];
    char bot_token[TOKEN_MAX_LEN];
    char ilink_bot_id[ID_MAX_LEN];
    char ilink_user_id[ID_MAX_LEN];
    char baseurl[BASEURL_MAX_LEN];
} login_status_t;

static int text_buf_init(text_buf_t *buf, size_t initial_cap)
{
    buf->data = (char *)calloc(1, initial_cap);
    if(!buf->data)
    {
        return -1;
    }
    buf->len = 0;
    buf->cap = initial_cap;
    return 0;
}

static void text_buf_free(text_buf_t *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int text_buf_reserve(text_buf_t *buf, size_t extra)
{
    if(buf->len + extra + 1 <= buf->cap)
    {
        return 0;
    }

    size_t new_cap = buf->cap ? buf->cap : 256;
    while(buf->len + extra + 1 > new_cap)
    {
        new_cap *= 2;
    }

    char *tmp = (char *)realloc(buf->data, new_cap);
    if(!tmp)
    {
        return -1;
    }

    buf->data = tmp;
    buf->cap = new_cap;
    return 0;
}

static int text_buf_append_n(text_buf_t *buf, const char *text, size_t len)
{
    if(text_buf_reserve(buf, len) != 0)
    {
        return -1;
    }
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 0;
}

static int text_buf_append(text_buf_t *buf, const char *text)
{
    return text_buf_append_n(buf, text, strlen(text));
}

static int text_buf_appendf(text_buf_t *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if(needed < 0)
    {
        va_end(ap);
        return -1;
    }
    if(text_buf_reserve(buf, (size_t)needed) != 0)
    {
        va_end(ap);
        return -1;
    }

    vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap);
    va_end(ap);
    buf->len += (size_t)needed;
    return 0;
}

static char *shell_quote(const char *value)
{
    size_t len = 2;
    for(const char *p = value; p && *p; p++)
    {
        len += (*p == '\'') ? 4 : 1;
    }

    char *out = (char *)malloc(len + 1);
    if(!out)
    {
        return NULL;
    }

    char *w = out;
    *w++ = '\'';
    for(const char *p = value; p && *p; p++)
    {
        if(*p == '\'')
        {
            memcpy(w, "'\\''", 4);
            w += 4;
        }
        else
        {
            *w++ = *p;
        }
    }
    *w++ = '\'';
    *w = '\0';
    return out;
}

static int run_command_capture(const char *cmd, char **out_text)
{
    text_buf_t out;
    if(text_buf_init(&out, 4096) != 0)
    {
        return -1;
    }

    FILE *pipe = popen(cmd, "r");
    if(!pipe)
    {
        text_buf_free(&out);
        return -1;
    }

    char chunk[4096];
    size_t n;
    while((n = fread(chunk, 1, sizeof(chunk), pipe)) > 0)
    {
        if(text_buf_append_n(&out, chunk, n) != 0)
        {
            pclose(pipe);
            text_buf_free(&out);
            return -1;
        }
    }

    int rc = pclose(pipe);
    if(rc != 0)
    {
        text_buf_free(&out);
        return -1;
    }

    *out_text = out.data;
    return 0;
}

static char *url_escape(const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t len = 0;
    for(const unsigned char *p = (const unsigned char *)value; *p; p++)
    {
        len += (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') ? 1 : 3;
    }

    char *out = (char *)malloc(len + 1);
    if(!out)
    {
        return NULL;
    }

    char *w = out;
    for(const unsigned char *p = (const unsigned char *)value; *p; p++)
    {
        if(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
        {
            *w++ = (char) * p;
        }
        else
        {
            *w++ = '%';
            *w++ = hex[*p >> 4];
            *w++ = hex[*p & 0x0f];
        }
    }
    *w = '\0';
    return out;
}

static int curl_get(const char *url,
                    const http_header_t *headers,
                    size_t header_count,
                    long timeout_sec,
                    char **response)
{
    text_buf_t cmd;
    if(text_buf_init(&cmd, 512) != 0)
    {
        return -1;
    }

    char *quoted_url = shell_quote(url);
    if(!quoted_url)
    {
        text_buf_free(&cmd);
        return -1;
    }

    int rc = 0;
    rc |= text_buf_append(&cmd, WEIXIN_LOGIN_CURL_CMD);
    rc |= text_buf_appendf(&cmd, " -fsS --max-time %ld", timeout_sec);

    for(size_t i = 0; i < header_count && rc == 0; i++)
    {
        char header_line[1024];
        snprintf(header_line, sizeof(header_line), "%s: %s", headers[i].name, headers[i].value);
        char *quoted_header = shell_quote(header_line);
        if(!quoted_header)
        {
            rc = -1;
            break;
        }
        rc |= text_buf_append(&cmd, " -H ");
        rc |= text_buf_append(&cmd, quoted_header);
        free(quoted_header);
    }

    rc |= text_buf_append(&cmd, " ");
    rc |= text_buf_append(&cmd, quoted_url);
    free(quoted_url);

    if(rc == 0)
    {
        rc = run_command_capture(cmd.data, response);
    }

    text_buf_free(&cmd);
    return rc;
}

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

static void json_copy_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    if(dst_size > 0)
    {
        dst[0] = '\0';
    }
    if(!root || !key || !dst || dst_size == 0)
    {
        return;
    }

    cJSON *item = cJSON_GetObjectItem(root, key);
    if(cJSON_IsString(item) && item->valuestring)
    {
        snprintf(dst, dst_size, "%s", item->valuestring);
    }
}

static int parse_login_qrcode_json(const char *json, login_qrcode_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if(!root)
    {
        return -1;
    }

    json_copy_string(root, "qrcode", out->qrcode, sizeof(out->qrcode));
    json_copy_string(root, "qrcode_img_content",
                     out->qrcode_img_content, sizeof(out->qrcode_img_content));
    cJSON_Delete(root);
    return (out->qrcode[0] && out->qrcode_img_content[0]) ? 0 : -1;
}

static int parse_login_status_json(const char *json, login_status_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if(!root)
    {
        return -1;
    }

    json_copy_string(root, "status", out->status, sizeof(out->status));
    json_copy_string(root, "bot_token", out->bot_token, sizeof(out->bot_token));
    json_copy_string(root, "ilink_bot_id", out->ilink_bot_id, sizeof(out->ilink_bot_id));
    json_copy_string(root, "ilink_user_id", out->ilink_user_id, sizeof(out->ilink_user_id));
    json_copy_string(root, "baseurl", out->baseurl, sizeof(out->baseurl));
    cJSON_Delete(root);
    return out->status[0] ? 0 : -1;
}

static int fetch_qrcode(const char *api_base, login_qrcode_t *out)
{
    char url[512];
    char uin[UIN_MAX_LEN];
    generate_uin(uin, sizeof(uin));
    snprintf(url, sizeof(url), "%s/ilink/bot/get_bot_qrcode?bot_type=3", api_base);

    http_header_t headers[] =
    {
        {"X-WECHAT-UIN", uin},
    };

    char *response = NULL;
    if(curl_get(url, headers, 1, REQUEST_TIMEOUT_SEC, &response) != 0)
    {
        fprintf(stderr, "Failed to request Weixin login QR code.\n");
        return -1;
    }

    int rc = parse_login_qrcode_json(response, out);
    if(rc != 0)
    {
        fprintf(stderr, "Failed to parse QR response: %.300s\n", response);
    }
    free(response);
    return rc;
}

static int poll_status(const char *api_base, const char *qrcode, login_status_t *out)
{
    char *escaped_qrcode = url_escape(qrcode);
    if(!escaped_qrcode)
    {
        return -1;
    }

    char url[QRCODE_MAX_LEN + 512];
    snprintf(url, sizeof(url), "%s/ilink/bot/get_qrcode_status?qrcode=%s",
             api_base, escaped_qrcode);
    free(escaped_qrcode);

    char uin[UIN_MAX_LEN];
    generate_uin(uin, sizeof(uin));
    http_header_t headers[] =
    {
        {"iLink-App-ClientVersion", "1"},
        {"X-WECHAT-UIN", uin},
    };

    char *response = NULL;
    if(curl_get(url, headers, 2, REQUEST_TIMEOUT_SEC, &response) != 0)
    {
        fprintf(stderr, "Failed to request Weixin login status.\n");
        return -1;
    }

    int rc = parse_login_status_json(response, out);
    if(rc != 0)
    {
        fprintf(stderr, "Failed to parse status response: %.300s\n", response);
    }
    free(response);
    return rc;
}

static void print_usage(const char *argv0)
{
    printf("Usage: %s [--env-file PATH] [--timeout SECONDS] [--api-base URL] [--qrencode]\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  --env-file PATH   Save WEIXIN_ENABLED and WEIXIN_BOT_TOKEN to PATH.\n");
    printf("  --timeout SEC     Total wait time for each QR code. Default: %d.\n", LOGIN_TIMEOUT_SEC);
    printf("  --api-base URL    iLink API base URL. Default: %s.\n", DEFAULT_API_BASE);
    printf("  --qrencode        Try rendering QR in terminal via external qrencode command.\n");
    printf("  --help            Show this help.\n");
}

static int save_env_file(const char *path, const char *token)
{
    FILE *f = fopen(path, "w");
    if(!f)
    {
        fprintf(stderr, "Failed to open env file for writing: %s\n", path);
        return -1;
    }

    fprintf(f, "WEIXIN_ENABLED=1\n");
    fprintf(f, "WEIXIN_BOT_TOKEN=%s\n", token);
    fclose(f);
    return 0;
}

static int render_with_qrencode(const char *content)
{
    FILE *pipe = popen("qrencode -t ANSIUTF8 -o -", "w");
    if(!pipe)
    {
        return -1;
    }

    fwrite(content, 1, strlen(content), pipe);
    int rc = pclose(pipe);
    return rc == 0 ? 0 : -1;
}

static void print_qrcode(const login_qrcode_t *qr, bool use_qrencode)
{
    printf("\nWeixin scan login\n");
    printf("=================\n\n");

    if(use_qrencode)
    {
        if(render_with_qrencode(qr->qrcode_img_content) == 0)
        {
            printf("\n");
            return;
        }
        printf("qrencode is not available or failed; printing raw QR content instead.\n\n");
    }

    printf("Copy this content into a QR generator, then scan it with WeChat:\n\n");
    printf("%s\n\n", qr->qrcode_img_content);
    printf("Tip: install qrencode and run with --qrencode to render a terminal QR code.\n\n");
}

int main(int argc, char **argv)
{
    const char *api_base = DEFAULT_API_BASE;
    const char *env_file = NULL;
    int timeout_sec = LOGIN_TIMEOUT_SEC;
    bool use_qrencode = false;

    srand((unsigned int)time(NULL));

    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if(strcmp(argv[i], "--env-file") == 0)
        {
            if(i + 1 >= argc)
            {
                fprintf(stderr, "--env-file requires a path\n");
                return 2;
            }
            env_file = argv[++i];
        }
        else if(strcmp(argv[i], "--timeout") == 0)
        {
            if(i + 1 >= argc)
            {
                fprintf(stderr, "--timeout requires seconds\n");
                return 2;
            }
            timeout_sec = atoi(argv[++i]);
            if(timeout_sec <= 0)
            {
                fprintf(stderr, "--timeout must be positive\n");
                return 2;
            }
        }
        else if(strcmp(argv[i], "--api-base") == 0)
        {
            if(i + 1 >= argc)
            {
                fprintf(stderr, "--api-base requires a URL\n");
                return 2;
            }
            api_base = argv[++i];
        }
        else if(strcmp(argv[i], "--qrencode") == 0)
        {
            use_qrencode = true;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    for(int refresh = 0; refresh < LOGIN_MAX_REFRESHES; refresh++)
    {
        login_qrcode_t qr;
        if(fetch_qrcode(api_base, &qr) != 0)
        {
            return 1;
        }

        print_qrcode(&qr, use_qrencode);
        printf("Waiting for scan confirmation, timeout: %d seconds...\n", timeout_sec);

        time_t started_at = time(NULL);
        char last_status[STATUS_MAX_LEN] = {0};

        while((int)(time(NULL) - started_at) < timeout_sec)
        {
            login_status_t status;
            if(poll_status(api_base, qr.qrcode, &status) != 0)
            {
                return 1;
            }

            if(strcmp(status.status, last_status) != 0)
            {
                printf("Status: %s\n", status.status);
                strncpy(last_status, status.status, sizeof(last_status) - 1);
            }

            if(strcmp(status.status, "confirmed") == 0)
            {
                if(!status.bot_token[0])
                {
                    fprintf(stderr, "Login confirmed, but bot_token is empty.\n");
                    return 1;
                }

                printf("\nWeixin login success.\n");
                printf("WEIXIN_ENABLED=1\n");
                printf("WEIXIN_BOT_TOKEN=%s\n", status.bot_token);

                if(status.ilink_bot_id[0])
                {
                    printf("ilink_bot_id=%s\n", status.ilink_bot_id);
                }
                if(status.ilink_user_id[0])
                {
                    printf("ilink_user_id=%s\n", status.ilink_user_id);
                }
                if(status.baseurl[0])
                {
                    printf("baseurl=%s\n", status.baseurl);
                }

                if(env_file)
                {
                    if(save_env_file(env_file, status.bot_token) != 0)
                    {
                        return 1;
                    }
                    printf("\nSaved env file: %s\n", env_file);
                }

                return 0;
            }

            if(strcmp(status.status, "expired") == 0)
            {
                printf("QR code expired, refreshing...\n");
                break;
            }

            sleep(1);
        }

        printf("QR code wait timed out.\n");
    }

    fprintf(stderr, "Weixin login failed after %d QR refresh attempts.\n", LOGIN_MAX_REFRESHES);
    return 1;
}
