/**
 * Magnetic Tamper Monitor for Calibris
 *
 * Compile with: gcc -o mt_monitor mt_monitor.c ../lcd/lcd.c -I../lcd -lgpiod
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "lcd.h"

// --- File Paths ---
#define CONFIG_FILE      "/home/pico/calibris/data/config.json"
#define TAMPER_LOG_BIN   "/home/pico/calibris/bin/tamper_log_bin/tamper_log"

// --- GPIO Configuration ---
const char *chipname = "gpiochip1";
const unsigned int line_offset = 23;

// --- LCD Configuration ---
#define I2C_BUS  "/dev/i2c-3"
#define I2C_ADDR 0x27

// --- Global Variables ---
volatile sig_atomic_t running = 1;
bool lcd_is_active = false; // Track if LCD is currently initialized
struct gpiod_chip *chip = NULL;
struct gpiod_line *line = NULL;

// --- Configuration Structure ---
typedef struct {
    int device_id;
    char device_type[64];
    double calibration_factor;
    long tare_offset;
    double zero_drift;
    double max_zero_drift_threshold;
    bool safe_mode;
    double latitude;
    double longitude;
    char city[64];
    char state[64];
    char last_updated[64];
} Config;

// --- Signal Handler for Clean Exit ---
void signal_handler(int signum) {
    (void)signum;
    running = 0;
}

// --- Helper: Extract string value from JSON line ---
void extract_json_string(const char *line, const char *key, char *dest, size_t dest_size) {
    char *pos = strstr(line, key);
    if (pos) {
        char *start = strchr(pos, ':');
        if (start) {
            // Find opening quote
            start = strchr(start, '"');
            if (start) {
                start++; // Move past quote
                char *end = strchr(start, '"');
                if (end) {
                    size_t len = end - start;
                    if (len >= dest_size) len = dest_size - 1;
                    strncpy(dest, start, len);
                    dest[len] = '\0';
                }
            }
        }
    }
}

// --- Parse config.json ---
int parse_config(const char *filepath, Config *config) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        perror("Failed to open config file");
        return -1;
    }

    // Initialize defaults
    config->device_id = 0;
    strcpy(config->device_type, "Unknown");
    config->calibration_factor = 0.0;
    config->tare_offset = 0;
    config->zero_drift = 0.0;
    config->max_zero_drift_threshold = 0.0;
    config->safe_mode = false;
    config->latitude = 0.0;
    config->longitude = 0.0;
    strcpy(config->city, "Unknown");
    strcpy(config->state, "Unknown");
    strcpy(config->last_updated, "");

    char line_buf[1024];
    while (fgets(line_buf, sizeof(line_buf), fp)) {
        if (strstr(line_buf, "\"device_id\"")) {
            char *p = strchr(line_buf, ':');
            if(p) sscanf(p + 1, "%d", &config->device_id);
        }
        extract_json_string(line_buf, "\"type\"", config->device_type, sizeof(config->device_type));
        if (strstr(line_buf, "\"latitude\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->latitude);
        }
        if (strstr(line_buf, "\"longitude\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->longitude);
        }
        extract_json_string(line_buf, "\"city\"", config->city, sizeof(config->city));
        extract_json_string(line_buf, "\"state\"", config->state, sizeof(config->state));
    }

    fclose(fp);
    return 0;
}

// --- Call /bin/tamper_log to log tamper event ---
int log_tamper_event(const char *tamper_type, const char *details) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        // Child process: exec /bin/tamper_log
        if (details) {
            execl(TAMPER_LOG_BIN, "tamper_log", "--type", tamper_type, "--details", details, (char *)NULL);
        } else {
            execl(TAMPER_LOG_BIN, "tamper_log", "--type", tamper_type, (char *)NULL);
        }
        perror("execl failed for /bin/tamper_log");
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// --- Initialize GPIO ---
int init_gpio(void) {
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
void cleanup(void) {
    if (line) {
        gpiod_line_release(line);
    }
    if (chip) {
        gpiod_chip_close(chip);
    }
    // Only close LCD if it was active
    if (lcd_is_active) {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_send_string("System Stopped");
        lcd_close();
    }
}

// --- Get Current Timestamp ---
void get_timestamp(char *buf, size_t sz) {
    time_t now = time(NULL);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

// --- Main Program ---
int main(void) {
    Config config = {0};

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("==========================================\n");
    printf("   Magnetic Tamper Monitor - Calibris\n");
    printf("==========================================\n");

    // --- Load Configuration ---
    printf("\n[Init] Loading configuration from %s\n", CONFIG_FILE);
    if (parse_config(CONFIG_FILE, &config) != 0) {
        fprintf(stderr, "Failed to load configuration!\n");
        return 1;
    }

    // --- Initialize GPIO ---
    printf("[Init] Initializing GPIO %s:%u...\n", chipname, line_offset);
    if (init_gpio() != 0) {
        fprintf(stderr, "Failed to initialize GPIO!\n");
        return 1;
    }

    // NOTE: LCD is NOT initialized here. It is only touched during tamper events.

    printf("[Monitor] System ready. Monitoring for magnetic tamper...\n");
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
            printf("|   WARNING: TAMPER DETECTED!                           |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time             : %-34s|\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            // 1. Log to database via /bin/tamper_log
            log_tamper_event("magnetic", "Magnet removed from sensor");

            // 2. STOP weight measurement service FIRST (Release resources)
            printf("[Action] Stopping measure_weight.service...\n");
            system("systemctl stop measure_weight.service");

            // 3. Initialize LCD (Only now that service is stopped)
            printf("[Action] Initializing LCD for warning display...\n");
            if (lcd_init(I2C_BUS, I2C_ADDR) == 0) {
                lcd_is_active = true;
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_send_string("!! SAFE MODE !!");
                lcd_set_cursor(1, 0);
                lcd_send_string("Remove Magnet");
            } else {
                fprintf(stderr, "[Error] Failed to initialize LCD during tamper event!\n");
            }
        }

        // --- TAMPER CLEARED (Falling Edge: HIGH -> LOW) ---
        else if (gpio_value == 0 && tampered_state) {
            tampered_state = false;

            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n");
            printf("+-------------------------------------------------------+\n");
            printf("|   OK: TAMPER CLEARED                                  |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time             : %-34s|\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            // 1. Close/Release LCD FIRST (Free resources for service)
            if (lcd_is_active) {
                printf("[Action] Closing LCD...\n");
                lcd_clear(); // Optional: Clear screen before closing
                lcd_close();
                lcd_is_active = false;
            }

            // 2. START weight measurement service
            printf("[Action] Starting measure_weight.service...\n");
            system("systemctl start measure_weight.service");
        }

        usleep(100000);  // 100ms polling interval
    }

    // --- Cleanup ---
    printf("\n[Shutdown] Cleaning up...\n");
    cleanup();
    printf("[Shutdown] Goodbye!\n");

    return 0;
}
