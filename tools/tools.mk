GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/tools/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/tools
VPATH += :$(AGENT_DIR)/tools
CFLAGS += "-I$(AGENT_DIR)/tools"
