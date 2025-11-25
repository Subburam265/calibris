#!/usr/bin/env python3
import time
import sys

# Define GPIO pins for HX711
GPIO_DOUT = 69
GPIO_SCK = 68

# --- Sysfs GPIO helper functions (no changes here) ---
def gpio_export(pin):
    try:
        with open("/sys/class/gpio/export", "w") as f:
            f.write(str(pin))
    except (IOError, PermissionError):
        # Already exported, or permission issue
        # print(f"Warning: Could not export pin {pin}. Already exported?")
        pass

def gpio_unexport(pin):
    try:
        with open("/sys/class/gpio/unexport", "w") as f:
            f.write(str(pin))
    except (IOError, PermissionError):
        print(f"Warning: Could not unexport pin {pin}.")

def gpio_set_dir(pin, direction):
    with open(f"/sys/class/gpio/gpio{pin}/direction", "w") as f:
        f.write(direction)

def gpio_set_value(pin, value):
    with open(f"/sys/class/gpio/gpio{pin}/value", "w") as f:
        f.write("1" if value else "0")

def gpio_get_value(pin):
    with open(f"/sys/class/gpio/gpio{pin}/value", "r") as f:
        return int(f.read().strip())

# --- Setup and Cleanup ---
def setup():
    print("Setting up GPIO pins...")
    gpio_export(GPIO_DOUT)
    gpio_export(GPIO_SCK)
    # Allow a moment for sysfs to create the directories
    time.sleep(0.1)
    gpio_set_dir(GPIO_DOUT, "in")
    gpio_set_dir(GPIO_SCK, "out")
    gpio_set_value(GPIO_SCK, 0)
    print("Setup complete.")

def cleanup():
    print("\nCleaning up GPIO...")
    gpio_unexport(GPIO_DOUT)
    gpio_unexport(GPIO_SCK)
    print("Cleanup complete.")

# --- MODIFIED HX711 Read Function ---
def hx711_read():
    """
    Reads a 24-bit value from the HX711.
    Returns the raw integer value or None on timeout.
    """
    # 1. Wait for the DOUT pin to go LOW
    ready_timeout = time.time() + 0.5  # 500ms timeout
    while gpio_get_value(GPIO_DOUT) == 1:
        if time.time() > ready_timeout:
            print("Error: Timeout waiting for HX711 to become ready.")
            return None
    
    # 2. Read the 24 bits of data
    count = 0
    # The delays from sysfs I/O are often sufficient,
    # so we remove the explicit time.sleep() calls here.
    for _ in range(24):
        gpio_set_value(GPIO_SCK, 1)
        count = count << 1
        gpio_set_value(GPIO_SCK, 0)
        if gpio_get_value(GPIO_DOUT):
            count += 1

    # 3. Send one more pulse to set gain for the next reading
    gpio_set_value(GPIO_SCK, 1)
    gpio_set_value(GPIO_SCK, 0)

    # 4. Check if the value is negative (using 2's complement)
    if (count & 0x800000):
        count |= ~0xffffff

    return count

# --- Main Program Execution ---
if __name__ == "__main__":
    try:
        setup()
        # Give the sensor time to settle
        time.sleep(1)
        
        while True:
            val = hx711_read()
            if val is not None:
                print(f"Raw weight data: {val}")
            else:
                # If a timeout occurred, wait before retrying
                print("Retrying after timeout...")
            time.sleep(0.5)

    except KeyboardInterrupt:
        print("Exiting program.")
    except Exception as e:
        print(f"An error occurred: {e}")
    finally:
        cleanup()
