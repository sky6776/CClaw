GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/agent/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/agent
VPATH += :$(AGENT_DIR)/agent
CFLAGS += "-I$(AGENT_DIR)/agent"
