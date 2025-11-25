#!/bin/sh

FILE="/home/pico/hx711/mw7.c"
TRUSTED_HASH="4101b063f3d5f48890f0ff2e69208c575d7f94d8497093b3d967a07d17735a63"
DB_PATH="/home/pico/mydata.db"
PRODUCT_ID_FILE="/home/pico/prod.id"
RENEWAL_CYCLE_FILE="/home/pico/cc.num"
TAMPER_TYPE="firmware"

PRODUCT_ID=$(head -n 1 "$PRODUCT_ID_FILE" | xargs)
RENEWAL_CYCLE=$(head -n 1 "$RENEWAL_CYCLE_FILE" | xargs)

CURRENT_HASH=$(sha256sum "$FILE" | awk '{print $1}')
echo "$CURRENT_HASH && $TRUSTED_HASH"
if [ "$CURRENT_HASH" != "$TRUSTED_HASH" ]; then
  TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

  sqlite3 "$DB_PATH" <<EOF
INSERT INTO tamper_log (product_id, created_at, tamper_type, renewal_cycle)
VALUES ($PRODUCT_ID, '$TIMESTAMP', '$TAMPER_TYPE', '$RENEWAL_CYCLE');
EOF

  echo "$TIMESTAMP: Tampering detected in $FILE - logged into SQLite database with product_id=$PRODUCT_ID." >> /home/pico/tamper_log.txt

  # Stop the measure_weight.service
  systemctl stop measure_weight.service

  # Show warning on LCD using your new lcdmsg utility
  /home/pico/firmware_tamper/lcd_msg "SAFE MODE" "Firmware Tamper"
fi
