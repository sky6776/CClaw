# CClaw

CClaw 基于 [memovai/mimiclaw](https://github.com/memovai/mimiclaw) 改造而来。CClaw 这个名字表示 “a pure-C Claw”：一只纯 C 的 Claw。最初的 mimiclaw 更像一套面向 ESP32-S3 的完整固件，而 CClaw 更关注一件事：把 agent 能力沉淀成一个干净、轻量、可以带到不同宿主里的 C 模块。

它保留了真正有复用价值的部分，例如 LLM 对话、工具调用、会话记忆、cron、heartbeat、文件存储和通道接入；同时把 ESP-IDF、WiFi 配网、NVS、SPIFFS、GPIO、OTA 这些板级绑定从核心链路里拿掉。这样 CClaw 既可以作为一个独立 agent 跑起来，也可以安静地嵌进已有工程里，成为宿主系统的一部分。

## 快速启动

最简启动流程如下：

1. 获取 Weixin/iLink Bot key。

   进入扫码登录工具目录并编译：

   ```sh
   cd tools/weixin_login
   make
   ```

   编译成功后执行：

   ```sh
   ./build/weixin_login --env-file weixin.env --qrencode
   ```

   如果本机没有安装 `qrencode`，可以把控制台输出的链接复制到任意二维码生成工具里生成二维码，然后使用微信扫码。扫码确认后，工具会把最终 key 保存到 `tools/weixin_login/weixin.env`。

2. 获取 DeepSeek key。

   在 DeepSeek 控制台创建或复制可用的 API key。

3. 写入本地配置。

   打开 `agent_conf.h`，设置 DeepSeek key：

   ```c
   #define AGENT_SECRET_API_KEY            "your-deepseek-key"
   ```

   同时把 `tools/weixin_login/weixin.env` 里的 Weixin bot key 写入：

   ```c
   #define AGENT_WEIXIN_BOT_TOKEN          "your-weixin-bot-key"
   ```

4. 编译并运行 agent。

   回到项目根目录执行：

   ```sh
   make
   ./build/agent_console
   ```

   启动后即可通过控制台，或通过微信与 agent 交互。

## 和 mimiclaw 的主要区别

mimiclaw 是一个完整的 ESP-IDF 工程，入口是 `app_main()`，运行时依赖 NVS、SPIFFS、WiFi、FreeRTOS task、ESP HTTP client、ESP WebSocket、GPIO、OTA、配网门户等 ESP 生态组件。它的定位很明确：直接作为 ESP32-S3 固件运行。

CClaw 的改造方向则更偏“拿来就能嵌入”：

- 线程模型从 FreeRTOS task 改为 POSIX `pthread`。
- 文件存储从 ESP SPIFFS 改为普通目录和文件，例如 `agent/config`、`agent/memory`、`agent/sessions`。
- HTTP/HTTPS 从 `esp_http_client` 改为 POSIX socket + OpenSSL。
- 配置从 NVS/ESP console 体系转为编译期宏和环境变量等更通用的方式。
- 构建方式从 ESP-IDF CMake 工程改为普通 GNU Make 模块。
- 通道层新增 Weixin/iLink Bot 支持，并移除了原有 Telegram、飞书通道。
- 顶层 `Makefile` 用于单独编译 agent；`.mk` 系列用于宿主工程嵌入，两者互不依赖。

## CClaw 的优势

### 不绑定 ESP 平台

CClaw 不再要求 ESP-IDF、ESP32-S3、SPIFFS 分区、NVS、WiFi 配网或 FreeRTOS。只要目标平台支持常见 POSIX 能力，例如 `pthread`、文件系统、socket，就能把它跑起来。

这带来的直接好处是：开发、调试和部署都不再被具体板子卡住。它可以用于嵌入式 Linux，也可以在桌面 Linux、macOS、类 Unix 环境，或其他提供 POSIX 兼容层的宿主工程中使用。

### 依赖边界清晰

核心依赖控制在通用 C/POSIX 运行环境和少量明确的第三方库范围内：

- C11 编译器
- pthread
- OpenSSL，用于 HTTPS
- vendored [`cJSON`](https://github.com/DaveGamble/cJSON)
- vendored [`tlsf`](https://github.com/sysprog21/tlsf-bsd)

`cJSON` 使用 [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON)，`tlsf` 使用 [sysprog21/tlsf-bsd](https://github.com/sysprog21/tlsf-bsd)。两者已随源码提供，不需要额外拉取复杂组件。相比 mimiclaw 的 ESP-IDF 组件集合，CClaw 的依赖关系更透明，也更便于宿主工程进行集成评估和构建管理。

### 两套构建入口，场景分明

CClaw 同时照顾两种用法，但这两种用法不是同一条构建链路上的上下游，而是两套彼此独立的入口。

- 顶层 `Makefile`：用于单独编译 CClaw agent。它可以生成独立运行的 console agent，也可以生成服务于独立 agent 场景的静态库，方便本地调试、验证和部署。
- `.mk` 系列：用于嵌入宿主工程。入口 `.mk` 文件会把 CClaw 的模块源码、include 路径和依赖交给宿主工程的 Makefile，由宿主工程决定怎么编译、怎么链接、怎么启动。

也就是说，宿主工程 include 入口 `.mk` 文件时，不会调用本目录顶层 `Makefile`；顶层 `Makefile` 里的 console、静态库等目标，也不会自动影响 `.mk` 嵌入路径。这样 CClaw 既能作为一个单独 agent 跑起来，也能以源码模块的方式安静地进入宿主工程。

### 使用 TLSF 管理 agent 内存

CClaw 引入 TLSF allocator，并提供统一的 `agent_malloc`、`agent_calloc`、`agent_realloc`、`agent_free`、`agent_strdup`。

默认内存池大小为 4MB：

```c
#define AGENT_TLSF_POOL_SIZE (4U * 1024U * 1024U)
```

TLSF 封装还支持多 arena 和细粒度锁，默认使用 `pthread_mutex_t`。这让 agent 内部的 JSON、LLM 响应、session 读写、HTTP buffer 等动态内存更可控。对于一个需要长期运行、又经常处理变长文本和 JSON 的 agent 来说，这一点很实用。

`cJSON` 的分配器也被 hook 到 TLSF，因此 JSON 对象和序列化字符串统一走 agent 内存池。

### 消息总线更适合大消息和长期运行

mimiclaw 的 message bus 基于 FreeRTOS queue，消息体是 heap 分配的 `char *`，调用方需要处理所有权和释放。

CClaw 改成预分配 ring buffer：

- inbound/outbound 每个方向默认 1MB，可通过 `MESSAGE_BUS_QUEUE_BYTES` 调整。
- 单条消息默认最大 256KB，可通过 `MESSAGE_BUS_MAX_CONTENT_BYTES` 调整。
- push 时将正文复制进 ring buffer。
- pop 时返回内部 buffer 的只读借用指针。
- 消费完成后调用 `message_bus_release()` 归还空间。

这个设计减少了消息路径上的 malloc/free，也绕开了 OS 队列常见的单条消息大小限制。对 agent 这种经常会遇到长回复、工具输出、上下文片段的系统来说，消息总线会更稳一些。

### 普通文件系统替代 SPIFFS

CClaw 启动时会自动初始化本地目录结构：

```text
agent/
  config/
    SOUL.md
    USER.md
  memory/
    MEMORY.md
    daily/
  sessions/
  skills/
  cron.json
  HEARTBEAT.md
```

这让 agent 的人格、用户信息、长期记忆、会话、技能和定时任务都变成普通文件。调试时可以直接打开看，迁移时可以直接拷走，宿主工程也更容易接管这些数据。

### LLM 通信更通用

CClaw 当前聚焦 DeepSeek，并以 OpenAI-compatible chat/completions 协议为核心。内部保留 provider 描述表和 request/response 转换层，后续扩展 Kimi、OpenRouter、Groq 等兼容 provider 时，可以在现有结构上继续加，而不是把业务逻辑重新拆开。

CClaw 还实现了 SSE 流式输出：

- `llm_chat_tools()`：非流式，一次性返回完整响应。
- `llm_chat_stream()`：流式读取 SSE token，并在结束后保留完整文本和工具调用结果。

这使 console 模式下可以实时看到模型输出，也为宿主工程接入实时 UI 提供了基础。对使用者来说，agent 不再像“黑盒等待”，而是能边思考边反馈。

### session 更健壮

mimiclaw 的 session 主要按 `chat_id` 存储，并且历史回放基本只保留 `role + content`。

CClaw 做了几项增强：

- session key 使用 `channel + chat_id`，避免不同通道同名 chat_id 串档。
- 对 channel/chat_id 做百分号编码，避免路径冲突和非法文件名。
- 支持保存结构化字段：`reasoning_content`、`tool_calls`、`tool_call_id`。
- 支持原子写入 assistant tool call 和 tool result 两条记录。
- 如果第二条写入失败，会尝试通过 `ftruncate` 回滚，避免留下半条工具调用链。
- 历史裁剪时会跳过开头的孤儿 `tool_result`，降低下一轮 LLM API 因工具上下文不完整而失败的概率。

这些改动对 DeepSeek reasoning 模式和 OpenAI-compatible tool call 都很重要。它们解决的不是“能不能保存几句话”这么简单的问题，而是保证下一轮请求仍然带着完整、可回放的工具调用上下文。

### 工具和通道更贴近当前使用场景

CClaw 保留了更通用的工具集：

- `read_file`
- `write_file`
- `edit_file`
- `list_dir`
- `cron_add`
- `cron_list`
- `cron_remove`

同时去掉 mimiclaw 中强依赖 ESP 硬件或设备配置的功能，例如 GPIO 工具、WiFi 配网、OTA、ESP WebSocket gateway、NVS 配置管理等。

通道侧也做了重新取舍：CClaw 新增 Weixin/iLink Bot 通道，作为当前主要 IM 接入方式；原 mimiclaw 中的 Telegram 和飞书通道已经移除。这样通道实现更少、更集中，也更贴近当前项目实际使用场景。

当前保留/提供的通道包括：

- console：便于本地调试和作为独立 agent 使用；通过 `.mk` 系列嵌入宿主工程时默认不包含 console 源码。
- Weixin/iLink Bot：新增支持，通过通用 HTTP 层实现，不依赖 ESP HTTP client。

## 构建方式

### 顶层 Makefile：单独编译 agent

顶层 `Makefile` 是给 CClaw 自己使用的独立构建入口，用来在当前目录下直接编译 agent。它适合做本地开发、功能验证、console 调试，或者把 CClaw 当作一个独立 agent 进程来部署。

构建独立 console agent：

```sh
make console
```

生成：

```text
build/agent_console
```

构建静态库：

```sh
make lib
```

生成：

```text
build/libcclaw.a
```

清理独立构建产物：

```sh
make clean
```

这里的 `libcclaw.a` 仍然属于顶层 `Makefile` 的独立构建产物。它和下面的 `.mk` 嵌入方式是两条路：前者由本目录 `Makefile` 组织编译，后者由宿主工程自己的构建系统组织编译。

如果使用顶层 `Makefile` 构建静态库，默认会编入 console 支持；这种情况下若不需要 console，可以设置：

```sh
make AGENT_CONSOLE_ENABLE=0 lib
```

### .mk 系列：嵌入宿主工程

`.mk` 系列不是顶层 `Makefile` 的子流程，而是给宿主工程 include 的源码集成入口。宿主工程使用 `.mk` 系列时，不需要、也不会先执行本目录的 `make console` 或 `make lib`。

通常做法是把 CClaw 放到宿主工程约定的模块目录下，然后在宿主工程自己的 Makefile 中 include CClaw 提供的入口 `.mk` 文件。`.mk` 文件名和默认目录仍然沿用现有工程布局，实际接入时按仓库中的文件位置 include 即可。

入口 `.mk` 文件默认假定 CClaw 位于宿主工程的既定模块目录；如果放在其他目录，需要同步调整 `AGENT_DIR`。include 之后，`.mk` 系列会按宿主工程已有约定追加 `GEN_CSRCS`、`DEPPATH`、`VPATH`、`CFLAGS`、`LDFLAGS` 等变量。这样 CClaw 会成为宿主工程的一组源码模块，而不是一个预先编好的独立程序。

## 嵌入宿主工程

宿主工程可以直接包含入口 `.mk` 文件，或自行把各模块源码加入构建系统。当前 `.mk` 嵌入方式没有包含 `console/` 模块，因此默认不带内置 console。宿主可以接自己的输入输出通道，也可以直接向 message bus 投递消息。

初始化入口为：

```c
#include "agent_conf.h"

int main(void)
{
    agent_app_init();

    for (;;) {
        /* host main loop */
    }
}
```

## Weixin / iLink Bot 配置

CClaw 新增了 Weixin/iLink Bot 通道，原有 Telegram 和飞书通道已经移除。当前扫码登录工具位于 `tools/weixin_login/`，它的作用是先帮你拿到运行时需要的 bot token/key，然后再交给 agent 使用。工具运行时需要 `curl`，如果希望在终端里直接显示二维码，可以额外安装 `qrencode`。

先编译工具：

```sh
cd tools/weixin_login
make
```

工具会生成在：

```text
tools/weixin_login/build/weixin_login
```

然后在同一目录下通过工具扫码登录并保存配置。带 `--qrencode` 时会尝试在终端显示二维码；如果系统没有安装 `qrencode`，也可以不加这个参数，根据工具输出的二维码内容自行处理。

```sh
build/weixin_login --env-file weixin.env --qrencode
```

扫码成功后，工具会输出或写入类似下面的配置：

```env
WEIXIN_ENABLED=1
WEIXIN_BOT_TOKEN=your-bot-token
```

如果回到项目根目录后在 POSIX shell 里运行独立 agent，可以在启动前加载这个文件：

```sh
set -a
. tools/weixin_login/weixin.env
set +a
```

拿到 token/key 后，有两种常用配置方式：

- 开发或宿主工程集成时，建议通过环境变量传入：运行时设置 `WEIXIN_ENABLED=1`、`WEIXIN_BOT_TOKEN=...`，如有需要再设置 `WEIXIN_ALLOW_FROM=...` 做来源限制。注意：代码会优先使用非空的 `AGENT_WEIXIN_BOT_TOKEN`，再读取环境变量；如果希望 token 由环境变量提供，请让编译期 token 保持为空。如果希望是否启用也由环境变量决定，请不要在编译期强制打开 `AGENT_WEIXIN_ENABLED`。
- 固化到构建产物时，可以在 `agent_conf.h` 或宿主工程的编译参数中设置 `AGENT_WEIXIN_ENABLED=1`，并更新 `AGENT_WEIXIN_BOT_TOKEN`。这种方式最直接，但不建议把真实 token/key 提交到公共仓库。

agent 初始化时会启动已启用的通道。独立运行时可以直接使用顶层 `Makefile` 构建出的 console agent；嵌入宿主工程时，由宿主调用 `agent_app_init()`，Weixin 通道会跟随 agent 初始化流程启动。

## 配置

主要配置集中在 `agent_conf.h`：

- agent 文件目录
- LLM provider、model、API URL
- LLM token 上限和流式开关
- Weixin/iLink Bot token、allowlist、轮询参数
- cron 和 heartbeat 参数
- session、context buffer 大小

Weixin/iLink Bot 的获取和启用流程见上一节。生产环境建议不要把真实 API key 或 bot token 固化在源码中，而是通过环境变量、宿主配置系统或编译期注入方式传入。

## 取舍

CClaw 的目标不是完整复刻 mimiclaw 的 ESP32-S3 固件能力，而是把“agent 真正需要的那部分能力”抽出来，做成一个更干净、可移植、可嵌入的内核。

因此，以下 mimiclaw 能力在 CClaw 中被移除或不再作为核心依赖：

- ESP WiFi 管理和 captive portal 配网
- NVS 配置存储
- SPIFFS 分区和预烧录数据
- OTA
- ESP GPIO 工具
- ESP WebSocket gateway
- 原有 Telegram 通道
- 原有飞书通道
- ESP console 命令系统
- web_search / get_current_time 等依赖 ESP HTTP client 或 NVS 的工具

换来的好处是：代码更轻、依赖更少、平台边界更清晰，既能作为单独 agent 使用，也能嵌入更大的宿主工程。CClaw 更像一个可以被宿主长期带着走的 agent 模块，而不是只能运行在特定板子上的一套固件。

## 许可证

CClaw 项目代码采用 MIT License。

仓库中随源码提供的第三方组件保留其各自上游许可证：`cJSON` 使用 MIT License，`tlsf-bsd` 使用 BSD-3-Clause License。相关许可证信息以第三方源码文件头和上游项目为准。

## 致谢

CClaw 的演进离不开这些优秀的开源项目：

- [memovai/mimiclaw](https://github.com/memovai/mimiclaw)：CClaw 基于该项目改造而来，延续并重构了其中的 agent 设计与实践经验。
- [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON)：为 CClaw 提供轻量、稳定的 C JSON 解析与生成能力。
- [sysprog21/tlsf-bsd](https://github.com/sysprog21/tlsf-bsd)：为 CClaw 的 agent 内存池提供 TLSF allocator 实现基础。

感谢这些项目及其维护者对开源生态的贡献。
