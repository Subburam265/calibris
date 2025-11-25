#!/bin/bash

# List of GPIO pin numbers
gpio_pins=(42 43 55 54 53 52 58 59 48 49 50 51 71 145 144 70 69 68 67 65 72 73)

# Loop through each pin and run the command
for pin in "${gpio_pins[@]}"; do
  echo "Running gpio_control for GPIO pin $pin"
  echo "$pin" | sudo ./c_programs/gpio_control
  echo "-----------------------------"
done
