#ifndef __AGENT_HTTP_H__
#define __AGENT_HTTP_H__

#include <stddef.h>

/* ── HTTP 请求头键值对 ──────────────────────────────────────────── */
typedef struct
{
    const char *name;
    const char *value;
} agent_http_header_t;

/* ── 响应体回调：在 agent_http_request() 完成后对完整响应体调用 ─── */
typedef int (*agent_http_body_cb)(const void *data, size_t len, void *userp);

/**
 * 发送一次完整的 HTTP 请求（非流式）。
 *
 * 整个响应体会被缓冲在内存中，请求完成后再调用 body_cb。
 * 适用于普通的 JSON API 调用，不适用于 SSE 流式场景。
 */
int agent_http_request(const char *method,
                       const char *url,
                       const agent_http_header_t *headers,
                       size_t header_count,
                       const char *body,
                       long timeout_sec,
                       agent_http_body_cb body_cb,
                       void *userp,
                       int *out_status);

/**
 * URL 编码（百分号编码）。
 * 返回值需要调用者 agent_free() 释放。
 */
char *agent_http_url_escape(const char *value);

/* ── 流式 HTTP API ──────────────────────────────────────────────── *
 *
 * 适用于 SSE (Server-Sent Events) 等"请求-持续读取"场景。
 *
 * 典型用法：
 *   1. agent_http_stream_open()  → 建立连接、发送请求、读取响应头
 *   2. 循环调用 agent_http_stream_read() → 逐块读取响应体
 *   3. agent_http_stream_close() → 关闭连接、释放资源
 *
 * stream_open 成功后，响应头已解析完毕（状态码通过 out_status 返回），
 * 后续 stream_read 只返回响应体数据，不包含 HTTP 头部。
 */

/* 不透明流句柄，具体字段在 .c 内部定义 */
typedef struct agent_http_stream agent_http_stream_t;

/**
 * 打开流式 HTTP 连接。
 *
 * 内部流程：解析 URL → TCP+TLS 连接 → 发送请求 → 读取并解析响应头。
 * 成功后 *out_stream 指向新分配的流句柄，调用者最终需要
 * 调用 agent_http_stream_close() 释放。
 *
 * @param method       HTTP 方法（"POST" / "GET" 等）
 * @param url          完整 URL
 * @param headers      自定义请求头数组
 * @param header_count 请求头数量
 * @param body         请求体（POST 时使用，GET 时传 NULL）
 * @param timeout_sec  超时秒数（≤0 使用默认 30 秒）
 * @param out_stream   [out] 成功时返回流句柄
 * @param out_status   [out] HTTP 状态码（如 200）
 * @return 0 成功，-1 失败
 */
int agent_http_stream_open(const char *method,
                           const char *url,
                           const agent_http_header_t *headers,
                           size_t header_count,
                           const char *body,
                           long timeout_sec,
                           agent_http_stream_t **out_stream,
                           int *out_status);

/**
 * 从流中读取一块响应体数据。
 *
 * 对于 chunked 传输编码，自动解码分块帧，只返回净荷数据。
 * 对于非 chunked 响应，直接从 socket 读取。
 *
 * @param stream    流句柄（由 stream_open 返回）
 * @param buf       接收缓冲区
 * @param buf_size  缓冲区大小
 * @return >0 读取的字节数，0 表示 EOF（服务端关闭），<0 表示错误
 */
int agent_http_stream_read(agent_http_stream_t *stream,
                           void *buf, size_t buf_size);

/**
 * 关闭流并释放所有资源。
 *
 * 关闭 TLS 连接、TCP socket，释放流句柄内存。
 * 调用后 stream 指针失效，不可再使用。
 */
void agent_http_stream_close(agent_http_stream_t *stream);

#endif
