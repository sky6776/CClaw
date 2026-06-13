GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/gateway/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/gateway
VPATH += :$(AGENT_DIR)/gateway
CFLAGS += "-I$(AGENT_DIR)/gateway"
