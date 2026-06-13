GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/bus/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/bus
VPATH += :$(AGENT_DIR)/bus
CFLAGS += "-I$(AGENT_DIR)/bus"
