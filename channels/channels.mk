GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/channels/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/channels
VPATH += :$(AGENT_DIR)/channels
CFLAGS += "-I$(AGENT_DIR)/channels"
