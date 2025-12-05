#!/bin/bash

# Exit immediately if a command exits with a non-zero status.
set -e
# Treat unset variables as an error.
set -u
# Pipelines return the exit status of the last command to exit with a non-zero status.
set -o pipefail

echo "=== Sync Tamper Data Script Started ==="

# --- Fixes for Cron: Set Working Directory and AWS Credentials ---
cd "$(dirname "$0")"
echo "[INFO] Working directory: $(pwd)"

export AWS_CONFIG_FILE="/home/pico/calibris/.aws/config"
export AWS_SHARED_CREDENTIALS_FILE="/home/pico/calibris/.aws/credentials"

# Configuration
CONFIG_FILE="/home/pico/calibris/data/config.json"
DB_FILE="/home/pico/calibris/data/mydata.db"
TABLE_NAME="tamper_logs"
PRIMARY_KEY_COLUMN="log_id"
NULL_COLUMN="pushed_at"
# FIX: Removed space in IP address "8. 8.8.8"
PING_HOST="8.8.8.8"
SYNC_DIR="/home/pico/calibris/data"
MASTER_CSV_FILENAME="tamper_logs.csv"
MASTER_CSV_PATH="$SYNC_DIR/$MASTER_CSV_FILENAME"
S3_BUCKET="calibris"
# FIX: Removed spaces in mktemp template
TEMP_CSV=$(mktemp /tmp/output.XXXXXX.csv)

# --- Check if config file exists ---
if [ ! -f "$CONFIG_FILE" ]; then
    echo "[ERROR] Config file not found at $CONFIG_FILE"
    exit 1
fi
echo "[OK] Config file found"

# --- Check if database file exists ---
if [ ! -f "$DB_FILE" ]; then
    echo "[ERROR] Database file not found at $DB_FILE"
    exit 1
fi
echo "[OK] Database file found"

# --- Read Device ID and Device Type from config.json ---
# FIX: Added '|| true' to prevent 'set -e' from killing the script if grep finds nothing
DEVICE_ID=$(grep -o '"device_id":[^,]*' "$CONFIG_FILE" | grep -o '[0-9]*' || true)
DEVICE_TYPE=$(grep -o '"type":[[:space:]]*"[^"]*"' "$CONFIG_FILE" | cut -d'"' -f4 || true)

if [ -z "$DEVICE_ID" ]; then
    echo "[ERROR] Could not read device_id from config.json"
    DEVICE_ID="unknown"
fi

if [ -z "$DEVICE_TYPE" ]; then
    echo "[ERROR] Could not read device type from config.json"
    DEVICE_TYPE="unknown_device"
fi

# Convert device type to S3-friendly folder name
DEVICE_TYPE_FOLDER=$(echo "$DEVICE_TYPE" | tr '[:upper:]' '[:lower:]' | tr ' ' '_')

echo "[INFO] Device ID: $DEVICE_ID"
echo "[INFO] Device Type: $DEVICE_TYPE"
echo "[INFO] S3 Folder: $DEVICE_TYPE_FOLDER"

# --- Cleanup function ---
cleanup() { rm -f "$TEMP_CSV"; }
trap cleanup EXIT

# --- Network check function ---
check_network() { ping -c 1 -W 2 "$PING_HOST" > /dev/null 2>&1; return $?; }

# --- Main Logic ---

# Step 1: Check for network connectivity.
echo "[INFO] Checking network connectivity..."
if ! check_network; then
    echo "[ERROR] Network connection not available. Aborting."
    exit 1
fi
echo "[OK] Network is available"

# Step 2: Find the IDs of all rows that need to be pushed.
echo "[INFO] Querying database for unsynced records..."
# FIX: Ensure sqlite3 failure is caught properly
IDS_TO_PUSH=$(sqlite3 -list "$DB_FILE" "SELECT $PRIMARY_KEY_COLUMN FROM $TABLE_NAME WHERE $NULL_COLUMN IS NULL;" 2>&1) || {
    echo "[ERROR] SQLite query failed: $IDS_TO_PUSH"
    exit 1
}

if [ -z "$IDS_TO_PUSH" ]; then
    echo "[INFO] No new records to push. Exiting."
    exit 0
fi

ID_LIST=$(echo "$IDS_TO_PUSH" | tr '\n' ',' | sed 's/,$//')
ROW_COUNT=$(echo "$IDS_TO_PUSH" | wc -l | xargs)
echo "[INFO] Found $ROW_COUNT records to push (IDs: $ID_LIST)"

# Step 3: Generate timestamp for this batch.
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
echo "[INFO] Timestamp for this batch: $TIMESTAMP"

# Step 4: Export records to a temporary CSV.
echo "[INFO] Exporting data to temporary CSV..."
SQL_QUERY="SELECT log_id, device_id, created_at, device_type, tamper_type, resolution_status, settling_time, renewal_cycle, latitude, longitude, city, state, drift, details, '$TIMESTAMP' AS $NULL_COLUMN FROM $TABLE_NAME WHERE $PRIMARY_KEY_COLUMN IN ($ID_LIST);"

sqlite3 -header -csv "$DB_FILE" "$SQL_QUERY" > "$TEMP_CSV"
echo "[OK] Exported to $TEMP_CSV"

# Step 5: Append new data to master CSV file.
echo "[INFO] Appending to master file: $MASTER_CSV_PATH"
mkdir -p "$SYNC_DIR"
# FIX: Removed bad spacing in '[ ! -f ... ]'
if [ ! -f "$MASTER_CSV_PATH" ]; then
    echo "[INFO] Creating new master CSV file"
    mv "$TEMP_CSV" "$MASTER_CSV_PATH"
else
    echo "[INFO] Appending to existing master CSV file"
    tail -n +2 "$TEMP_CSV" >> "$MASTER_CSV_PATH"
fi
echo "[OK] Master CSV updated"

# Step 6: Sync to AWS S3.
S3_DESTINATION="s3://$S3_BUCKET/$DEVICE_TYPE_FOLDER/$DEVICE_ID/"
echo "[INFO] Uploading to: $S3_DESTINATION"
aws s3 sync "$SYNC_DIR" "$S3_DESTINATION" --exclude "*" --include "$MASTER_CSV_FILENAME"
echo "[OK] S3 sync completed"

# Step 7: Update 'pushed_at' timestamp.
echo "[INFO] Updating 'pushed_at' timestamp for $ROW_COUNT records..."
sqlite3 "$DB_FILE" "UPDATE $TABLE_NAME SET $NULL_COLUMN = '$TIMESTAMP' WHERE $PRIMARY_KEY_COLUMN IN ($ID_LIST);"
echo "[OK] Database updated"

echo "=== Process completed successfully! ==="
