#!/bin/sh
# firmware_tamper.sh - Firmware integrity checker
#
# Fixes Applied:
# - Fixed filename typo (config. json -> config.json)
# - Improved JSON parsing regex to handle spaces around colons
# - Added default values for variables to prevent SQL syntax errors
# - Fixed syntax errors in 'if' conditions
# - Added error checking for sqlite3 command

# --- Configuration ---
CONFIG_FILE="/home/pico/calibris/data/config.json"
DB_PATH="/home/pico/calibris/data/mydata.db"
FILE_TO_CHECK="/home/pico/calibris/hx711/mw7.c"
TRUSTED_HASH="4101b063f3d5f48890f0ff2e69208c575d7f94d8497093b3d967a07d17735a63"

# --- Read config values (with robust whitespace handling) ---
# We use sed to handle potential spaces around ':' which grep -o misses
DEVICE_ID=$(grep -o '"device_id"[[:space:]]*:[[:space:]]*[0-9]*' "$CONFIG_FILE" | grep -o '[0-9]*' | head -n 1)
# "type" logic ensures we don't accidentally match "device_type" by including the quote
DEVICE_TYPE=$(grep -o '"type"[[:space:]]*:[[:space:]]*"[^"]*"' "$CONFIG_FILE" | cut -d'"' -f4 | head -n 1)
LATITUDE=$(grep -o '"latitude"[[:space:]]*:[[:space:]]*[0-9.]*' "$CONFIG_FILE" | grep -o '[0-9.]*' | head -n 1)
LONGITUDE=$(grep -o '"longitude"[[:space:]]*:[[:space:]]*[0-9.]*' "$CONFIG_FILE" | grep -o '[0-9.]*' | head -n 1)
CITY=$(grep -o '"city"[[:space:]]*:[[:space:]]*"[^"]*"' "$CONFIG_FILE" | cut -d'"' -f4 | head -n 1)
STATE=$(grep -o '"state"[[:space:]]*:[[:space:]]*"[^"]*"' "$CONFIG_FILE" | cut -d'"' -f4 | head -n 1)
ZERO_DRIFT=$(grep -o '"zero_drift"[[:space:]]*:[[:space:]]*[0-9.]*' "$CONFIG_FILE" | grep -o '[0-9.]*' | head -n 1)

# --- Apply Defaults (Crucial for SQL Safety) ---
# If variables are empty, SQL query will fail with syntax error
DEVICE_ID=${DEVICE_ID:-0}
DEVICE_TYPE=${DEVICE_TYPE:-"Unknown"}
LATITUDE=${LATITUDE:-0.0}
LONGITUDE=${LONGITUDE:-0.0}
CITY=${CITY:-"Unknown"}
STATE=${STATE:-"Unknown"}
ZERO_DRIFT=${ZERO_DRIFT:-0.0}

# --- Check if file exists ---
if [ ! -f "$FILE_TO_CHECK" ]; then
    echo "[ERROR] File not found: $FILE_TO_CHECK"
    exit 1
fi

# --- Calculate current hash ---
# Ensure sha256sum exists
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

# 1. Log to SQLite database
# Note: Using defaults ensures we don't have broken SQL (e.g. "VALUES (,,)")
sqlite3 "$DB_PATH" "INSERT INTO tamper_logs (device_id, device_type, tamper_type, resolution_status, latitude, longitude, city, state, drift) VALUES ($DEVICE_ID, '$DEVICE_TYPE', 'firmware', 'detected', $LATITUDE, $LONGITUDE, '$CITY', '$STATE', $ZERO_DRIFT);"

if [ $? -eq 0 ]; then
    echo "[DB] Tamper event logged"
    echo "     device_id        : $DEVICE_ID"
    echo "     device_type      : $DEVICE_TYPE"
    echo "     tamper_type      : firmware"
    echo "     resolution_status: detected"
    echo "     latitude         : $LATITUDE"
    echo "     longitude        : $LONGITUDE"
    echo "     city             : $CITY"
    echo "     state            : $STATE"
    echo "     drift            : $ZERO_DRIFT"
else
    echo "[ERROR] Failed to log to database"
fi

# 2. Update safe_mode to true in config.json
echo "[ACTION] Setting safe_mode = true in config.json..."
# Robust regex to handle spaces: "safe_mode" : false
sed -i 's/"safe_mode"[[:space:]]*:[[:space:]]*false/"safe_mode": true/' "$CONFIG_FILE"

if [ $? -eq 0 ]; then
    echo "[CONFIG] safe_mode updated to true"
else
    echo "[ERROR] Failed to update config.json"
fi

# 3. Stop weight service
echo "[ACTION] Stopping measure_weight.service..."
systemctl stop measure_weight.service
systemctl disable measure_weight.service

exit 1
