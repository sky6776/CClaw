AGENT_DIR = module/mod_agent
HTTP_CFLAGS ?=
HTTP_LIBS ?= -lssl -lcrypto
CFLAGS += $(HTTP_CFLAGS)
LDFLAGS += $(HTTP_LIBS)

include $(AGENT_DIR)/agent/agent.mk
include $(AGENT_DIR)/bus/bus.mk
include $(AGENT_DIR)/channels/channels.mk
include $(AGENT_DIR)/cron/cron.mk
include $(AGENT_DIR)/gateway/gateway.mk
include $(AGENT_DIR)/heartbeat/heartbeat.mk
include $(AGENT_DIR)/llm/llm.mk
include $(AGENT_DIR)/memory/memory.mk
include $(AGENT_DIR)/skills/skills.mk
include $(AGENT_DIR)/test/test.mk
include $(AGENT_DIR)/tools/tools.mk
include $(AGENT_DIR)/http/http.mk
include $(AGENT_DIR)/cJSON/cJSON.mk
include $(AGENT_DIR)/tlsf/tlsf.mk

GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/*.c))
DEPPATH += --dep-path $(AGENT_DIR)
VPATH += :$(AGENT_DIR)
CFLAGS += -I$(AGENT_DIR)
