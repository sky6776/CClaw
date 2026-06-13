#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_proxy.h"
#include "agent_tlsf.h"
#include "cJSON/cJSON.h"
#include "lib_log.h"
#include "mod_user_shell.h"

#ifdef LLM_PROXY_TEST

/*
 * 这些函数只在编译 llm_proxy.c 时定义 LLM_PROXY_TEST 才会导出。
 * 测试用它们绕过真实 HTTP 调用，直接验证 DeepSeek provider 的协议适配逻辑。
 */
const char *llm_proxy_test_provider(void);
const char *llm_proxy_test_model(void);
const char *llm_proxy_test_api_url(void);
char *llm_proxy_test_build_request_json(const char *system_prompt,
                                        cJSON *messages,
                                        const char *tools_json);
int llm_proxy_test_parse_response_json(const char *json, llm_response_t *resp);

static cJSON *parse_json_or_die(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    AGENT_ASSERT(root != NULL);
    return root;
}

static const char *string_item(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    AGENT_ASSERT(item != NULL);
    AGENT_ASSERT(cJSON_IsString(item));
    return item->valuestring;
}

static void test_provider_descriptor(void)
{
    /*
     * 当前版本明确“仅支持 DeepSeek”：
     * - provider 注册表中只有 deepseek；
     * - 默认模型使用 DeepSeek；
     * - URL 指向 DeepSeek chat/completions。
     *
     * 如果以后新增 provider，这个测试可以调整为检查默认 provider 仍为 deepseek，
     * 并新增其他 provider 自己的 descriptor 测试。
     */
    AGENT_ASSERT(strcmp(llm_proxy_test_provider(), "deepseek") == 0);
    AGENT_ASSERT(strcmp(llm_proxy_test_model(), "deepseek-v4-pro") == 0);
    AGENT_ASSERT(strcmp(llm_proxy_test_api_url(),
                        "https://api.deepseek.com/v1/chat/completions") == 0);
}

static void test_build_request_without_tools(void)
{
    /*
     * 无工具场景：最小请求体应该只包含 model/max_tokens/messages。
     * system prompt 被转换为第一条 role=system 消息，用户消息保持 role=user。
     */
    cJSON *messages = parse_json_or_die(
                          "[{\"role\":\"user\",\"content\":\"hello\"}]");
    char *request = llm_proxy_test_build_request_json("system prompt", messages, NULL);
    AGENT_ASSERT(request != NULL);

    cJSON *root = parse_json_or_die(request);
    AGENT_ASSERT(strcmp(string_item(root, "model"), "deepseek-v4-pro") == 0);
    AGENT_ASSERT(cJSON_GetObjectItem(root, "tools") == NULL);
    AGENT_ASSERT(cJSON_GetObjectItem(root, "tool_choice") == NULL);

    cJSON *max_tokens = cJSON_GetObjectItem(root, "max_tokens");
    AGENT_ASSERT(max_tokens != NULL);
    AGENT_ASSERT(cJSON_IsNumber(max_tokens));

    cJSON *api_messages = cJSON_GetObjectItem(root, "messages");
    AGENT_ASSERT(api_messages != NULL);
    AGENT_ASSERT(cJSON_IsArray(api_messages));
    AGENT_ASSERT(cJSON_GetArraySize(api_messages) == 2);

    cJSON *sys = cJSON_GetArrayItem(api_messages, 0);
    AGENT_ASSERT(strcmp(string_item(sys, "role"), "system") == 0);
    AGENT_ASSERT(strcmp(string_item(sys, "content"), "system prompt") == 0);

    cJSON *user = cJSON_GetArrayItem(api_messages, 1);
    AGENT_ASSERT(strcmp(string_item(user, "role"), "user") == 0);
    AGENT_ASSERT(strcmp(string_item(user, "content"), "hello") == 0);

    cJSON_Delete(root);
    cJSON_Delete(messages);
    agent_free(request);
}

