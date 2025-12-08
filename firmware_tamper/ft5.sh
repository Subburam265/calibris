#!/bin/sh
# firmware_tamper. sh - Firmware integrity checker
#
# Uses the tamper_log CLI tool for logging with blockchain support. 
# The CLI tool handles: DB insert, blockchain, safe_mode update, service stop

# --- Configuration ---
CONFIG_FILE="/home/pico/calibris/data/config.json"
FILE_TO_CHECK="/home/pico/calibris/hx711/mw7.c"
TRUSTED_HASH="4101b063f3d5f48890f0ff2e69208c575d7f94d8497093b3d967a07d17735a63"
TAMPER_LOG_CLI="/home/pico/calibris/bin/tamper_log"

# --- Check if tamper_log CLI exists ---
if [ !  -x "$TAMPER_LOG_CLI" ]; then
    echo "[ERROR] tamper_log CLI not found at $TAMPER_LOG_CLI"
    echo "[ERROR] Please build and install the tamper_log tool first."
    exit 1
fi

# --- Check if file exists ---
if [ ! -f "$FILE_TO_CHECK" ]; then
    echo "[ERROR] File not found: $FILE_TO_CHECK"
    exit 1
fi

# --- Calculate current hash ---
if ! command -v sha256sum >/dev/null 2>&1; then
    echo "[ERROR] sha256sum command not found"
    exit 1
fi
CURRENT_HASH=$(sha256sum "$FILE_TO_CHECK" | awk '{print $1}')

# --- Compare hashes ---
if [ "$CURRENT_HASH" = "$TRUSTED_HASH" ]; then
    echo "[OK] Firmware integrity verified"
    exit 0
fi

# --- TAMPERING DETECTED ---
echo "[ALERT] Firmware tampering detected!"
echo "  File: $FILE_TO_CHECK"
echo "  Expected: $TRUSTED_HASH"
echo "  Got:      $CURRENT_HASH"

# Build details string
DETAILS="File: $FILE_TO_CHECK | Expected: ${TRUSTED_HASH:0:16}...  | Got: ${CURRENT_HASH:0:16}..."

# Log using CLI tool (handles everything: DB, blockchain, safe_mode, service)
"$TAMPER_LOG_CLI" --type "firmware" --details "$DETAILS"

EXIT_CODE=$? 
if [ $EXIT_CODE -eq 0 ]; then
    echo "[OK] Tamper event logged successfully"
else
    echo "[ERROR] Failed to log tamper event (exit code: $EXIT_CODE)"
fi

exit 1
