#!/bin/bash

# Exit immediately if a command exits with a non-zero status.
set -e
# Treat unset variables as an error.
set -u
# Pipelines return the exit status of the last command to exit with a non-zero status.
set -o pipefail

# --- Fixes for Cron: Set Working Directory and AWS Credentials ---
cd "$(dirname "$0")"

# FIX: Removed space in ". aws"
export AWS_CONFIG_FILE="/home/pico/calibris/.aws/config"
export AWS_SHARED_CREDENTIALS_FILE="/home/pico/calibris/.aws/credentials"

# Configuration
CONFIG_FILE="/home/pico/calibris/data/config.json"
DB_FILE="mydata.db"
TABLE_NAME="tamper_logs"
PRIMARY_KEY_COLUMN="log_id"
NULL_COLUMN="pushed_at"
# FIX: Removed space in IP address "8.8.8. 8"
PING_HOST="8.8.8.8"
SYNC_DIR="/home/pico/calibris/data"
MASTER_CSV_FILENAME="tamper_logs.csv"
MASTER_CSV_PATH="$SYNC_DIR/$MASTER_CSV_FILENAME"
S3_BUCKET="calibris"
# FIX: Removed spaces in filename template
TEMP_CSV=$(mktemp /tmp/output.XXXXXX.csv)

# --- Read Device ID and Device Type from config.json ---
# IMPROVEMENT: Added error handling if config file is missing
if [ ! -f "$CONFIG_FILE" ]; then
    echo "[ERROR] Config file not found at $CONFIG_FILE"
    exit 1
fi

DEVICE_ID=$(grep -o '"device_id": *"[^"]*"' "$CONFIG_FILE" | cut -d'"' -f4)
DEVICE_TYPE=$(grep -o '"type": *"[^"]*"' "$CONFIG_FILE" | cut -d'"' -f4)

if [ -z "$DEVICE_ID" ]; then
    echo "[ERROR] Could not read device_id from config.json"
    DEVICE_ID="unknown"
fi

if [ -z "$DEVICE_TYPE" ]; then
    echo "[ERROR] Could not read device type from config.json"
    DEVICE_TYPE="unknown_device"
fi

# Convert device type to S3-friendly folder name (lowercase, replace spaces with underscores)
DEVICE_TYPE_FOLDER=$(echo "$DEVICE_TYPE" | tr '[:upper:]' '[:lower:]' | tr ' ' '_')

echo "Device ID: $DEVICE_ID"
echo "Device Type: $DEVICE_TYPE"
echo "S3 Folder: $DEVICE_TYPE_FOLDER"

# --- Cleanup function ---
cleanup() { rm -f "$TEMP_CSV"; }
trap cleanup EXIT

# --- Network check function ---
check_network() { ping -c 1 -W 2 "$PING_HOST" > /dev/null 2>&1; return $?; }

# --- Main Logic ---

# Step 1: Check for network connectivity.
if ! check_network; then
    echo "Network connection not available. Aborting update."
    exit 1
fi

# Step 2: Find the IDs of all rows that need to be pushed.
IDS_TO_PUSH=$(sqlite3 -list "$DB_FILE" "SELECT $PRIMARY_KEY_COLUMN FROM $TABLE_NAME WHERE $NULL_COLUMN IS NULL;")

if [ -z "$IDS_TO_PUSH" ]; then
    echo "No new records to push. Exiting."
    exit 0
fi

ID_LIST=$(echo "$IDS_TO_PUSH" | tr '\n' ',' | sed 's/,$//')
ROW_COUNT=$(echo "$IDS_TO_PUSH" | wc -l | xargs)
echo "Found $ROW_COUNT records to push."

# Step 3: Generate timestamp for this batch.
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
echo "Generated timestamp for this batch: $TIMESTAMP"

# Step 4: Export records to a temporary CSV.
echo "Exporting data to temporary CSV..."
# Note: Ensure the columns here match your DB schema exactly.
SQL_QUERY="SELECT log_id, device_id, created_at, device_type, tamper_type, resolution_status, settling_time, renewal_cycle, latitude, longitude, city, state, drift, details, '$TIMESTAMP' AS $NULL_COLUMN FROM $TABLE_NAME WHERE $PRIMARY_KEY_COLUMN IN ($ID_LIST);"

sqlite3 -header -csv "$DB_FILE" "$SQL_QUERY" > "$TEMP_CSV"

# Step 5: Append new data to master CSV file.
echo "Appending new data to master file: $MASTER_CSV_PATH"
mkdir -p "$SYNC_DIR"
# FIX: Removed extra space in "[ !  -f ... ]"
if [ ! -f "$MASTER_CSV_PATH" ]; then
    mv "$TEMP_CSV" "$MASTER_CSV_PATH"
else
    # Append without header (tail -n +2 skips the header of the temp file)
    tail -n +2 "$TEMP_CSV" >> "$MASTER_CSV_PATH"
fi

# Step 6: Sync to AWS S3.
S3_DESTINATION="s3://$S3_BUCKET/$DEVICE_TYPE_FOLDER/$DEVICE_ID/"
echo "Uploading to: $S3_DESTINATION"

# SECURITY IMPROVEMENT:
# Originally, 'aws s3 sync' on $SYNC_DIR would also upload your config.json (which contains secrets).
# We now specifically include only the CSV file.
aws s3 sync "$SYNC_DIR" "$S3_DESTINATION" --exclude "*" --include "$MASTER_CSV_FILENAME"

# Step 7: Update 'pushed_at' timestamp.
echo "Successfully synced to $S3_DESTINATION"
echo "Updating 'pushed_at' timestamp for $ROW_COUNT records..."
sqlite3 "$DB_FILE" "UPDATE $TABLE_NAME SET $NULL_COLUMN = '$TIMESTAMP' WHERE $PRIMARY_KEY_COLUMN IN ($ID_LIST);"

echo "Process completed successfully!"
