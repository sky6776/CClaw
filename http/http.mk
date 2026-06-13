GEN_CSRCS += $(notdir $(wildcard $(AGENT_DIR)/http/*.c))
DEPPATH += --dep-path $(AGENT_DIR)/http
VPATH += :$(AGENT_DIR)/http
CFLAGS += "-I$(AGENT_DIR)/http"
