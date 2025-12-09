# Makefile for tamper_log CLI tool
# Save this as: /home/pico/calibris/bin/Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2

# The CLI tool source file (matching YOUR filename)
SRC = tamper_logs_cli.c
TARGET = tamper_log

# Library location
LIB_DIR = ../../lib
LIB = $(LIB_DIR)/libtamper_log.a

# Libraries to link
LDFLAGS = -lsqlite3 -lssl -lcrypto

# Default target
all: $(TARGET)

# Build the CLI tool
$(TARGET): $(SRC) $(LIB)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) -I$(LIB_DIR) $(LIB) $(LDFLAGS)
	@echo ""
	@echo "[SUCCESS] Built CLI tool: $(TARGET)"
	@echo ""
	@echo "Test with: ./$(TARGET) --help"
	@echo ""

# Clean
clean:
	rm -f $(TARGET)
	@echo "[CLEANED] Removed $(TARGET)"

. PHONY: all clean
