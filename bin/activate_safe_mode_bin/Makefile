# Makefile for Safe Mode Activator
# Location: /home/pico/calibris/bin/

CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = activate_safe_mode
SRC = activate_safe_mode.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)
	@echo ""
	@echo "[SUCCESS] Built: $(TARGET)"
	@echo ""
	@echo "Usage:"
	@echo "  ./$(TARGET)              # Use default config path"
	@echo "  ./$(TARGET) --help       # Show help"
	@echo ""

install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/
	@echo "[INSTALLED] $(TARGET) -> /usr/local/bin/"

clean:
	rm -f $(TARGET)

.PHONY: all install clean
