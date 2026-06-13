GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/test/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/test
VPATH += :$(AGENT_DIR)/test
CFLAGS += "-I$(AGENT_DIR)/test"
