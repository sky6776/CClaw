#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "agent_conf.h"
#include "message_bus.h"
#include "memory_store.h"
#include "session_mgr.h"
#include "skill_loader.h"
#include "llm_proxy.h"
#include "cron_service.h"
#include "tool_registry.h"
#include "weixin_channel.h"
#include "heartbeat.h"
#include "agent_loop.h"
#include "agent_tlsf.h"
#include "agent_fs.h"
#if AGENT_CONSOLE_ENABLE
#include "agent_console.h"
#endif
//--------------------------------------
#define AGENT_VER       "V0.0.1"
//--------------------------------------




/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void *outbound_dispatch_task(void *arg)
{
    (void)arg;
    AGENT_LOG("Outbound dispatch started");

    while(1)
    {
        bus_msg_t msg;
        /*
         * outbound 消息正文由 message_bus 借出；发送到具体 channel 后立即 release。
         * release 前不要把 msg.content 保存到异步回调里，否则会悬空。
         */
        if(message_bus_pop_outbound(&msg, UINT32_MAX) <= 0)
        {
            continue;
        }

        AGENT_LOG("Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if(strcmp(msg.channel, CHANNEL_TELEGRAM) == 0)
        {
            //            int send_err = telegram_send_message(msg.chat_id, msg.content);
            //            if (send_err != 0) {
            //                AGENT_LOG("Telegram send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
            //            } else {
            //                AGENT_LOG("Telegram send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
            //            }
        }
        else if(strcmp(msg.channel, CHANNEL_WEBSOCKET) == 0)
        {
            //            int ws_err = ws_server_send(msg.chat_id, msg.content);
            //            if(ws_err != 0)
            //            {
            //                AGENT_LOG("WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            //            }
        }
        else if(strcmp(msg.channel, CHANNEL_WEIXIN) == 0)
        {
            int send_err = weixin_channel_send_message(msg.chat_id, msg.content);
            if(send_err != 0)
            {
                AGENT_LOG("Weixin send failed for %s", msg.chat_id);
            }
            else
            {
                AGENT_LOG("Weixin send success for %s (%d bytes): \n\n%s\n",
                          msg.chat_id, (int)strlen(msg.content), msg.content);
            }
        }
        else if(strcmp(msg.channel, CHANNEL_SYSTEM) == 0)
        {
            AGENT_LOG("System message [%s](%d bytes): \n\n%s\n",
                      msg.chat_id, (int)strlen(msg.content), msg.content);
        }
        else if(strcmp(msg.channel, CHANNEL_CLI) == 0)
        {
#if AGENT_CONSOLE_ENABLE
            agent_console_send_message(msg.chat_id, msg.content);
#else
            AGENT_LOG("CLI message [%s](%d bytes): \n\n%s\n",
                      msg.chat_id, (int)strlen(msg.content), msg.content);
#endif
        }
        else
        {
            AGENT_LOG("Unknown channel: %s", msg.channel);
        }

        /* 归还 outbound ring 空间，允许 agent_loop 继续写后续回复。 */
        message_bus_release(&msg);
    }

    return NULL;
}

static void create_outbound_dispatch_task(void)
{
    pthread_t outbound_dispatch_thread = 0;

    if(0 != pthread_create(&outbound_dispatch_thread, NULL, outbound_dispatch_task, NULL))
    {
        AGENT_LOG("fail to create outbound_dispatch_task");
    }
}

void agent_app_init(void)
{
    AGENT_LOG("agent app %s", AGENT_VER);

    if(agent_tlsf_init() != 0)
    {
        AGENT_LOG("fail to init TLSF allocator");
        return;
    }
    if(agent_fs_init_layout() != 0)
    {
        AGENT_LOG("agent storage layout init incomplete");
    }
    message_bus_init();
    memory_store_init();
    skill_loader_init();
    session_mgr_init();
    llm_proxy_init();
    cron_service_init();
    tool_registry_init();
    weixin_channel_init();
    heartbeat_init();
#if AGENT_CONSOLE_ENABLE
    agent_console_init();
#endif
    agent_loop_init();

    create_outbound_dispatch_task();
    weixin_channel_start();
#if AGENT_CONSOLE_ENABLE
    agent_console_start();
#endif
    agent_loop_start();
    cron_service_start();
    heartbeat_start();
}
