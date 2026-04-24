# RAMking — Makefile

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -pedantic -std=c99 -g
INCLUDE  = -Iinclude

SRC_DIR  = src
TEST_DIR = test
OBJ_DIR  = obj

SRCS     = $(SRC_DIR)/safe_alloc.c
OBJS     = $(OBJ_DIR)/safe_alloc.o

TEST_SRC = $(TEST_DIR)/test_safe_alloc.c
TEST_BIN = test_safe_alloc

.PHONY: all test clean

all: $(TEST_BIN)

# Compile the library object
$(OBJ_DIR)/safe_alloc.o: $(SRC_DIR)/safe_alloc.c include/safe_alloc.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

# Build the test binary
$(TEST_BIN): $(OBJS) $(TEST_SRC)
	$(CC) $(CFLAGS) $(INCLUDE) $(TEST_SRC) $(OBJS) -o $@

# Create obj directory if needed
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Build and run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(OBJ_DIR) $(TEST_BIN)
