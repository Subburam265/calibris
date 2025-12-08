# Makefile for tamper_log library
# Save this as: /home/pico/calibris/lib/Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lsqlite3 -lssl -lcrypto

# Source files (matching YOUR filenames with 's')
SRC = tamper_logs.c
HDR = tamper_logs.h
OBJ = tamper_logs.o

# Output library
STATIC_LIB = libtamper_log.a

# Default target
all: $(STATIC_LIB)

# Compile object file
$(OBJ): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -c $(SRC) -o $(OBJ)

# Create static library
$(STATIC_LIB): $(OBJ)
	ar rcs $(STATIC_LIB) $(OBJ)
	@echo ""
	@echo "[SUCCESS] Built library: $(STATIC_LIB)"
	@echo ""

# Clean build files
clean:
	rm -f $(OBJ) $(STATIC_LIB)
	@echo "[CLEANED] Removed build files"

. PHONY: all clean
