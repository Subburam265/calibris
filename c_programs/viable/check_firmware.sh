#!/bin/sh

FILE="/home/pico/c_programs/viable/v4.c"
TRUSTED_HASH="c6c7ae0787b0655589784f1779aa5d30557a3b287acefbde45cb1b252366c9a5"
DB_PATH="/home/pico/mydata.db"
PRODUCT_ID_FILE="/home/pico/prod.id"
RENEWAL_CYCLE_FILE="/home/pico/cc.num"
TAMPER_TYPE="firmware"

while true; do
  PRODUCT_ID=$(head -n 1 "$PRODUCT_ID_FILE" | xargs)
  RENEWAL_CYCLE=$(head -n 1 "$RENEWAL_CYCLE_FILE" | xargs)
  CURRENT_HASH=$(sha256sum "$FILE" | awk '{print $1}')
  TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

  echo "$TIMESTAMP: $CURRENT_HASH vs $TRUSTED_HASH"

  if [ "$CURRENT_HASH" != "$TRUSTED_HASH" ]; then
    sqlite3 "$DB_PATH" <<EOF
INSERT INTO tamper_log (product_id, created_at, tamper_type, renewal_cycle)
VALUES ($PRODUCT_ID, '$TIMESTAMP', '$TAMPER_TYPE', '$RENEWAL_CYCLE');
EOF

    echo "$TIMESTAMP: Tampering detected in $FILE - logged into SQLite database with product_id=$PRODUCT_ID." >> /home/pico/tamper_log.txt
  fi

  sleep 10  # Wait 10 seconds before next check
done

