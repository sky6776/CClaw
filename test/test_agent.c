#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include "agent_conf.h"
#include "message_bus.h"
#include "lib_log.h"
#include "mod_user_shell.h"


//用户输入 -> agentloop -> LLM
static bool send_usermsg_to_llm(const char *user_msg)
{
    AGENT_LOG("user_msg:%s", user_msg);

    bus_msg_t msg;
    /* CLI 输入作为 inbound 文本消息；push 会复制内容，之后可安全复用 args 字符串。 */
    bus_msg_set(&msg, CHANNEL_CLI, "cli", user_msg);

    int err = message_bus_push_inbound(&msg);
    if(err != 0)
    {
        AGENT_LOG("Failed to push heartbeat message: %d", err);
        return false;
    }

    AGENT_LOG("Triggered agent check");

    return true;
}

void test_agent(char **args)
{
    static bool init_flag = false;

    if(false == init_flag)
    {
        agent_app_init();
        init_flag = true;
        sleep(2);
    }

    if((NULL == args[1]) || (0 == strlen(args[1])))
    {
        AGENT_LOG("para error! Please use: ush agent_test your_input");
        return;
    }

    send_usermsg_to_llm(args[1]);
}
USH_CMD_EXPORT_ALIAS(test_agent, agent_test, test agent);

