GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/heartbeat/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/heartbeat
VPATH += :$(AGENT_DIR)/heartbeat
CFLAGS += "-I$(AGENT_DIR)/heartbeat"
