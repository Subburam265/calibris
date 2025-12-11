/**
 * Integrated Tamper Monitor (Enclosure + Magnetic)
 * Fixed: Magnetic Tamper is TEMPORARY (Simulated Safe Mode).
 * Fixed: Enclosure Tamper is PERMANENT (Real Safe Mode).
 * Fixed: Removed console spam on "Magnet Returned".
 */

#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/wait.h>
#include "lcd.h"

// --- Config ---
#define CHIP1 "gpiochip1"
#define CHIP2 "gpiochip2"

// Enclosure Settings (GPIO1_C5 -> Pin 21)
#define ENC_PIN 21      
// Magnetic Settings (Input: GPIO1_C7 -> Pin 23, Output: GPIO1_C6 -> Pin 22)
#define MAG_IN_PIN  23  
#define MAG_OUT_PIN 22  
// Status Pin (GPIO2_A0)
#define STATUS_PIN 0    

// System Paths
#define CONFIG_FILE      "/home/pico/calibris/data/config.json"
#define TAMPER_LOG_BIN   "/home/pico/calibris/bin/tamper_log_bin/tamper_log"
#define SAFE_SERVICE     "safe_mode.service"
#define NORMAL_SERVICE   "measure_weight.service"

// --- Globals ---
volatile sig_atomic_t running = 1;
bool lcd_active = false;
bool magnet_was_missing = false; // To track state and prevent spam

// GPIO Pointers
struct gpiod_chip *chip1 = NULL;
struct gpiod_chip *chip2 = NULL;
struct gpiod_line *line_enc = NULL;
struct gpiod_line *line_mag_in = NULL;
struct gpiod_line *line_mag_out = NULL;
struct gpiod_line *line_status = NULL;

// --- Signal Handler ---
void signal_handler(int signum) {
    (void)signum;
    running = 0;
}

// --- Helper: Execute System Command ---
void run_cmd(const char *cmd) {
    int ret = system(cmd);
    if (ret != 0) {
        // fprintf(stderr, "[System] Command failed: %s\n", cmd); // Optional debug
    }
}

// --- Helper: Log Tamper ---
void log_tamper(const char *type, const char *details) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s --type %s --details \"%s\"", TAMPER_LOG_BIN, type, details);
    printf("[Log] Executing tamper logger...\n");
    run_cmd(cmd);
}

// --- ACTION 1: Enclosure Tamper (PERMANENT SAFE MODE) ---
void handle_enclosure_tamper() {
    printf("\n[!!!] CRITICAL: Enclosure Breached! Locking down... [!!!]\n");

    // 1. Log it
    log_tamper("Enclosure_Tamper", "Case opened (GPIO1_C5)");

    // 2. Stop Normal Service
    run_cmd("systemctl stop " NORMAL_SERVICE);
    run_cmd("systemctl disable " NORMAL_SERVICE);

    // 3. Update Config PERMANENTLY
    run_cmd("sed -i 's/\"safe_mode\"[[:space:]]*:[[:space:]]*false/\"safe_mode\": true/' " CONFIG_FILE);

    // 4. Start Real Safe Mode Service
    run_cmd("systemctl enable --now " SAFE_SERVICE);

    // 5. Visual Warning
    if (lcd_init("/dev/i2c-3", 0x27) == 0) {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_send_string("SYSTEM LOCKED");
        lcd_set_cursor(1, 0);
        lcd_send_string("Contact Admin");
        lcd_close(); // We close it because safe_mode service might want it
    }
    
    // Exit this monitor? 
    // Usually yes, because safe_mode.service takes over. But we can keep running just in case.
}

// --- ACTION 2: Magnetic Tamper (TEMPORARY / SIMULATED) ---
void handle_magnetic_tamper(bool is_tampered) {
    
    // Case A: Magnet REMOVED (Tamper Detected)
    if (is_tampered) {
        if (!magnet_was_missing) { // Only run once when state changes
            printf("\n[WARNING] Magnetic Field Lost! Pausing system...\n");
            magnet_was_missing = true;

            // 1. Log it
            log_tamper("Magnetic_Tamper", "Magnet removed from sensor");

            // 2. Stop Normal Service (Temporary Pause)
            run_cmd("systemctl stop " NORMAL_SERVICE);

            // 3. Set Status Pins High
            if (line_status) gpiod_line_set_value(line_status, 1);
            if (line_mag_out) gpiod_line_set_value(line_mag_out, 1);

            // 4. Show LCD Warning (Keep open)
            if (!lcd_active) {
                if (lcd_init("/dev/i2c-3", 0x27) == 0) {
                    lcd_active = true;
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_send_string("!! SAFE MODE !!");
                    lcd_set_cursor(1, 0);
                    lcd_send_string("Remove Magnet");
                }
            }
        }
    } 
    // Case B: Magnet RETURNED (Tamper Cleared)
    else {
        if (magnet_was_missing) { // Only run once when state changes
            printf("\n[OK] Magnet Returned. Resuming system...\n");
            magnet_was_missing = false;

            // 1. Reset Status Pins
            if (line_status) gpiod_line_set_value(line_status, 0);
            if (line_mag_out) gpiod_line_set_value(line_mag_out, 0);

            // 2. Clear LCD
            if (lcd_active) {
                lcd_clear();
                lcd_close();
                lcd_active = false;
            }

            // 3. Restart Normal Service
            run_cmd("systemctl start " NORMAL_SERVICE);
        }
    }
}

