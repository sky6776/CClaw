GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/cJSON/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/cJSON
VPATH += :$(AGENT_DIR)/cJSON
