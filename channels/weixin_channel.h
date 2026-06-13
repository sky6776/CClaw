#ifndef __MOD_WEIXIN_CHANNEL_H__
#define __MOD_WEIXIN_CHANNEL_H__

#include "agent_conf.h"
#include "bus/message_bus.h"

/*
 * Weixin / WeChat iLink Bot runtime channel.
 *
 * This module only handles agent runtime messaging:
 * - weixin_channel_init(): load token, allowlist, and enabled config.
 * - weixin_channel_start(): poll incoming messages into the inbound bus.
 * - weixin_channel_send_message(): send agent replies through Weixin.
 *
 * QR login and token acquisition are handled by tools/weixin_login.
 */
int weixin_channel_init(void);
int weixin_channel_start(void);
int weixin_channel_is_enabled(void);
int weixin_channel_send_message(const char *chat_id, const char *text);

#endif
