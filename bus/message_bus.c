#define _POSIX_C_SOURCE 200809L

#include "message_bus.h"
#include "agent_conf.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define MSG_TYPE_INBOUND    0x01U
#define MSG_TYPE_OUTBOUND   0x02U

#define BUS_RECORD_MAGIC    0x4d534731U
#define BUS_PADDING_MAGIC   0x50414431U
#define BUS_ALIGN_BYTES     8U

/*
 * 设计目标：
 *   1. 不再依赖 Linux/SysV 消息队列，避免单条消息大小限制。
 *   2. 不在消息路径上 malloc/free，队列内存一次性静态预分配。
 *   3. 多生产者 push 时按 ticket 顺序进入，避免某个生产者长期抢不到空间。
 *   4. pop 采用 lease 模型，正文不二次拷贝，消费者 release 后才释放 ring 空间。
 *
 * 单个方向队列的数据布局：
 *
 *   buffer[0]                                                     buffer[N-1]
 *   +------------------+-------------------+------+-----------------------+
 *   | record + content | record + content  | ...  | padding or free space |
 *   +------------------+-------------------+------+-----------------------+
 *          ^                                      ^
 *          |                                      |
 *       read_pos                              write_pos
 *
 * 每条记录的实际字节布局：
 *
 *   +--------------+------------------+--------+-----------+
 *   | bus_record_t | content bytes    | '\0'   | align pad |
 *   +--------------+------------------+--------+-----------+
 *
 * 回绕场景：
 *
 *   如果队尾剩余空间不足以容纳下一条完整记录，会在队尾写入 padding，
 *   然后把 write_pos 回到 0，从队首继续写。
 *
 *   before:
 *     +-------------+------------------+-----------+
 *     | used record | free but too tiny | free head |
 *     +-------------+------------------+-----------+
 *                                 ^write_pos
 *
 *   after:
 *     +-------------+---------+--------------------+
 *     | new record  |   ...   | padding to buffer end
 *     +-------------+---------+--------------------+
 *       ^write_pos wrapped to head
 */
typedef struct
{
    /* padding 和 record 共用这个前缀，读取时先看 magic 判断类型。 */
    uint32_t magic;
    uint32_t total_len;
} bus_prefix_t;

typedef struct
{
    /* magic 用于在读端快速识别损坏或 padding。 */
    uint32_t magic;

    /* total_len 包含 header、正文、NUL 结尾和对齐 padding。 */
    uint32_t total_len;

    /* content_len 只表示有效正文长度，不包含末尾额外写入的 NUL。 */
    uint32_t content_len;

    /* 当前只用于调试/校验方向，inbound/outbound 各自固定。 */
    uint32_t msg_type;

    /* 路由字段固定保存在 header 里，正文仍然变长存储。 */
    char channel[MESSAGE_BUS_CHANNEL_MAX];
    char chat_id[MESSAGE_BUS_CHAT_ID_MAX];
} bus_record_t;

typedef struct
{
    const char *name;
    uint32_t msg_type;

    /* lock 保护下面所有队列状态；cond 只在持锁时检查条件。 */
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    /* 变长消息正文存储区。所有 record/padding 都写在这块内存里。 */
    unsigned char buffer[MESSAGE_BUS_QUEUE_BYTES];

    /* read_pos 指向下一条可 pop 的 record；write_pos 指向下一次写入位置。 */
    size_t read_pos;
    size_t write_pos;

    /* used 包含 record 和 padding；msg_count 只统计真实消息，不统计 padding。 */
    size_t used;
    uint32_t msg_count;

    /*
     * ticket 公平机制：
     *
     *   producer A       producer B       producer C
     *      |                |                |
     *      v                v                v
     *   ticket=0        ticket=1        ticket=2
     *      |                |                |
     *      +------ wait until serving_ticket matches -----+
     *
     * 只有 ticket == serving_ticket 且 ring 空间足够时才能写入。
     * 这样不会出现后来的小消息一直插队，导致较早的大消息永远无法 push。
     */
    uint64_t next_ticket;
    uint64_t serving_ticket;

    /*
     * lease 状态：
     * pop 返回的 content 指向内部 buffer。为了避免 pop 后立刻覆盖这段内存，
     * 当前实现每个方向一次只允许借出一条消息；消费者 release 后才推进 read_pos。
     */
    bool leased;
    size_t leased_len;
    const char *leased_content;
} bus_queue_t;

