#!/bin/bash

# Constantly monitors a GPIO pin for a reed switch closure (tamper event)
# and logs it to an SQLite database.

# --- Configuration ---
GPIO_PIN=71
DB_FILE="/home/pico/mydata.db"
PRODUCT_ID_FILE="/home/pico/prod.id"
CHECK_INTERVAL=0.5

# --- Function to unexport GPIO on exit ---
cleanup() {
    echo -e "\nCleaning up and unexporting GPIO ${GPIO_PIN}..."
    if [ -d "/sys/class/gpio/gpio${GPIO_PIN}" ]; then
        echo ${GPIO_PIN} > /sys/class/gpio/unexport
    fi
    exit 0
}

trap cleanup SIGINT EXIT

# --- Setup GPIO Pin ---
echo "Setting up GPIO pin ${GPIO_PIN}..."
if [ ! -d "/sys/class/gpio/gpio${GPIO_PIN}" ]; then
    echo ${GPIO_PIN} > /sys/class/gpio/export
    sleep 0.5
fi

# Configure pin as an input
echo "in" > "/sys/class/gpio/gpio${GPIO_PIN}/direction"

# --- Main Monitoring Loop ---
echo "Starting tamper detection loop. This will run until stopped."

TAMPER_STATE=0 # 0 = not tampered (secure), 1 = tampered

while true; do
    # Read the direct pin value
    CURRENT_VALUE=$(cat "/sys/class/gpio/gpio${GPIO_PIN}/value")

    # REVERSED LOGIC: GPIO value 1 (HIGH) now indicates tamper (magnet removed)
    if [ "${CURRENT_VALUE}" -eq 1 ] && [ "${TAMPER_STATE}" -eq 0 ]; then
        # --- TAMPER DETECTED ---
        TIMESTAMP=$(date +"%Y-%m-%d %H:%M:%S")
        echo "[${TIMESTAMP}] TAMPER DETECTED! Reed switch open (magnet removed)."

        TAMPER_STATE=1

        if [ -f "${PRODUCT_ID_FILE}" ]; then
            PRODUCT_ID=$(cat "${PRODUCT_ID_FILE}")
        else
            PRODUCT_ID="UNKNOWN"
        fi

        sqlite3 "${DB_FILE}" "INSERT INTO tamper_log (product_id, tamper_type) VALUES ('${PRODUCT_ID}', 'magnetic');"
        echo "--> Logged magnetic tamper event for product ID '${PRODUCT_ID}' to the database."

    # REVERSED LOGIC: GPIO value 0 (LOW) now indicates secure (magnet present)
    elif [ "${CURRENT_VALUE}" -eq 0 ] && [ "${TAMPER_STATE}" -eq 1 ]; then
        # --- TAMPER RESOLVED ---
        TIMESTAMP=$(date +"%Y-%m-%d %H:%M:%S")
        echo "[${TIMESTAMP}] Tamper condition resolved. Reed switch closed (magnet present)."
        TAMPER_STATE=0
    fi

    sleep ${CHECK_INTERVAL}
done
