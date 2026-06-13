#ifndef __CCLAW_CONF_H__
#define __CCLAW_CONF_H__

#include <stdio.h>

#ifndef UNUSED
#define UNUSED(x)	(void)(x)
#endif

#define AGENT_LOG(fmt, ...) 		    printf("[%s %s %d]"fmt"\n", __FILE__, __func__, __LINE__, ##__VA_ARGS__)
#define AGENT_ASSERT(exp)		do{							\
	if(!(exp)) {										\
		printf("%s:%d:%s: Assertion '%s' failed.\n",	\
			   __FILE__, __LINE__, __func__, #exp);		\
		abort();										\
	}													\
}while(0)


#define LLM_PROXY_TEST                  1

#define AGENT_BASE                      "agent"
#define AGENT_CONFIG_DIR                AGENT_BASE "/config"
#define AGENT_MEMORY_DIR                AGENT_BASE "/memory"
#define AGENT_MEMORY_DAILY_DIR          AGENT_MEMORY_DIR "/daily"
#define AGENT_SESSION_DIR               AGENT_BASE "/sessions"
#define AGENT_SKILLS_DIR                AGENT_BASE "/skills"
#define SKILLS_PREFIX                   AGENT_SKILLS_DIR "/"
#define AGENT_MEMORY_FILE               AGENT_MEMORY_DIR "/MEMORY.md"
#define AGENT_SOUL_FILE                 AGENT_CONFIG_DIR "/SOUL.md"
#define AGENT_USER_FILE                 AGENT_CONFIG_DIR "/USER.md"
#define AGENT_CONTEXT_BUF_SIZE          (16 * 1024)
#define AGENT_SESSION_MAX_MSGS          20

/* Agent Loop */
#define AGENT_MAX_HISTORY               20
#define AGENT_MAX_TOOL_ITER             10
#define AGENT_MAX_TOOL_CALLS            4
#define AGENT_SEND_WORKING_STATUS       1

/* LLM */
/*
 * 当前版本只启用 DeepSeek。
 *
 * DeepSeek 的接口兼容 OpenAI chat/completions 协议，但这里保留 provider name
 * 和 API URL 两个独立配置，方便以后在 llm_proxy.c 的 provider 注册表中继续
 * 增加其他 provider，而不是把协议细节散落在业务代码里。
 */
#define AGENT_LLM_DEFAULT_MODEL         "deepseek-v4-pro"
#define AGENT_LLM_PROVIDER_NAME         "deepseek"
#define AGENT_LLM_MAX_TOKENS            4096
#define AGENT_LLM_API_URL               "https://api.deepseek.com/v1/chat/completions"
#define AGENT_LLM_STREAM_BUF_SIZE       (32 * 1024)
#define AGENT_LLM_LOG_VERBOSE_PAYLOAD   0
#define AGENT_LLM_LOG_PREVIEW_BYTES     160

/* 流式输出 (SSE) 配置 */
#define AGENT_LLM_STREAM_ENABLED        1       /* 1=启用流式, 0=禁用（回退到非流式） */
#define AGENT_SECRET_API_KEY            ""
/* 当编译期没有写入 AGENT_SECRET_API_KEY 时，从这个环境变量读取 DeepSeek key。 */
#define AGENT_DEEPSEEK_API_KEY_ENV      "DEEPSEEK_API_KEY"

/* Weixin / WeChat iLink Bot channel */
/*
 * 通过 iLink Bot API 长轮询 getupdates 收消息，
 * 再用 sendmessage 把 agent 回复发回微信。
 *
 * 微信 token 默认从 AGENT_WEIXIN_BOT_TOKEN 或 WEIXIN_BOT_TOKEN 环境变量读取。
 * 可通过 Makefile 里的 weixin-login 目标构建扫码登录工具来获取 token。
 * 要启用轮询线程，设置 AGENT_WEIXIN_ENABLED 为 1，或设置环境变量 WEIXIN_ENABLED=1。
 */
#define AGENT_WEIXIN_ENABLED            1
#define AGENT_WEIXIN_ENABLED_ENV        "WEIXIN_ENABLED"
#define AGENT_WEIXIN_BOT_TOKEN          ""
#define AGENT_WEIXIN_BOT_TOKEN_ENV      "WEIXIN_BOT_TOKEN"
#define AGENT_WEIXIN_ALLOW_FROM         ""
#define AGENT_WEIXIN_ALLOW_FROM_ENV     "WEIXIN_ALLOW_FROM"
#define AGENT_WEIXIN_API_BASE           "https://ilinkai.weixin.qq.com"
#define AGENT_WEIXIN_POLL_TIMEOUT_SEC   35
#define AGENT_WEIXIN_RETRY_SEC          2
#define AGENT_WEIXIN_BACKOFF_SEC        30
#define AGENT_WEIXIN_MAX_FAILURES       3
#define AGENT_WEIXIN_SEND_CHUNK_BYTES   2000

/* Cron / Heartbeat */
#define CRON_FILE                       AGENT_BASE "/cron.json"
#define CRON_MAX_JOBS                   16
#define CRON_CHECK_INTERVAL_SEC         60
#define AGENT_CRON_TASK_PREFIX          "[CRON_TASK]"
#define HEARTBEAT_FILE                  AGENT_BASE "/HEARTBEAT.md"
#define HEARTBEAT_INTERVAL_SEC           (30 * 60)


void agent_app_init(void);


#endif
