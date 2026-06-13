GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/memory/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/memory
VPATH += :$(AGENT_DIR)/memory
CFLAGS += "-I$(AGENT_DIR)/memory"