static bus_queue_t s_inbound_queue; //to agent loop
static bus_queue_t s_outbound_queue; //from agent loop
static bool s_initialized = false;

/* 所有 record 长度按 8 字节对齐，方便下一条 header 保持自然边界。 */
static size_t bus_align_up(size_t value)
{
    return (value + (BUS_ALIGN_BYTES - 1U)) & ~(size_t)(BUS_ALIGN_BYTES - 1U);
}

/* 环形下标推进。len 不会超过队列长度，因此最多回绕一次。 */
static size_t bus_advance_pos(size_t pos, size_t len)
{
    pos += len;
    if(pos >= MESSAGE_BUS_QUEUE_BYTES)
    {
        pos -= MESSAGE_BUS_QUEUE_BYTES;
    }
    return pos;
}

/* 剩余总空间，包含队尾和队首两段可能不连续的空间。 */
static size_t bus_queue_free_bytes(const bus_queue_t *q)
{
    return MESSAGE_BUS_QUEUE_BYTES - q->used;
}

/*
 * 计算写入一条 record 需要占用多少 ring 空间。
 *
 * 如果队尾连续空间足够：
 *   required = record_len
 *
 * 如果队尾连续空间不足：
 *   required = tail_padding + record_len
 *
 * 这样可以在判断空间时提前把“回绕 padding”也计算进去。
 */
static size_t bus_queue_required_space(const bus_queue_t *q, size_t record_len)
{
    size_t tail = MESSAGE_BUS_QUEUE_BYTES - q->write_pos;

    if(tail >= record_len)
    {
        return record_len;
    }

    return tail + record_len;
}

/* 在队尾写 padding，把读端明确引导回 buffer 头部。调用者必须持有 q->lock。 */
static void bus_queue_write_padding(bus_queue_t *q, size_t len)
{
    bus_prefix_t pad;

    if(len == 0)
    {
        return;
    }

    if(len >= sizeof(pad))
    {
        pad.magic = BUS_PADDING_MAGIC;
        pad.total_len = (uint32_t)len;
        memcpy(q->buffer + q->write_pos, &pad, sizeof(pad));
        if(len > sizeof(pad))
        {
            memset(q->buffer + q->write_pos + sizeof(pad), 0, len - sizeof(pad));
        }
    }
    else
    {
        memset(q->buffer + q->write_pos, 0, len);
    }

    q->write_pos = 0;
    q->used += len;
}

/* 队列损坏或初始化时使用的内部复位。调用者必须持有 q->lock 或处于初始化阶段。 */
static void bus_queue_reset_locked(bus_queue_t *q)
{
    q->read_pos = 0;
    q->write_pos = 0;
    q->used = 0;
    q->msg_count = 0;
    q->leased = false;
    q->leased_len = 0;
    q->leased_content = NULL;
}

/*
 * 跳过 read_pos 处可能存在的 padding。
 *
 * 典型场景：
 *   write 端因为队尾空间不足写入 padding 并回绕；
 *   read 端消费完队尾最后一条真实消息后，会先看到 padding；
 *   这里跳过 padding，继续从 buffer[0] 读取下一条 record。
 *
 * 调用者必须持有 q->lock。
 */
