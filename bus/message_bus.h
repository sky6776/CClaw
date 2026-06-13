#ifndef __MOD_MESSAGE_BUS_H__
#define __MOD_MESSAGE_BUS_H__

#include <stdint.h>
#include <string.h>

#define CHANNEL_WEBSOCKET  "websocket"
#define CHANNEL_CLI        "cli"
#define CHANNEL_SYSTEM     "system"
#define CHANNEL_TELEGRAM   "telegram"
#define CHANNEL_WEIXIN     "weixin"

/* 通道名和会话 ID 仍然放在消息头里，便于消费者不解析正文即可路由。 */
#define MESSAGE_BUS_CHANNEL_MAX         16U
#define MESSAGE_BUS_CHAT_ID_MAX         96U

#ifndef MESSAGE_BUS_QUEUE_BYTES
/* 每个方向一块预分配 ring buffer，不使用 malloc/free。 */
#define MESSAGE_BUS_QUEUE_BYTES         (1024U * 1024U)
#endif

#ifndef MESSAGE_BUS_MAX_CONTENT_BYTES
/* 单条正文最大值需要小于队列容量，留出消息头和环形回绕 padding 空间。 */
#define MESSAGE_BUS_MAX_CONTENT_BYTES   (256U * 1024U)
#endif

/*
 * bus_msg_t 是 message_bus 的公开消息视图。
 *
 * push 时：
 *   - 调用方填写 channel/chat_id/content/content_len；
 *   - message_bus 会把正文复制进内部 ring buffer；
 *   - push 返回后，调用方可以立即释放或复用原始 content。
 *
 * pop 时：
 *   - message_bus 填写 channel/chat_id/content/content_len；
 *   - content 指向 bus 内部 ring buffer，属于一段“借出”的只读内存；
 *   - 消费者处理完成后必须调用 message_bus_release()，否则该段空间不会归还。
 *
 * 生命周期示意：
 *
 *   producer string
 *        |
 *        | message_bus_push_*(): copy
 *        v
 *   [ring buffer record + content] --pop--> bus_msg_t.content
 *                                            |
 *                                            | consumer uses it
 *                                            v
 *                                      message_bus_release()
 */
typedef struct
{
    char channel[MESSAGE_BUS_CHANNEL_MAX];
    char chat_id[MESSAGE_BUS_CHAT_ID_MAX];
    const char *content;
    uint32_t content_len;

    /* 内部租约拥有者。外部不要读写；只交给 message_bus_release() 使用。 */
    void *owner;
} bus_msg_t;

/* 便捷构造函数：适用于 NUL 结尾的文本消息。二进制内容请手动设置 content_len。 */
static inline void bus_msg_set(bus_msg_t *msg,
                               const char *channel,
                               const char *chat_id,
                               const char *content)
{
    if(!msg)
    {
        return;
    }

    memset(msg, 0, sizeof(*msg));
    if(channel)
    {
        strncpy(msg->channel, channel, sizeof(msg->channel) - 1);
    }
    if(chat_id)
    {
        strncpy(msg->chat_id, chat_id, sizeof(msg->chat_id) - 1);
    }

    msg->content = content ? content : "";
    msg->content_len = content ? (uint32_t)strlen(content) : 0;
}

int message_bus_init(void);

/* 多生产者安全：push 内部通过 mutex + ticket 顺序保证先到先写。 */
int message_bus_push_inbound(const bus_msg_t *msg);

/* pop 成功后，调用方必须最终调用 message_bus_release(msg)。 */
int message_bus_pop_inbound(bus_msg_t *msg, uint32_t timeout_ms);

int message_bus_push_outbound(const bus_msg_t *msg);

int message_bus_pop_outbound(bus_msg_t *msg, uint32_t timeout_ms);

void message_bus_release(bus_msg_t *msg);

#endif
