GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/tlsf/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/tlsf
VPATH += :$(AGENT_DIR)/tlsf
CFLAGS += "-I$(AGENT_DIR)/tlsf"
