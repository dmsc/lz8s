# Minimal standard makefile
# -------------------------

# Compiler flags
CC = gcc
CFLAGS = -g -Og -Wall -flto

# Build folders
BUILD_DIR = build
OUT_DIR = $(BUILD_DIR)
OBJ_DIR = $(BUILD_DIR)/obj

# Main programs
PROGS=\
 lz8s\
 lz8dec\

#######################################
TARGETS=$(PROGS:%=$(OUT_DIR)/%)

all: $(TARGETS)

$(TARGETS): $(OUT_DIR)/%: $(OBJ_DIR)/%.o | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT_DIR) $(OBJ_DIR):
	mkdir -p $@

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