// --- Setup GPIOs ---
int init_gpios() {
    chip1 = gpiod_chip_open_by_name(CHIP1);
    chip2 = gpiod_chip_open_by_name(CHIP2);
    if (!chip1 || !chip2) return -1;

    // 1. Enclosure (Input, Falling Edge)
    line_enc = gpiod_chip_get_line(chip1, ENC_PIN);
    struct gpiod_line_request_config config_enc = {
        .consumer = "integ_enc",
        .request_type = GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE,
    };
    if (gpiod_line_request(line_enc, &config_enc, 0) < 0) return -1;

    // 2. Magnetic Input (Input, Both Edges for state change)
    line_mag_in = gpiod_chip_get_line(chip1, MAG_IN_PIN);
    struct gpiod_line_request_config config_mag = {
        .consumer = "integ_mag",
        .request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES,
    };
    if (gpiod_line_request(line_mag_in, &config_mag, 0) < 0) return -1;

    // 3. Magnetic Mirror (Output)
    line_mag_out = gpiod_chip_get_line(chip1, MAG_OUT_PIN);
    if (gpiod_line_request_output(line_mag_out, "integ_mirror", 0) < 0) return -1;

    // 4. Status Pin (Output)
    line_status = gpiod_chip_get_line(chip2, STATUS_PIN);
    if (gpiod_line_request_output(line_status, "integ_status", 0) < 0) return -1;

    return 0;
}

// --- Cleanup ---
void cleanup() {
    if (line_enc) gpiod_line_release(line_enc);
    if (line_mag_in) gpiod_line_release(line_mag_in);
    if (line_mag_out) gpiod_line_release(line_mag_out);
    if (line_status) gpiod_line_release(line_status);
    if (chip1) gpiod_chip_close(chip1);
    if (chip2) gpiod_chip_close(chip2);
    if (lcd_active) {
        lcd_clear();
        lcd_close();
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Starting Integrated Tamper Monitor...\n");
    printf("Modes: Enclosure=PERMANENT, Magnetic=TEMPORARY\n");

    if (init_gpios() < 0) {
        perror("GPIO Init Failed");
        return 1;
    }

    // Initial State Check for Magnetic Sensor
    if (gpiod_line_get_value(line_mag_in) == 1) {
        magnet_was_missing = false; // Fake false to force a trigger update
        handle_magnetic_tamper(true);
    } else {
        magnet_was_missing = true; // Fake true to force a clear update
        handle_magnetic_tamper(false);
    }

    struct pollfd fds[2];
    fds[0].fd = gpiod_line_event_get_fd(line_enc);
    fds[0].events = POLLIN;
    fds[1].fd = gpiod_line_event_get_fd(line_mag_in);
    fds[1].events = POLLIN;

    while (running) {
        int ret = poll(fds, 2, -1);

        if (ret > 0) {
            // --- ENCLOSURE EVENT ---
            if (fds[0].revents & POLLIN) {
                struct gpiod_line_event event;
                gpiod_line_event_read(line_enc, &event);
                if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
                    usleep(50000); // Debounce
                    if (gpiod_line_get_value(line_enc) == 0) {
                        handle_enclosure_tamper();
                    }
                }
            }

            // --- MAGNETIC EVENT ---
            if (fds[1].revents & POLLIN) {
                struct gpiod_line_event event;
                gpiod_line_event_read(line_mag_in, &event);
                
                // Read current state (1=Tampered/Removed, 0=OK/Present)
                int val = gpiod_line_get_value(line_mag_in);

                // Always update the mirror pin
                gpiod_line_set_value(line_mag_out, val);

                // Handle Logic
                if (val == 1) {
                    handle_magnetic_tamper(true);
                } else {
                    handle_magnetic_tamper(false);
                }
            }
        }
    }

    cleanup();
    printf("[Shutdown] Integrated Monitor Stopped.\n");
    return 0;
}
