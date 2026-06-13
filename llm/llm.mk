GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/llm/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/llm
VPATH += :$(AGENT_DIR)/llm
CFLAGS += "-I$(AGENT_DIR)/llm"
