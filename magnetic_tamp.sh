#!/bin/sh

GPIO_PIN=71  # Change to your pin number
DB_PATH="/home/pico/mydata.db"
PRODUCT_ID_FILE="/home/pico/prod.id"
RENEWAL_CYCLE_FILE="/home/pico/cc.num"
TAMPER_TYPE="magnetic"

# Export GPIO and set as input
echo "$GPIO_PIN" > /sys/class/gpio/export 2>/dev/null
echo "in" > /sys/class/gpio/gpio${GPIO_PIN}/direction

RENEWAL_CYCLE=$(head -n 1 "$RENEWAL_CYCLE_FILE" | xargs)
# Read product ID safely
PRODUCT_ID=$(head -n 1 "$PRODUCT_ID_FILE" | xargs)
if [ -z "$PRODUCT_ID" ]; then
  echo "Product ID empty, exiting." >&2
  exit 1
fi

prev_state=0

while true; do
  state=$(cat /sys/class/gpio/gpio${GPIO_PIN}/value)
  
  if [ "$prev_state" -eq 0 ] && [ "$state" -eq 1 ]; then
    TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
    
    # Insert tamper event into SQLite log
    sqlite3 "$DB_PATH" <<EOF
INSERT INTO tamper_log (product_id, created_at, tamper_type, renewal_cycle)
VALUES ($PRODUCT_ID, '$TIMESTAMP', '$TAMPER_TYPE', '$RENEWAL_CYCLE');
EOF

    echo "$TIMESTAMP: Tamper detected on GPIO pin $GPIO_PIN!" >> /home/pico/tamper_log.txt
  fi
  
  prev_state=$state
  sleep 0.1  # poll every 100ms, adjust as needed
done
