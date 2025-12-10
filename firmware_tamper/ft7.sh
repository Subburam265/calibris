#!/bin/sh
# ft7.sh - Firmware integrity checker with Auto-Compilation
#
# Fixes: Added cJSON.c and -lgpiod to the compilation command.

# --- Configuration ---
CONFIG_FILE="/home/pico/calibris/data/config.json"
WORK_DIR="/home/pico/calibris/hx711"
FILE_TO_CHECK="${WORK_DIR}/mw9"
SOURCE_FILE="${WORK_DIR}/mw9.c"

# Trusted hash for the known-good binary of mw9
# NOTE: Because we are changing how we compile (adding libraries), 
# the resulting hash WILL change. You must update this after a successful run.
TRUSTED_HASH="5387ccdf22538ee66cdc51437ba40fed74241990a75138efbfb022b5be77a7b5"

TAMPER_LOG_CLI="/home/pico/calibris/bin/tamper_log_bin/tamper_log"
SAFE_MODE_ACTIVATOR="/home/pico/calibris/bin/activate_safe_mode_bin/activate_safe_mode"

# --- Check tools existence ---
if [ ! -x "$TAMPER_LOG_CLI" ]; then
    echo "[ERROR] tamper_log CLI not found at $TAMPER_LOG_CLI"
    exit 1
fi

if [ ! -x "$SAFE_MODE_ACTIVATOR" ]; then
    echo "[ERROR] safe_mode_activator not found at $SAFE_MODE_ACTIVATOR"
    exit 1
fi

# --- Step 0: Compile Firmware ---
echo "[Step 0] Compiling firmware from source..."

if [ ! -f "$SOURCE_FILE" ]; then
    echo "[ERROR] Source file not found: $SOURCE_FILE"
    exit 1
fi

# Switch to the directory so headers are found
cd "$WORK_DIR" || exit 1

# COMPILE COMMAND UPDATED:
# 1. Added cJSON.c (source file)
# 2. Added -lgpiod (library flag)
gcc mw9.c hx711.c lcd.c cJSON.c -o mw9 -lgpiod -lpthread -lm

COMPILE_STATUS=$?
if [ $COMPILE_STATUS -ne 0 ]; then
    echo "[ERROR] Compilation failed! (Exit code: $COMPILE_STATUS)"
    exit 1
fi

echo "[OK] Compilation successful."

# --- Step 1: Calculate current hash ---
if ! command -v sha256sum >/dev/null 2>&1; then
    echo "[ERROR] sha256sum command not found"
    exit 1
fi
CURRENT_HASH=$(sha256sum "$FILE_TO_CHECK" | awk '{print $1}')

# --- Step 2: Compare hashes ---
if [ "$CURRENT_HASH" = "$TRUSTED_HASH" ]; then
    echo "[OK] Firmware integrity verified"
    exit 0
fi

# --- TAMPERING DETECTED ---
echo "[ALERT] Firmware tampering detected!"
echo "  File: $FILE_TO_CHECK"
echo "  Expected: $TRUSTED_HASH"
echo "  Got:      $CURRENT_HASH"

# Build details string (POSIX-compatible)
TRUSTED_SHORT=$(echo "$TRUSTED_HASH" | cut -c1-16)
CURRENT_SHORT=$(echo "$CURRENT_HASH" | cut -c1-16)
DETAILS="File: mw9 (Compiled) | Expected: ${TRUSTED_SHORT}... | Got: ${CURRENT_SHORT}..."

# --- Step 3: Log using CLI tool ---
echo ""
echo "[Step 3] Logging tamper event..."
"$TAMPER_LOG_CLI" --type "firmware" --details "$DETAILS"

LOG_EXIT_CODE=$?
if [ $LOG_EXIT_CODE -eq 0 ]; then
    echo "[OK] Tamper event logged successfully"
else
    echo "[ERROR] Failed to log tamper event (exit code: $LOG_EXIT_CODE)"
fi

# --- Step 4: Activate safe mode ---
echo ""
echo "[Step 4] Activating safe mode..."
"$SAFE_MODE_ACTIVATOR"

SAFE_MODE_EXIT_CODE=$?
if [ $SAFE_MODE_EXIT_CODE -eq 0 ]; then
    echo "[OK] Safe mode activated successfully"
else
    echo "[ERROR] Failed to activate safe mode (exit code: $SAFE_MODE_EXIT_CODE)"
fi

exit 1