static void test_build_request_converts_tools_and_history(void)
{
    /*
     * 有工具和历史的场景最容易出错，所以这里覆盖完整链路：
     * 1. 内部工具定义 input_schema -> DeepSeek function.parameters；
     * 2. assistant 的 tool_use block -> tool_calls；
     * 3. user 的 tool_result block -> role=tool 消息；
     * 4. 普通文本仍然保留为 role=user/assistant 的 content。
     */
    const char *messages_json =
        "["
        " {\"role\":\"assistant\",\"content\":["
        "   {\"type\":\"text\",\"text\":\"I'll check.\"},"
        "   {\"type\":\"tool_use\",\"id\":\"call_1\",\"name\":\"read_file\","
        "    \"input\":{\"path\":\"notes.txt\"}}"
        " ]},"
        " {\"role\":\"user\",\"content\":["
        "   {\"type\":\"tool_result\",\"tool_use_id\":\"call_1\",\"content\":\"ok\"},"
        "   {\"type\":\"text\",\"text\":\"continue\"}"
        " ]}"
        "]";
    const char *tools_json =
        "[{\"name\":\"read_file\",\"description\":\"Read file\","
        "\"input_schema\":{\"type\":\"object\",\"properties\":"
        "{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}]";

    cJSON *messages = parse_json_or_die(messages_json);
    char *request = llm_proxy_test_build_request_json("system prompt", messages, tools_json);
    AGENT_ASSERT(request != NULL);

    cJSON *root = parse_json_or_die(request);
    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    AGENT_ASSERT(tools != NULL);
    AGENT_ASSERT(cJSON_IsArray(tools));
    AGENT_ASSERT(cJSON_GetArraySize(tools) == 1);
    AGENT_ASSERT(strcmp(string_item(cJSON_GetArrayItem(tools, 0), "type"), "function") == 0);

    cJSON *function = cJSON_GetObjectItem(cJSON_GetArrayItem(tools, 0), "function");
    AGENT_ASSERT(function != NULL);
    AGENT_ASSERT(strcmp(string_item(function, "name"), "read_file") == 0);
    AGENT_ASSERT(strcmp(string_item(function, "description"), "Read file") == 0);
    AGENT_ASSERT(cJSON_GetObjectItem(function, "parameters") != NULL);
    AGENT_ASSERT(strcmp(string_item(root, "tool_choice"), "auto") == 0);

    cJSON *api_messages = cJSON_GetObjectItem(root, "messages");
    AGENT_ASSERT(cJSON_GetArraySize(api_messages) == 4);

    cJSON *assistant = cJSON_GetArrayItem(api_messages, 1);
    AGENT_ASSERT(strcmp(string_item(assistant, "role"), "assistant") == 0);
    AGENT_ASSERT(strcmp(string_item(assistant, "content"), "I'll check.") == 0);
    cJSON *tool_calls = cJSON_GetObjectItem(assistant, "tool_calls");
    AGENT_ASSERT(tool_calls != NULL);
    AGENT_ASSERT(cJSON_GetArraySize(tool_calls) == 1);

    cJSON *tool_msg = cJSON_GetArrayItem(api_messages, 2);
    AGENT_ASSERT(strcmp(string_item(tool_msg, "role"), "tool") == 0);
    AGENT_ASSERT(strcmp(string_item(tool_msg, "tool_call_id"), "call_1") == 0);
    AGENT_ASSERT(strcmp(string_item(tool_msg, "content"), "ok") == 0);

    cJSON *user = cJSON_GetArrayItem(api_messages, 3);
    AGENT_ASSERT(strcmp(string_item(user, "role"), "user") == 0);
    AGENT_ASSERT(strcmp(string_item(user, "content"), "continue") == 0);

    cJSON_Delete(root);
    cJSON_Delete(messages);
    agent_free(request);
}

static void test_parse_deepseek_tool_call_response(void)
{
    /*
     * DeepSeek 的 tool call 响应符合 OpenAI-compatible choices/message 结构。
     * 解析后 agent_loop 仍拿到统一的 llm_response_t：
     * - text 保存 assistant 文本；
     * - tool_use 表示本轮需要执行工具；
     * - calls[] 保存工具名、调用 id 和 arguments JSON 字符串。
     */
    const char *response_json =
        "{"
        " \"choices\":[{"
        "   \"finish_reason\":\"tool_calls\","
        "   \"message\":{"
        "     \"content\":\"Need file\","
        "     \"tool_calls\":[{"
        "       \"id\":\"call_x\","
        "       \"type\":\"function\","
        "       \"function\":{\"name\":\"read_file\","
        "       \"arguments\":\"{\\\"path\\\":\\\"notes.txt\\\"}\"}"
        "     }]"
        "   }"
        " }]"
        "}";

    llm_response_t resp = {0};
    AGENT_ASSERT(llm_proxy_test_parse_response_json(response_json, &resp) == 0);
    AGENT_ASSERT(resp.tool_use);
    AGENT_ASSERT(resp.text != NULL);
    AGENT_ASSERT(strcmp(resp.text, "Need file") == 0);
    AGENT_ASSERT(resp.text_len == strlen("Need file"));
    AGENT_ASSERT(resp.call_count == 1);
    AGENT_ASSERT(strcmp(resp.calls[0].id, "call_x") == 0);
    AGENT_ASSERT(strcmp(resp.calls[0].name, "read_file") == 0);
    AGENT_ASSERT(strcmp(resp.calls[0].input, "{\"path\":\"notes.txt\"}") == 0);

    llm_response_free(&resp);
}

int test_dp_build_parse(char **args)
{
    (void)args;
    AGENT_ASSERT(agent_tlsf_init() == 0);
    test_provider_descriptor();
    test_build_request_without_tools();
    test_build_request_converts_tools_and_history();
    test_parse_deepseek_tool_call_response();

    AGENT_LOG("test_llm_deepseek: ok");
    return 0;
}
USH_CMD_EXPORT_ALIAS(test_dp_build_parse, agent_dp, test deepseek protocol build / parse);

#endif
