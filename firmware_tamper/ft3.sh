#!/bin/sh
# firmware_tamper.sh - Firmware integrity checker

# --- Configuration ---
CONFIG_FILE="/home/pico/calibris/data/config.json"
DB_PATH="/home/pico/calibris/data/mydata.db"
FILE_TO_CHECK="/home/pico/calibris/hx711/mw7.c"
TRUSTED_HASH="4101b063f3d5f48890f0ff2e69208c575d7f94d8497093b3d967a07d17735a63"

# --- Read config values ---
DEVICE_ID=$(grep -o '"device_id":[^,]*' "$CONFIG_FILE" | grep -o '[0-9]*')
SITE_NAME=$(grep -o '"site_name":"[^"]*' "$CONFIG_FILE" | cut -d'"' -f4)
LATITUDE=$(grep -o '"latitude":[^,]*' "$CONFIG_FILE" | grep -o '[0-9. ]*')
LONGITUDE=$(grep -o '"longitude":[^,}]*' "$CONFIG_FILE" | grep -o '[0-9.]*')

# --- Check if file exists ---
if [ ! -f "$FILE_TO_CHECK" ]; then
    echo "[ERROR] File not found: $FILE_TO_CHECK"
    exit 1
fi

# --- Calculate current hash ---
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

# Build location string
LOCATION="$SITE_NAME, $LATITUDE, $LONGITUDE"

# 1. Log to SQLite database
sqlite3 "$DB_PATH" "INSERT INTO tamper_log (product_id, tamper_type, resolution_status, location) VALUES ($DEVICE_ID, 'firmware', 'detected', '$LOCATION');"

if [ $? -eq 0 ]; then
    echo "[DB] Tamper event logged"
else
    echo "[ERROR] Failed to log to database"
fi

# 2.  Update safe_mode to true in config.json
echo "[ACTION] Setting safe_mode = true in config.json..."
sed -i 's/"safe_mode":[[:space:]]*false/"safe_mode": true/' "$CONFIG_FILE"

if [ $? -eq 0 ]; then
    echo "[CONFIG] safe_mode updated to true"
else
    echo "[ERROR] Failed to update config. json"
fi

# 3. Stop weight service
echo "[ACTION] Stopping measure_weight.service..."
systemctl stop measure_weight.service
systemctl disable measure_weight.service

exit 1
