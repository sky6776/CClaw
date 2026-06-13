# Standalone Makefile for the Weixin scan-login helper.
#
# Usage:
#   make
#   make clean
#   build/weixin_login --env-file weixin.env --qrencode
#
# Runtime dependencies:
#   curl
#   qrencode (optional, only with --qrencode)
# Build dependencies:
#   bundled CClaw cJSON source

ifeq ($(origin CC),default)
CC := gcc
endif
MKDIR ?= mkdir
RM_RF ?= rm -rf

BUILD_DIR ?= build
WARN_CFLAGS ?= -Wall -Wextra
STD_CFLAGS ?= -std=c11
CJSON_DIR ?= ../../cJSON
CJSON_SRC := $(CJSON_DIR)/cJSON.c

CPPFLAGS += -I../.. -I../../tlsf
CFLAGS += $(STD_CFLAGS) $(WARN_CFLAGS)

TARGET := $(BUILD_DIR)/weixin_login

.PHONY: all build clean

all: build

build: $(TARGET)

$(TARGET): weixin_login.c $(CJSON_SRC)
	-$(MKDIR) $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) weixin_login.c $(CJSON_SRC) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	$(RM_RF) $(BUILD_DIR)
