#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <stdbool.h>
#include "lcd.h"

// GPIO config
const char *chipname = "gpiochip1";
const unsigned int line_offset = 23; // GPIO1_B2_d

// LCD config
#define I2C_BUS "/dev/i2c-3"
#define I2C_ADDR 0x27

int main(void) {
    // --- LCD Initialization ---
    if (lcd_init(I2C_BUS, I2C_ADDR) != 0) {
        fprintf(stderr, "Failed to initialize LCD.\n");
        return 1;
    }
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("System Ready");
    usleep(1000000);
    lcd_clear();

    // --- GPIO Initialization ---
    struct gpiod_chip *chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip");
        return 1;
    }

    struct gpiod_line *line = gpiod_chip_get_line(chip, line_offset);
    if (! line) {
        perror("Error getting GPIO line");
        gpiod_chip_close(chip);
        return 1;
    }

    if (gpiod_line_request_input(line, "tamper_detect") < 0) {
        perror("Error requesting GPIO line");
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Monitoring for magnetic tamper.. .\n");

    bool tampered = false;

    // --- Main Loop ---
    while (1) {
        int value = gpiod_line_get_value(line);
        if (value < 0) {
            perror("Error reading GPIO");
            break;
        }

        // Tamper detected (HIGH)
        if (value == 1 && !tampered) {
            tampered = true;
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("SAFE MODE");
            lcd_set_cursor(1, 0);
            lcd_send_string("Remove Magnet");
            printf("SAFE MODE - Magnet detected!\n");
        }
        // Tamper cleared (LOW)
        else if (value == 0 && tampered) {
            tampered = false;
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("System Ready");
            printf("Normal mode restored.\n");
        }

        usleep(100000); // 100ms delay
    }

    // Cleanup
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    lcd_close();
    return 0;
}
