#!/bin/bash

# anna.sh - Luckfox tamper detection data sync script
# Syncs tamper logs from local SQLite to remote PostgreSQL backend

# Configuration
API_BASE="https://unexploratory-harland-nontemporizingly.ngrok-free.dev/api"  # UPDATE THIS with your ngrok URL
DEVICE_ID=1
LOCAL_DB="/home/pico/calibris/data/mydata.db"
LOG_FILE="/home/pico/calibris/data/sync.log"
LAST_SYNC_FILE="/home/pico/calibris/data/last_sync_id.txt"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to log messages
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

# Function to escape JSON strings (handles CSV quote escaping)
escape_json_string() {
    local str="$1"
    # Remove leading and trailing quotes if they exist
    str="${str#\"}"
    str="${str%\"}"
    # Escape backslashes first
    str="${str//\\/\\\\}"
    # Escape double quotes
    str="${str//\"/\\\"}"
    echo "$str"
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

# Main sync function
sync_tamper_logs() {
    log "${YELLOW}Starting tamper log sync...${NC}"

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

    # Query for new tamper logs (matching your actual database schema)
    QUERY="SELECT log_id, device_id, created_at, device_type, tamper_type,
                  resolution_status, settling_time, renewal_cycle,
                  latitude, longitude, city, state, drift, details,
                  prev_hash, curr_hash
           FROM tamper_logs
           WHERE log_id > $LAST_ID
           ORDER BY log_id ASC;"

    # Execute query and process results
    sqlite3 -csv "$LOCAL_DB" "$QUERY" | while IFS=',' read -r log_id device_id created_at device_type tamper_type \
                                                              resolution_status settling_time renewal_cycle \
                                                              latitude longitude city state drift details \
                                                              prev_hash curr_hash; do

        # Skip header row
        if [ "$log_id" = "log_id" ]; then
            continue
        fi

        log "${YELLOW}Processing tamper log ID: $log_id${NC}"

        # Escape JSON strings
        device_type=$(escape_json_string "$device_type")
        tamper_type=$(escape_json_string "$tamper_type")
        details=$(escape_json_string "$details")
        created_at=$(escape_json_string "$created_at")
        resolution_status=$(escape_json_string "$resolution_status")
        city=$(escape_json_string "$city")
        state=$(escape_json_string "$state")
        prev_hash=$(escape_json_string "$prev_hash")
        curr_hash=$(escape_json_string "$curr_hash")

        # Build JSON payload (no severity field in your DB, using device_type instead)
        JSON_PAYLOAD=$(cat <<EOF
{
  "tamper_type": "$tamper_type",
  "details": "$details",
  "event_time": "$created_at",
  "resolution_status": "$resolution_status",
  "settling_time": $settling_time,
  "renewal_cycle": $renewal_cycle,
  "latitude": $latitude,
  "longitude": $longitude,
  "city": "$city",
  "state": "$state",
  "drift": $drift,
  "prev_hash": "$prev_hash",
  "curr_hash": "$curr_hash",
  "luckfox_log_id": "$log_id"
}
EOF
)

        # Debug: Print JSON payload
        log "Sending JSON payload:"
        echo "$JSON_PAYLOAD" | tee -a "$LOG_FILE"

        # Send to API
        RESPONSE=$(curl -X POST \
            -H "Content-Type: application/json" \
            -d "$JSON_PAYLOAD" \
            -w "\nHTTP_CODE:%{http_code}" \
            -s \
            "$API_BASE/devices/$DEVICE_ID/tamper")

        # Parse response
        HTTP_CODE=$(echo "$RESPONSE" | grep "HTTP_CODE" | cut -d: -f2)
        BODY=$(echo "$RESPONSE" | sed '/HTTP_CODE/d')

        if [ "$HTTP_CODE" = "201" ] || [ "$HTTP_CODE" = "200" ]; then
            log "${GREEN}✓ Successfully synced log ID: $log_id${NC}"
            save_last_sync_id "$log_id"
        else
            log "${RED}✗ Failed to sync log ID: $log_id (HTTP $HTTP_CODE)${NC}"
            log "Response: $BODY"
            # Don't update last_sync_id on failure
            return 1
        fi

        # Small delay to avoid overwhelming the server
        sleep 0.5
    done

    log "${GREEN}Tamper log sync completed!${NC}"
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
