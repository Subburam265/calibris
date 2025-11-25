#!/bin/sh

FILE="/home/pico/c_program/viable/v4"
TRUSTED_HASH="6e0c3ae9cc2a8379b6cefebee1b825b87fbd226a2c60cfc3b8a34aa58c04528b"
DB_PATH="/home/pico/mydata.db"
PRODUCT_ID_FILE="/home/pico/prod.id"
RENEWAL_CYCLE_FILE="/home/pico/cc.num"
TAMPER_TYPE="firmware"

# Read product id from file (trim whitespace)
PRODUCT_ID=$(head -n 1 "$PRODUCT_ID_FILE" | xargs)
RENEWAL_CYCLE=$(head -n 1 "$RENEWAL_CYCLE_FILE" | xargs)


CURRENT_HASH=$(sha256sum "$FILE" | awk '{print $1}')

if [ "$CURRENT_HASH" != "$TRUSTED_HASH" ]; then
  TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

  sqlite3 "$DB_PATH" <<EOF
INSERT INTO tamper_log (product_id, created_at, tamper_type, renewal_cycle)
VALUES ($PRODUCT_ID, '$TIMESTAMP', '$TAMPER_TYPE', '$RENEWAL_CYCLE');
EOF

  echo "$TIMESTAMP: Tampering detected in $FILE - logged into SQLite database with product_id=$PRODUCT_ID." >> /home/pico/tamper_log.txt
fi
