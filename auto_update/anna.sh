#!/bin/bash

# anna.sh - Luckfox tamper detection data sync script (BATCH MODE)
# Syncs tamper logs from local SQLite to remote PostgreSQL backend

# Configuration
API_BASE="https://calibris-fullstack-production.up.railway.app/api"  # UPDATE THIS with your ngrok URL
DEVICE_ID=1
LOCAL_DB="/home/pico/calibris/data/mydata.db"
LOG_FILE="/home/pico/calibris/data/sync.log"
LAST_SYNC_FILE="/home/pico/calibris/data/last_sync_id.txt"
TEMP_FILE="/tmp/anna_sync_$$.json"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to log messages
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

# Function to check internet connectivity
check_internet() {
    if ping -c 1 8.8.8.8 &> /dev/null; then
        return 0
    else
        log "${RED}No internet connection${NC}"
        return 1
    fi
}

# Get last synced ID
get_last_sync_id() {
    if [ -f "$LAST_SYNC_FILE" ]; then
        cat "$LAST_SYNC_FILE"
    else
        echo "0"
    fi
}

# Save last synced ID
save_last_sync_id() {
    echo "$1" > "$LAST_SYNC_FILE"
}

# Escape JSON string
escape_json() {
    local str="$1"
    # Remove leading/trailing quotes
    str="${str#\"}"
    str="${str%\"}"
    # Escape backslashes and quotes
    str="${str//\\/\\\\}"
    str="${str//\"/\\\"}"
    # Handle newlines
    str="${str//$'\n'/\\n}"
    echo "$str"
}

# Main batch sync function
sync_tamper_logs() {
    log "${YELLOW}Starting BATCH tamper log sync...${NC}"

    # Check internet
    if ! check_internet; then
        log "${RED}Sync aborted: No internet${NC}"
        return 1
    fi

    # Check if database exists
    if [ ! -f "$LOCAL_DB" ]; then
        log "${RED}Database not found: $LOCAL_DB${NC}"
        return 1
    fi

    # Get last synced ID
    LAST_ID=$(get_last_sync_id)
    log "Last synced ID: $LAST_ID"

    # Query for new tamper logs
    QUERY="SELECT log_id, device_id, created_at, device_type, tamper_type,
                  resolution_status, settling_time, renewal_cycle,
                  latitude, longitude, city, state, drift, details,
                  prev_hash, curr_hash
           FROM tamper_logs
           WHERE log_id > $LAST_ID
           ORDER BY log_id ASC;"

    # Count new logs
    COUNT=$(sqlite3 "$LOCAL_DB" "SELECT COUNT(*) FROM tamper_logs WHERE log_id > $LAST_ID;")

    if [ "$COUNT" -eq 0 ]; then
        log "${GREEN}No new tamper logs to sync${NC}"
        return 0
    fi

    log "${YELLOW}Found $COUNT new tamper logs to sync${NC}"

    # Export data to temp file and process
    sqlite3 -csv "$LOCAL_DB" "$QUERY" > "$TEMP_FILE.csv"

    # Build JSON array
    JSON_ARRAY="["
    FIRST=true
    MAX_LOG_ID=0

    while IFS=',' read -r log_id device_id created_at device_type tamper_type \
                                  resolution_status settling_time renewal_cycle \
                                  latitude longitude city state drift details \
                                  prev_hash curr_hash; do
        # Skip header
        if [ "$log_id" = "log_id" ]; then
            continue
        fi

        # Escape fields
        device_type=$(escape_json "$device_type")
        tamper_type=$(escape_json "$tamper_type")
        details=$(escape_json "$details")
        created_at=$(escape_json "$created_at")
        resolution_status=$(escape_json "$resolution_status")
        city=$(escape_json "$city")
        state=$(escape_json "$state")
        prev_hash=$(escape_json "$prev_hash")
        curr_hash=$(escape_json "$curr_hash")

        # Add comma if not first
        if [ "$FIRST" = false ]; then
            JSON_ARRAY="$JSON_ARRAY,"
        fi
        FIRST=false

        # Add log object to array
        JSON_ARRAY="$JSON_ARRAY{\"tamper_type\":\"$tamper_type\",\"details\":\"$details\",\"event_time\":\"$created_at\",\"resolution_status\":\"$resolution_status\",\"settling_time\":$settling_time,\"renewal_cycle\":$renewal_cycle,\"latitude\":$latitude,\"longitude\":$longitude,\"city\":\"$city\",\"state\":\"$state\",\"drift\":$drift,\"prev_hash\":\"$prev_hash\",\"curr_hash\":\"$curr_hash\",\"luckfox_log_id\":\"$log_id\"}"

        # Track max log_id
        if [ "$log_id" -gt "$MAX_LOG_ID" ]; then
            MAX_LOG_ID=$log_id
        fi
    done < "$TEMP_FILE.csv"

    JSON_ARRAY="$JSON_ARRAY]"

    # Clean up temp file
    rm -f "$TEMP_FILE.csv"

    # If no logs were processed, exit
    if [ "$MAX_LOG_ID" -eq 0 ]; then
        log "${YELLOW}No new logs to process${NC}"
        return 0
    fi

    # Build final payload
    JSON_PAYLOAD="{\"logs\":$JSON_ARRAY}"

    # Debug output
    log "Sending batch to API (${COUNT} logs)..."
    log "Max log_id in batch: $MAX_LOG_ID"

    # Save to temp file for debugging
    echo "$JSON_PAYLOAD" > "$TEMP_FILE"

    # Send batch to API
    RESPONSE=$(curl -X POST \
        -H "Content-Type: application/json" \
        -d @"$TEMP_FILE" \
        -w "\nHTTP_CODE:%{http_code}" \
        -s \
        "$API_BASE/devices/$DEVICE_ID/tamper/batch")

    # Clean up temp file
    rm -f "$TEMP_FILE"

    # Parse response
    HTTP_CODE=$(echo "$RESPONSE" | grep "HTTP_CODE" | cut -d: -f2)
    BODY=$(echo "$RESPONSE" | sed '/HTTP_CODE/d')

    if [ "$HTTP_CODE" = "201" ] || [ "$HTTP_CODE" = "200" ]; then
        log "${GREEN}✓ Successfully synced ${COUNT} tamper logs (batch)${NC}"
        save_last_sync_id "$MAX_LOG_ID"
        log "Updated last sync ID to: $MAX_LOG_ID"
    else
        log "${RED}✗ Failed to sync batch (HTTP $HTTP_CODE)${NC}"
        log "Response: $BODY"
        return 1
    fi

    log "${GREEN}Batch sync completed!${NC}"
    return 0
}

# Run sync
sync_tamper_logs

# Exit with appropriate code
if [ $? -eq 0 ]; then
    exit 0
else
    exit 1
fi
