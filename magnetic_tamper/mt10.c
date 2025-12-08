/**
 * Magnetic Tamper Monitor for Calibris
 *
 * Simplified version - only handles GPIO-based magnetic tamper detection. 
 * Uses the tamper_log library for logging, blockchain, safe_mode, and service control.
 *
 * Compile: gcc -o mt7 mt7.c lcd.c -lgpiod -L../lib -ltamper_log -lsqlite3 -lssl -lcrypto
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include "lcd.h"
#include "../lib/tamper_log.h"

// --- GPIO Configuration ---
static const char *chipname = "gpiochip1";
static const unsigned int line_offset = 23;

// --- LCD Configuration ---
#define I2C_BUS  "/dev/i2c-3"
#define I2C_ADDR 0x27

// --- Global Variables ---
static volatile sig_atomic_t running = 1;
static struct gpiod_chip *chip = NULL;
static struct gpiod_line *line = NULL;

// --- Signal Handler for Clean Exit ---
static void signal_handler(int signum) {
    (void)signum;
    running = 0;
}

// --- Initialize GPIO ---
static int init_gpio(void) {
    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip");
        return -1;
    }

    line = gpiod_chip_get_line(chip, line_offset);
    if (!line) {
        perror("Error getting GPIO line");
        gpiod_chip_close(chip);
        return -1;
    }

    if (gpiod_line_request_input(line, "magnetic_tamper") < 0) {
        perror("Error requesting GPIO line as input");
        gpiod_chip_close(chip);
        return -1;
    }

    return 0;
}

// --- Cleanup Resources ---
static void cleanup(void) {
    if (line) {
        gpiod_line_release(line);
    }
    if (chip) {
        gpiod_chip_close(chip);
    }
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("System Stopped");
    lcd_close();
}

// --- Get Current Timestamp ---
static void get_timestamp(char *buf, size_t sz) {
    time_t now = time(NULL);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

// --- Main Program ---
int main(void) {
    TamperConfig config = {0};

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("==========================================\n");
    printf("  Magnetic Tamper Monitor - Calibris\n");
    printf("==========================================\n");

    // --- Load Configuration (for display purposes) ---
    printf("\n[Init] Loading configuration...\n");
    if (parse_config(DEFAULT_CONFIG_FILE, &config) != 0) {
        fprintf(stderr, "Failed to load configuration!\n");
        return 1;
    }
    printf("[OK] Device ID: %d, City: %s\n", config.device_id, config.city);

    // --- Initialize LCD ---
    printf("[Init] Initializing LCD...\n");
    if (lcd_init(I2C_BUS, I2C_ADDR) != 0) {
        fprintf(stderr, "Failed to initialize LCD!\n");
        return 1;
    }
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Tamper Monitor");
    lcd_set_cursor(1, 0);
    lcd_send_string(config.city);
    usleep(1500000);

    // --- Initialize GPIO ---
    printf("[Init] Initializing GPIO %s:%u...\n", chipname, line_offset);
    if (init_gpio() != 0) {
        fprintf(stderr, "Failed to initialize GPIO!\n");
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_send_string("GPIO Error!");
        return 1;
    }

    // --- Display Ready Status ---
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("System Ready");
    lcd_set_cursor(1, 0);
    char device_str[17];
    snprintf(device_str, sizeof(device_str), "ID:%d", config.device_id);
    lcd_send_string(device_str);

    printf("[Monitor] System ready.  Monitoring for magnetic tamper...\n");
    printf("[Monitor] Press Ctrl+C to exit.\n\n");

    bool tampered_state = false;

    // --- Main Monitoring Loop ---
    while (running) {
        int gpio_value = gpiod_line_get_value(line);
        if (gpio_value < 0) {
            perror("Error reading GPIO");
            break;
        }

        // --- TAMPER DETECTED (Rising Edge: LOW -> HIGH) ---
        if (gpio_value == 1 && !tampered_state) {
            tampered_state = true;

            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  WARNING: MAGNETIC TAMPER DETECTED!                   |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time: %s                             |\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            // Log tamper event using the library
            // This handles: DB insert, blockchain, safe_mode update, service stop
            TamperLogResult result = log_tamper("magnetic", "Magnet removed from sensor");

            if (result != TAMPER_LOG_SUCCESS) {
                fprintf(stderr, "[ERROR] Failed to log tamper: %s\n", tamper_log_strerror(result));
            }

            // Update LCD
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("!!  SAFE MODE !!");
            lcd_set_cursor(1, 0);
            lcd_send_string("Magnet Removed");
        }

        // --- TAMPER CLEARED (Falling Edge: HIGH -> LOW) ---
        else if (gpio_value == 0 && tampered_state) {
            tampered_state = false;

            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  OK: TAMPER CLEARED                                   |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time: %s                             |\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            // Update safe_mode to false and restart service
            update_safe_mode(DEFAULT_CONFIG_FILE, false);
            printf("[Action] Config updated: safe_mode = false\n");

            start_weight_service();

            // Update LCD
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("System Ready");
            lcd_set_cursor(1, 0);
            lcd_send_string(device_str);
        }

        usleep(100000);  // 100ms polling interval
    }

    // --- Cleanup ---
    printf("\n[Shutdown] Cleaning up...\n");
    cleanup();
    printf("[Shutdown] Goodbye!\n");

    return 0;
}
