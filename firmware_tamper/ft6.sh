#!/bin/sh
# firmware_tamper. sh - Firmware integrity checker
#
# Uses the tamper_log CLI tool for logging with blockchain support. 
# Then activates safe mode using safe_mode_activator. 

# --- Configuration ---
CONFIG_FILE="/home/pico/calibris/data/config.json"
FILE_TO_CHECK="/home/pico/calibris/hx711/mw7.c"
TRUSTED_HASH="4101b063f3d5f48890f0ff2e69208c575d7f94d8497093b3d967a07d17735a63"
TAMPER_LOG_CLI="/home/pico/calibris/bin/tamper_log_bin/tamper_log"
SAFE_MODE_ACTIVATOR="/home/pico/calibris/bin/activate_safe_mode_bin/activate_safe_mode"

# --- Check if tamper_log CLI exists ---
if [ !  -x "$TAMPER_LOG_CLI" ]; then
    echo "[ERROR] tamper_log CLI not found at $TAMPER_LOG_CLI"
    echo "[ERROR] Please build and install the tamper_log tool first."
    exit 1
fi

# --- Check if safe_mode_activator exists ---
if [ ! -x "$SAFE_MODE_ACTIVATOR" ]; then
    echo "[ERROR] safe_mode_activator not found at $SAFE_MODE_ACTIVATOR"
    echo "[ERROR] Please build and install the safe_mode_activator tool first."
    exit 1
fi

# --- Check if file exists ---
if [ !  -f "$FILE_TO_CHECK" ]; then
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
echo "  Got:       $CURRENT_HASH"

# Build details string (POSIX-compatible)
TRUSTED_SHORT=$(echo "$TRUSTED_HASH" | cut -c1-16)
CURRENT_SHORT=$(echo "$CURRENT_HASH" | cut -c1-16)
DETAILS="File: $FILE_TO_CHECK | Expected: ${TRUSTED_SHORT}...  | Got: ${CURRENT_SHORT}..."

# Step 1: Log using CLI tool
echo ""
echo "[Step 1] Logging tamper event..."
"$TAMPER_LOG_CLI" --type "firmware" --details "$DETAILS"

LOG_EXIT_CODE=$?
if [ $LOG_EXIT_CODE -eq 0 ]; then
    echo "[OK] Tamper event logged successfully"
else
    echo "[ERROR] Failed to log tamper event (exit code: $LOG_EXIT_CODE)"
fi

# Step 2: Activate safe mode
echo ""
echo "[Step 2] Activating safe mode..."
"$SAFE_MODE_ACTIVATOR"

SAFE_MODE_EXIT_CODE=$?
if [ $SAFE_MODE_EXIT_CODE -eq 0 ]; then
    echo "[OK] Safe mode activated successfully"
else
    echo "[ERROR] Failed to activate safe mode (exit code: $SAFE_MODE_EXIT_CODE)"
fi

exit 1
