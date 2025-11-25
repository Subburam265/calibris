#!/bin/bash
# This script forcefully unexports the GPIO pins to release any kernel locks.

echo "Forcefully resetting GPIO pins 68 and 69..."

# Writing a pin's global number to 'unexport' tells the kernel to
# release any claims on it. The '2>/dev/null' part suppresses
# harmless errors if the pin isn't currently exported.
echo 68 > /sys/class/gpio/unexport 2>/dev/null
echo 69 > /sys/class/gpio/unexport 2>/dev/null

echo "Pins have been reset. You can now run your program."
