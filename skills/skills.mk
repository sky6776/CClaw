GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/skills/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/skills
VPATH += :$(AGENT_DIR)/skills
CFLAGS += "-I$(AGENT_DIR)/skills"
