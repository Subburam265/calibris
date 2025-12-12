#!/bin/bash
# Calibris Remote Unlock Polling Service - FIXED VERSION
# Works with manual tamper detection (no systemd services for tamper monitoring)

# Configuration
API_BASE="https://unexploratory-harland-nontemporizingly.ngrok-free.dev/api"
DEVICE_ID=1
CONFIG_FILE="/home/pico/calibris/data/config.json"
LOG_FILE="/home/pico/calibris/data/unlock_poll.log"
POLL_INTERVAL=10  # seconds

# Colors for logging
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
NC='\033[0m' # No Color

# Logging function
log() {
  echo -e "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

is_safe_mode() {
  if [ ! -f "$CONFIG_FILE" ]; then
    log "${RED}Config file not found: $CONFIG_FILE${NC}"
    return 1
  fi

  # Simple check - works with any whitespace/tabs
  if grep -q '"safe_mode":[[:space:]]*true' "$CONFIG_FILE"; then
    log "${BLUE}[DEBUG] safe_mode = 'true'${NC}"
    return 0
  else
    log "${BLUE}[DEBUG] safe_mode = 'false'${NC}"
    return 1
  fi
}

# Exit safe mode - SIMPLIFIED for manual tamper detection setup
exit_safe_mode() {
  log "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  log "${BLUE}Executing safe mode exit sequence...${NC}"
  log "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

  # STEP 1: Update config.json to set safe_mode = false
  log "${YELLOW}[Step 1/3] Updating configuration file...${NC}"
  if [ -f "$CONFIG_FILE" ]; then
    # Backup config before modification
    cp "$CONFIG_FILE" "$CONFIG_FILE.backup"

    # Use sed to replace "safe_mode": true with "safe_mode": false
    sed -i 's/"safe_mode"[[:space:]]*:[[:space:]]*true/"safe_mode": false/' "$CONFIG_FILE"

    # Verify the change
    if grep -q '"safe_mode"[[:space:]]*:[[:space:]]*false' "$CONFIG_FILE"; then
      log "${GREEN}✓ Updated config.json (safe_mode = false)${NC}"
    else
      log "${RED}✗ Failed to update config.json${NC}"
      return 1
    fi
  else
    log "${RED}✗ Config file not found: $CONFIG_FILE${NC}"
    return 1
  fi

  # STEP 2: Stop safe_mode.service if it exists and is running
  log "${YELLOW}[Step 2/3] Stopping safe mode display service...${NC}"

  # Check if safe_mode.service exists
  if systemctl list-unit-files | grep -q "safe_mode.service"; then
    if systemctl is-active --quiet safe_mode.service 2>/dev/null; then
      sudo systemctl stop safe_mode.service
      sudo systemctl disable safe_mode.service 2>/dev/null
      log "${GREEN}✓ Stopped safe_mode.service${NC}"
    else
      log "${BLUE}  safe_mode.service is not running${NC}"
    fi
  else
    log "${BLUE}  safe_mode.service not installed (OK - manual setup)${NC}"
  fi

  # Also try to kill any running sm processes
  SM_PIDS=$(pgrep -f "/home/pico/calibris/safe_mode/sm")
  if [ -n "$SM_PIDS" ]; then
    log "${YELLOW}  Killing running safe mode display processes...${NC}"
    sudo kill $SM_PIDS 2>/dev/null
    log "${GREEN}✓ Killed safe mode display processes${NC}"
  fi

  # STEP 3: Re-enable and start measure_weight.service
  log "${YELLOW}[Step 3/3] Starting weight measurement service...${NC}"

  # Check if measure_weight.service exists
  if systemctl list-unit-files | grep -q "measure_weight.service"; then
    sudo systemctl enable measure_weight.service 2>/dev/null
    sudo systemctl start measure_weight.service

    if systemctl is-active --quiet measure_weight.service; then
      log "${GREEN}✓ Started measure_weight.service${NC}"
    else
      log "${RED}✗ Failed to start measure_weight.service${NC}"
    fi
  else
    log "${YELLOW}⚠  measure_weight.service not installed${NC}"
    log "${BLUE}  Attempting to start mw9 binary directly...${NC}"

    # Try to start the weight measurement binary directly
    if [ -f "/home/pico/calibris/hx711/mw9" ]; then
      nohup /home/pico/calibris/hx711/mw9 > /dev/null 2>&1 &
      log "${GREEN}✓ Started mw9 process in background${NC}"
    else
      log "${RED}✗ mw9 binary not found${NC}"
    fi
  fi

  log "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  log "${GREEN}✅ Safe mode exit complete - Device unlocked${NC}"
  log "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  log "${MAGENTA}NOTE: Tamper detection scripts are manual in your setup.${NC}"
  log "${MAGENTA}      They will not auto-restart. This is expected.${NC}"
  log "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

# Poll backend for unlock command - FIXED REGEX
check_unlock_status() {
  log "${BLUE}[DEBUG] Polling API...${NC}"
  
  # Make HTTP request to backend
  RESPONSE=$(curl -s --max-time 10 \
    -H "ngrok-skip-browser-warning: true" \
    "$API_BASE/devices/$DEVICE_ID/unlock-status")
  CURL_EXIT=$?

  # Check if curl succeeded
  if [ $CURL_EXIT -ne 0 ] || [ -z "$RESPONSE" ]; then
    log "${RED}✗ Network error - unable to reach backend (exit code: $CURL_EXIT)${NC}"
    return 1
  fi

  log "${BLUE}[DEBUG] Response: $RESPONSE${NC}"

  # Check if unlock is pending
  if echo "$RESPONSE" | grep -q '"unlock_pending"[[:space:]]*:[[:space:]]*true'; then
    # Extract command details - FIXED REGEX
    COMMAND_ID=$(echo "$RESPONSE" | grep -o '"command_id"[[:space:]]:[[:space:]][0-9]' | grep -o '[0-9]$')
    OFFICER_ID=$(echo "$RESPONSE" | grep -o '"officer_id"[[:space:]]:[[:space:]]"[^"]"' | sed 's/."\([^"]\)"./\1/')
    REASON=$(echo "$RESPONSE" | grep -o '"reason"[[:space:]]:[[:space:]]"[^"]"' | sed 's/."\([^"]\)"./\1/')

    log "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    log "${GREEN}🔓 UNLOCK COMMAND RECEIVED${NC}"
    log "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    log "   Command ID: ${BLUE}${COMMAND_ID:-N/A}${NC}"
    log "   Officer: ${BLUE}${OFFICER_ID:-N/A}${NC}"
    log "   Reason: ${BLUE}${REASON:-N/A}${NC}"
    log "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    # Exit safe mode
    exit_safe_mode

    # Wait for safe mode to fully exit
    sleep 2

    # Confirm unlock to backend
    log "${BLUE}Confirming unlock to backend...${NC}"
    CONFIRM_RESPONSE=$(curl -s --max-time 10 -X POST \
      "$API_BASE/devices/$DEVICE_ID/unlock-confirm" \
      -H "Content-Type: application/json" \
      -H "ngrok-skip-browser-warning: true" \
      -d "{\"command_id\": ${COMMAND_ID:-0}}")

    log "${BLUE}[DEBUG] Confirm response: $CONFIRM_RESPONSE${NC}"

    # Check if confirmation succeeded
    if echo "$CONFIRM_RESPONSE" | grep -q '"new_status".*"online"'; then
      log "${GREEN}✅ Unlock confirmed - Device status updated to ONLINE${NC}"
    else
      log "${YELLOW}⚠  Unlock executed but confirmation failed${NC}"
      log "   Response: $CONFIRM_RESPONSE"
    fi

    return 0
  else
    # No unlock pending
    log "${BLUE}ℹ No unlock command pending${NC}"
    return 1
  fi
}

# Main entry point
main() {
  log "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  log "${GREEN}🚀 Calibris Unlock Polling Service Started${NC}"
  log "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  log "   Device ID: $DEVICE_ID"
  log "   API: $API_BASE"
  log "   Poll Interval: ${POLL_INTERVAL}s"
  log "   Config: $CONFIG_FILE"
  log "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

  # Check initial state
  if is_safe_mode; then
    log "${YELLOW}⚠  Device is currently in SAFE MODE - will poll for unlock commands${NC}"
  else
    log "${GREEN}✓ Device is currently ONLINE - monitoring for safe mode entry${NC}"
  fi

  # Main loop
  while true; do
    # Only poll if device is in safe mode
    if is_safe_mode; then
      check_unlock_status
    fi

    # Wait before next poll
    sleep $POLL_INTERVAL
  done
}

# Run main function
main
