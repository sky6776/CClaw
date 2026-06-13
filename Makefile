# GNU make Makefile
#
# Usage:
#   make
#   make console
#   make lib
#   make clean
#
# Notes:
# - cJSON is built from the vendored source in cJSON/cJSON.c.
# - HTTPS support for agent_http.c links OpenSSL by default.

CC ?= gcc
AR ?= ar
MKDIR_P ?= mkdir -p
RM_RF ?= rm -rf

BUILD_DIR ?= build

CJSON_SRC := cJSON/cJSON.c
CJSON_CFLAGS ?=
HTTP_CFLAGS ?=
HTTP_LIBS ?= -lssl -lcrypto
PTHREAD_LIBS ?= -lpthread
AGENT_CONSOLE_ENABLE ?= 1

WARN_CFLAGS ?= -Wall -Wextra
STD_CFLAGS ?= -std=c11

INCLUDES := \
	-I. \
	-Iagent \
	-Ibus \
	-Ichannels \
	-Iconsole \
	-Icron \
	-Igateway \
	-Iheartbeat \
	-Ihttp \
	-Illm \
	-Imemory \
	-Iskills \
	-Itlsf \
	-Itools

CPPFLAGS += $(INCLUDES) $(CJSON_CFLAGS) $(HTTP_CFLAGS)
CPPFLAGS += -DAGENT_CONSOLE_ENABLE=$(AGENT_CONSOLE_ENABLE)
CFLAGS += $(STD_CFLAGS) $(WARN_CFLAGS)

AGENT_TLSF_SRCS := \
	tlsf/tlsf.c \
	tlsf/tlsf_thread.c \
	tlsf/agent_tlsf.c

AGENT_SRCS := \
	$(CJSON_SRC) \
	$(AGENT_TLSF_SRCS) \
	http/agent_http.c \
	agent_app.c \
	agent/agent_fs.c \
	agent/agent_loop.c \
	agent/context_builder.c \
	bus/message_bus.c \
	channels/weixin_channel.c \
	cron/cron_service.c \
	heartbeat/heartbeat.c \
	llm/llm_proxy.c \
	memory/memory_store.c \
	memory/session_mgr.c \
	skills/skill_loader.c \
	tools/tool_cron.c \
	tools/tool_files.c \
	tools/tool_registry.c

ifeq ($(AGENT_CONSOLE_ENABLE),1)
AGENT_SRCS += console/agent_console.c
endif

AGENT_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(AGENT_SRCS))
AGENT_CONSOLE_APP := $(BUILD_DIR)/agent_console

.PHONY: all build console lib clean

all: console

build: console

console: $(AGENT_CONSOLE_APP)

lib: $(BUILD_DIR)/libcclaw.a

$(BUILD_DIR)/libcclaw.a: $(AGENT_OBJS)
	$(AR) rcs $@ $^

$(AGENT_CONSOLE_APP): $(AGENT_OBJS) $(BUILD_DIR)/console/agent_console_main.o
	$(MKDIR_P) $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(HTTP_LIBS) $(PTHREAD_LIBS) -o $@

$(BUILD_DIR)/%.o: %.c
	$(MKDIR_P) $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	$(RM_RF) $(BUILD_DIR)