static int bus_queue_skip_padding_locked(bus_queue_t *q)
{
    while(q->used > 0)
    {
        size_t tail = MESSAGE_BUS_QUEUE_BYTES - q->read_pos;
        bus_prefix_t prefix;

        if(tail < sizeof(prefix))
        {
            q->read_pos = 0;
            q->used -= tail;
            continue;
        }

        memcpy(&prefix, q->buffer + q->read_pos, sizeof(prefix));
        if(prefix.magic == BUS_RECORD_MAGIC)
        {
            return 0;
        }

        if(prefix.magic == BUS_PADDING_MAGIC &&
                prefix.total_len > 0 &&
                prefix.total_len <= tail &&
                prefix.total_len <= q->used)
        {
            q->read_pos = bus_advance_pos(q->read_pos, prefix.total_len);
            q->used -= prefix.total_len;
            continue;
        }

        AGENT_LOG("%s queue corrupt, reset", q->name);
        bus_queue_reset_locked(q);
        return -EIO;
    }

    return 0;
}

static void bus_queue_init(bus_queue_t *q, const char *name, uint32_t msg_type)
{
    memset(q, 0, sizeof(*q));
    q->name = name;
    q->msg_type = msg_type;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

/* 给 timedwait 计算绝对超时时间。这里使用 CLOCK_REALTIME，对现有 pthread API 兼容最好。 */
static void bus_timespec_add_ms(struct timespec *ts, uint32_t timeout_ms)
{
    ts->tv_sec += timeout_ms / 1000U;
    ts->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;

    if(ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/*
 * 等待 pop 条件成立：
 *   - 队列里至少有一条真实消息；
 *   - 当前没有未 release 的 lease。
 *
 * lease 的限制很重要：
 *   pop 返回的是内部 buffer 指针，如果允许下一次 pop/release 提前推进 read_pos，
 *   生产者可能覆盖上一条消费者仍在使用的 content。
 */
static int bus_wait_for_pop(bus_queue_t *q, uint32_t timeout_ms)
{
    if(timeout_ms == 0U)
    {
        return (q->msg_count > 0U && !q->leased) ? 0 : -EAGAIN;
    }

    if(timeout_ms == UINT32_MAX)
    {
        while(q->msg_count == 0U || q->leased)
        {
            pthread_cond_wait(&q->not_empty, &q->lock);
        }
        return 0;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    bus_timespec_add_ms(&deadline, timeout_ms);

    while(q->msg_count == 0U || q->leased)
    {
        int err = pthread_cond_timedwait(&q->not_empty, &q->lock, &deadline);
        if(err == ETIMEDOUT)
        {
            return -ETIMEDOUT;
        }
        if(err != 0)
        {
            return -err;
        }
    }

    return 0;
}

/*
 * 公平 push 主流程：
 *
 *   producer
 *      |
 *      v
 *   normalize content/content_len
 *      |
 *      v
 *   build record header
 *      |
 *      v
 *   lock queue
 *      |
 *      v
 *   take ticket = next_ticket++
 *      |
 *      v
 *   wait(ticket == serving_ticket && enough_space)
 *      |
 *      v
 *   write padding if needed, then write [record][content][NUL]
 *      |
 *      v
 *   serving_ticket++, signal consumers/producers, unlock
 *
 * 注意：这里的“动态大小”是从静态 ring buffer 中按需占用字节，
 * 不是调用 malloc/free。
 */
static int bus_queue_push(bus_queue_t *q, const bus_msg_t *msg)
{
    const char *content;
    uint32_t content_len;
    size_t raw_len;
    size_t record_len;
    uint64_t ticket;
    bus_record_t record;

    if(!msg)
    {
        AGENT_LOG("msg is NULL");
        return -EINVAL;
    }

    content = msg->content ? msg->content : "";
    content_len = msg->content_len;
    if(content_len == 0U && content[0] != '\0')
    {
        content_len = (uint32_t)strlen(content);
    }

    if(content_len > MESSAGE_BUS_MAX_CONTENT_BYTES)
    {
        AGENT_LOG("%s message too large: %u > %u",
                  q->name, content_len, MESSAGE_BUS_MAX_CONTENT_BYTES);
        return -EMSGSIZE;
    }

    raw_len = sizeof(bus_record_t) + (size_t)content_len + 1U;
    record_len = bus_align_up(raw_len);
    if(record_len > MESSAGE_BUS_QUEUE_BYTES)
    {
        AGENT_LOG("%s message does not fit queue: %u bytes",
                  q->name, content_len);
        return -EMSGSIZE;
    }

    memset(&record, 0, sizeof(record));
    record.magic = BUS_RECORD_MAGIC;
    record.total_len = (uint32_t)record_len;
    record.content_len = content_len;
    record.msg_type = q->msg_type;
    strncpy(record.channel, msg->channel, sizeof(record.channel) - 1);
    strncpy(record.chat_id, msg->chat_id, sizeof(record.chat_id) - 1);

    pthread_mutex_lock(&q->lock);

    /*
     * ticket 只在持锁区分配，确保 next_ticket/serving_ticket 的更新有序。
     * pthread_cond_wait 会原子释放 lock 并睡眠，被唤醒后重新持锁检查 while 条件。
     */
    ticket = q->next_ticket++;
    while(ticket != q->serving_ticket ||
            bus_queue_required_space(q, record_len) > bus_queue_free_bytes(q))
    {
        pthread_cond_wait(&q->not_full, &q->lock);
    }

    /* 队尾放不下完整 record 时，用 padding 占满队尾，再从 buffer[0] 写入。 */
    if(MESSAGE_BUS_QUEUE_BYTES - q->write_pos < record_len)
    {
        bus_queue_write_padding(q, MESSAGE_BUS_QUEUE_BYTES - q->write_pos);
    }

    memcpy(q->buffer + q->write_pos, &record, sizeof(record));
    if(content_len > 0U)
    {
        memcpy(q->buffer + q->write_pos + sizeof(record), content, content_len);
    }
    q->buffer[q->write_pos + sizeof(record) + content_len] = '\0';
    if(record_len > raw_len)
    {
        memset(q->buffer + q->write_pos + raw_len, 0, record_len - raw_len);
    }

    q->write_pos = bus_advance_pos(q->write_pos, record_len);
    q->used += record_len;
    q->msg_count++;
    q->serving_ticket++;

    pthread_cond_signal(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);

    return 0;
}

/*
 * pop 主流程：
 *
 *   consumer
 *      |
 *      v
 *   lock queue
 *      |
 *      v
 *   wait message available and no outstanding lease
 *      |
 *      v
 *   skip padding
 *      |
 *      v
 *   copy header metadata, return content pointer into ring buffer
 *      |
 *      v
 *   mark leased, unlock
 *
 * content 是只读借用指针；消费者必须调用 message_bus_release() 归还空间。
 */
static int bus_queue_pop(bus_queue_t *q, bus_msg_t *msg, uint32_t timeout_ms)
{
    bus_record_t record;
    const unsigned char *record_addr;
    int err;

    if(!msg)
    {
        AGENT_LOG("msg is NULL");
        return -EINVAL;
    }

    pthread_mutex_lock(&q->lock);

    err = bus_wait_for_pop(q, timeout_ms);
    if(err != 0)
    {
        pthread_mutex_unlock(&q->lock);
        return err;
    }

    err = bus_queue_skip_padding_locked(q);
    if(err != 0)
    {
        pthread_cond_broadcast(&q->not_full);
        pthread_mutex_unlock(&q->lock);
        return err;
    }

    /*
     * 不直接把 unsigned char buffer 强转成 bus_record_t* 后解引用。
     * ARM 目标上未对齐结构体访问可能异常；memcpy 到局部变量最稳妥。
     */
    record_addr = q->buffer + q->read_pos;
    memcpy(&record, record_addr, sizeof(record));
    if(record.magic != BUS_RECORD_MAGIC ||
            record.total_len == 0U ||
            record.total_len > q->used ||
            record.content_len > MESSAGE_BUS_MAX_CONTENT_BYTES)
    {
        AGENT_LOG("%s queue record invalid, reset", q->name);
        bus_queue_reset_locked(q);
        pthread_cond_broadcast(&q->not_full);
        pthread_mutex_unlock(&q->lock);
        return -EIO;
    }

    memset(msg, 0, sizeof(*msg));
    strncpy(msg->channel, record.channel, sizeof(msg->channel) - 1);
    strncpy(msg->chat_id, record.chat_id, sizeof(msg->chat_id) - 1);
    msg->content = (const char *)(record_addr + sizeof(record));
    msg->content_len = record.content_len;
    msg->owner = q;

    q->leased = true;
    q->leased_len = record.total_len;
    q->leased_content = msg->content;

    pthread_mutex_unlock(&q->lock);
    return (int)(sizeof(record) + record.content_len);
}

int message_bus_init(void)
{
    if(s_initialized)
    {
        return 0;
    }

    bus_queue_init(&s_inbound_queue, "inbound", MSG_TYPE_INBOUND);
    bus_queue_init(&s_outbound_queue, "outbound", MSG_TYPE_OUTBOUND);
    s_initialized = true;

    AGENT_LOG("Message bus initialized (%u bytes per queue, %u bytes max content)",
              MESSAGE_BUS_QUEUE_BYTES, MESSAGE_BUS_MAX_CONTENT_BYTES);
    return 0;
}

/* inbound: 外部 channel/cron/heartbeat -> agent loop。 */
int message_bus_push_inbound(const bus_msg_t *msg)
{
    return bus_queue_push(&s_inbound_queue, msg);
}

int message_bus_pop_inbound(bus_msg_t *msg, uint32_t timeout_ms)
{
    return bus_queue_pop(&s_inbound_queue, msg, timeout_ms);
}

/* outbound: agent loop -> outbound_dispatch -> 各 channel。 */
int message_bus_push_outbound(const bus_msg_t *msg)
{
    return bus_queue_push(&s_outbound_queue, msg);
}

int message_bus_pop_outbound(bus_msg_t *msg, uint32_t timeout_ms)
{
    return bus_queue_pop(&s_outbound_queue, msg, timeout_ms);
}

/*
 * 释放 pop 借出的消息空间。
 *
 * release 前：
 *   +------------------+-------------------+----------+
 *   | leased record    | next record        | free ... |
 *   +------------------+-------------------+----------+
 *     ^read_pos
 *
 * release 后：
 *   +------------------+-------------------+----------+
 *   | free/old bytes   | next record        | free ... |
 *   +------------------+-------------------+----------+
 *                        ^read_pos
 *
 * 注意：
 *   - msg->content 在 release 后立即失效，调用方不得继续保存或访问；
 *   - release 会唤醒等待空间的 producer；
 *   - 如果 release 后读指针遇到 padding，会顺便跳到 buffer[0]。
 */
void message_bus_release(bus_msg_t *msg)
{
    bus_queue_t *q;

    if(!msg || !msg->owner)
    {
        return;
    }

    q = (bus_queue_t *)msg->owner;
    pthread_mutex_lock(&q->lock);

    if(q->leased && q->leased_content == msg->content)
    {
        q->read_pos = bus_advance_pos(q->read_pos, q->leased_len);
        q->used -= q->leased_len;
        q->msg_count--;
        q->leased = false;
        q->leased_len = 0;
        q->leased_content = NULL;

        if(q->used == 0U)
        {
            q->read_pos = 0;
            q->write_pos = 0;
        }
        else
        {
            bus_queue_skip_padding_locked(q);
        }

        pthread_cond_broadcast(&q->not_full);
        pthread_cond_broadcast(&q->not_empty);
    }

    pthread_mutex_unlock(&q->lock);
    memset(msg, 0, sizeof(*msg));
}
