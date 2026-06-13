#ifndef __CCLAW_CONSOLE_H__
#define __CCLAW_CONSOLE_H__

int agent_console_init(void);
int agent_console_start(void);
int agent_console_send_message(const char *chat_id, const char *content);

/**
 * 打印 CLI 提示符到 stdout。
 *
 * 用于流式输出完成后恢复提示符，避免在 agent_loop 中硬编码 "> "。
 * 当 AGENT_CONSOLE_ENABLE=0 时为空操作（no-op）。
 *
 * 使用场景：
 * - CLI channel 流式输出结束后，需要重新显示提示符供用户输入
 * - 其他需要在非消息上下文中恢复提示符的场景
 */
void agent_console_print_prompt(void);

#endif
