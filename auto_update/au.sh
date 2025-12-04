#!/bin/bash

# Exit immediately if a command exits with a non-zero status.
set -e
# Treat unset variables as an error.
set -u
# Pipelines return the exit status of the last command to exit with a non-zero status.
set -o pipefail

# --- Fixes for Cron: Set Working Directory and AWS Credentials ---
cd "$(dirname "$0")"
export AWS_CONFIG_FILE="/home/pico/.aws/config"
export AWS_SHARED_CREDENTIALS_FILE="/home/pico/.aws/credentials"

# Configuration
DB_FILE="mydata.db"
# ... (rest of configuration is the same) ...
TABLE_NAME="tamper_log"
PRIMARY_KEY_COLUMN="log_id"
NULL_COLUMN="pushed_at"
PING_HOST="8.8.8.8"
SYNC_DIR="/home/pico/csv"
MASTER_CSV_FILENAME="logs.csv"
MASTER_CSV_PATH="$SYNC_DIR/$MASTER_CSV_FILENAME"
S3_BUCKET="calibris"
S3_PATH="/"
TEMP_CSV=$(mktemp /tmp/output.XXXXXX.csv)


# --- Script starts here ---
# (cleanup and check_network functions are the same)
cleanup() { rm -f "$TEMP_CSV"; }
trap cleanup EXIT
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

# --- NEW Step 3: Get the GEOGRAPHIC Location using jq ---
# This is the most reliable method. It requires 'jq' to be installed.
# It makes one network request and formats the output string in a guaranteed order.
GEOGRAPHIC_LOCATION=$(curl -s ipinfo.io | jq -r '.loc + "|" + .city + "|" + .region')

if [ -z "$GEOGRAPHIC_LOCATION" ]; then
    GEOGRAPHIC_LOCATION="unknown"
fi
echo "Current device location (Geographic) for this batch: $GEOGRAPHIC_LOCATION"

# --- NEW Step 4: Update the Location in the Database ---
echo "Updating location for $ROW_COUNT records in the database..."
sqlite3 "$DB_FILE" "UPDATE $TABLE_NAME SET location = '$GEOGRAPHIC_LOCATION' WHERE $PRIMARY_KEY_COLUMN IN ($ID_LIST);"

# Step 5: Generate timestamp for this batch.
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
echo "Generated timestamp for this batch: $TIMESTAMP"

# Step 6: Export records to a temporary CSV.
echo "Exporting timestamped data to temporary CSV..."
SQL_QUERY="SELECT log_id, product_id, created_at, tamper_type, resolution_status, settling_time, renewal_cycle, location, CASE WHEN $NULL_COLUMN IS NULL THEN '$TIMESTAMP' ELSE $NULL_COLUMN END AS $NULL_COLUMN FROM $TABLE_NAME WHERE $PRIMARY_KEY_COLUMN IN ($ID_LIST);"
sqlite3 -header -csv "$DB_FILE" "$SQL_QUERY" > "$TEMP_CSV"

# Step 7: Append new data to master CSV file.
echo "Appending new data to master file: $MASTER_CSV_PATH"
mkdir -p "$SYNC_DIR"
if [ ! -f "$MASTER_CSV_PATH" ]; then
    mv "$TEMP_CSV" "$MASTER_CSV_PATH"
else
    tail -n +2 "$TEMP_CSV" >> "$MASTER_CSV_PATH"
fi

# Step 8: Sync to AWS S3.
echo "Syncing directory $SYNC_DIR to S3..."
S3_DESTINATION="s3://$S3_BUCKET$S3_PATH"
aws s3 sync "$SYNC_DIR" "$S3_DESTINATION"

# Step 9: Update 'pushed_at' timestamp.
echo "Successfully synced directory to $S3_DESTINATION"
echo "Updating 'pushed_at' timestamp for $ROW_COUNT records..."
sqlite3 "$DB_FILE" "UPDATE $TABLE_NAME SET $NULL_COLUMN = '$TIMESTAMP' WHERE $PRIMARY_KEY_COLUMN IN ($ID_LIST);"

echo "Process completed successfully!"
