GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/cron/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/cron
VPATH += :$(AGENT_DIR)/cron
CFLAGS += "-I$(AGENT_DIR)/cron"
