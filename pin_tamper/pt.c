/**
 * Integrated Tamper Monitor (Enclosure + Magnetic)
 * Solves "Resource Busy" by handling all GPIOs in one process using poll().
 */

#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>      // Crucial for monitoring multiple events
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/wait.h>
#include "lcd.h"       // Ensure lcd.h and lcd.c are in the compile path

// --- Config ---
#define CHIP1 "gpiochip1"
#define CHIP2 "gpiochip2"

// Enclosure Settings
#define ENC_PIN 21      // GPIO1_C5
#define ENC_NAME "enclosure_tamper"

// Magnetic Settings
#define MAG_IN_PIN  23  // GPIO1_C7 (Input)
#define MAG_OUT_PIN 22  // GPIO1_C6 (Mirror Output)
#define MAG_NAME "magnetic_tamper"

// Status Pin
#define STATUS_PIN 0    // GPIO2_A0 (Status Output)

// System Paths
#define CONFIG_FILE      "/home/pico/calibris/data/config.json"
#define TAMPER_LOG_BIN   "/home/pico/calibris/bin/tamper_log_bin/tamper_log"
#define SAFE_SERVICE     "safe_mode.service"
#define NORMAL_SERVICE   "measure_weight.service"

// --- Globals ---
volatile sig_atomic_t running = 1;
bool lcd_active = false;

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
    if (ret != 0) fprintf(stderr, "[System] Command failed: %s\n", cmd);
}

// --- Helper: Log Tamper ---
void log_tamper(const char *type, const char *details) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s --type %s --details \"%s\"", TAMPER_LOG_BIN, type, details);
    printf("[Log] %s\n", cmd);
    run_cmd(cmd);
}

// --- Action: Activate Safe Mode ---
void trigger_safe_mode(const char *reason) {
    printf("\n[!!!] TAMPER TRIGGERED: %s [!!!]\n", reason);
    
    // 1. Log it
    log_tamper(reason, "Hardware trigger detected");

    // 2. Stop Normal Service
    run_cmd("systemctl stop " NORMAL_SERVICE);

    // 3. Update Config (Atomic sed)
    run_cmd("sed -i 's/\"safe_mode\"[[:space:]]*:[[:space:]]*false/\"safe_mode\": true/' " CONFIG_FILE);

    // 4. Visual Warning (LCD)
    if (!lcd_active) {
        if (lcd_init("/dev/i2c-3", 0x27) == 0) {
            lcd_active = true;
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("!! TAMPERED !!");
            lcd_set_cursor(1, 0);
            lcd_send_string(reason);
        }
    }

    // 5. Start Safe Service
    run_cmd("systemctl enable --now " SAFE_SERVICE);
    
    // 6. Set Status LED/Pin High
    if (line_status) gpiod_line_set_value(line_status, 1);
}

// --- Action: Clear Magnetic Tamper ---
void clear_magnetic_tamper() {
    printf("[OK] Magnet Returned.\n");
    if (lcd_active) {
        lcd_clear();
        lcd_close();
        lcd_active = false;
    }
    // Note: We do NOT auto-revert safe_mode config. That requires manual admin intervention usually.
    // But we will restart the service as per your original logic:
    run_cmd("systemctl start " NORMAL_SERVICE);
    
    if (line_status) gpiod_line_set_value(line_status, 0);
}

// --- Setup GPIOs ---
int init_gpios() {
    // Open Chips
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

    // 2. Magnetic Input (Input, Both Edges)
    line_mag_in = gpiod_chip_get_line(chip1, MAG_IN_PIN);
    struct gpiod_line_request_config config_mag = {
        .consumer = "integ_mag",
        .request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES,
    };
    if (gpiod_line_request(line_mag_in, &config_mag, 0) < 0) return -1;

    // 3. Magnetic Mirror (Output)
    line_mag_out = gpiod_chip_get_line(chip1, MAG_OUT_PIN);
    if (gpiod_line_request_output(line_mag_out, "integ_mirror", 0) < 0) return -1;

    // 4. Status Pin (Output, Chip 2)
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
    if (lcd_active) lcd_close();
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Starting Integrated Tamper Monitor...\n");
    if (init_gpios() < 0) {
        perror("GPIO Init Failed");
        return 1;
    }

    // Prepare POLL structure to wait for events on two lines
    struct pollfd fds[2];
    fds[0].fd = gpiod_line_event_get_fd(line_enc);
    fds[0].events = POLLIN;
    fds[1].fd = gpiod_line_event_get_fd(line_mag_in);
    fds[1].events = POLLIN;

    while (running) {
        // Wait for event (timeout -1 = forever)
        int ret = poll(fds, 2, -1);

        if (ret > 0) {
            // --- CHECK ENCLOSURE ---
            if (fds[0].revents & POLLIN) {
                struct gpiod_line_event event;
                gpiod_line_event_read(line_enc, &event);
                if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
                    // Debounce
                    usleep(50000); 
                    if (gpiod_line_get_value(line_enc) == 0) {
                        trigger_safe_mode("Enclosure_Open");
                        sleep(2); // Anti-spam
                    }
                }
            }

            // --- CHECK MAGNETIC ---
            if (fds[1].revents & POLLIN) {
                struct gpiod_line_event event;
                gpiod_line_event_read(line_mag_in, &event);
                
                int val = gpiod_line_get_value(line_mag_in);
                
                // Mirror the value immediately
                gpiod_line_set_value(line_mag_out, val);

                if (val == 1) {
                    // Magnet REMOVED (High)
                    trigger_safe_mode("Magnetic_Tamper");
                } else {
                    // Magnet RETURNED (Low)
                    clear_magnetic_tamper();
                }
            }
        }
    }

    cleanup();
    return 0;
}
