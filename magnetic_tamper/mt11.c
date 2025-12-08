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
#define TAMPER_LOG_BIN   "/bin/tamper_log"

// --- GPIO Configuration ---
const char *chipname = "gpiochip1";
const unsigned int line_offset = 23;

// --- LCD Configuration ---
#define I2C_BUS  "/dev/i2c-3"
#define I2C_ADDR 0x27

// --- Global Variables ---
volatile sig_atomic_t running = 1;
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

        // Parse device_id
        if (strstr(line_buf, "\"device_id\"")) {
            char *p = strchr(line_buf, ':');
            if(p) sscanf(p + 1, "%d", &config->device_id);
        }

        // Parse type (device_type)
        extract_json_string(line_buf, "\"type\"", config->device_type, sizeof(config->device_type));

        // Parse calibration_factor
        if (strstr(line_buf, "\"calibration_factor\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->calibration_factor);
        }

        // Parse tare_offset
        if (strstr(line_buf, "\"tare_offset\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%ld", &config->tare_offset);
        }

        // Parse zero_drift
        if (strstr(line_buf, "\"zero_drift\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->zero_drift);
        }

        // Parse max_zero_drift_threshold
        if (strstr(line_buf, "\"max_zero_drift_threshold\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->max_zero_drift_threshold);
        }

        // Parse safe_mode
        if (strstr(line_buf, "\"safe_mode\"")) {
            config->safe_mode = (strstr(line_buf, "true") != NULL);
        }

        // Parse latitude
        if (strstr(line_buf, "\"latitude\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->latitude);
        }

        // Parse longitude
        if (strstr(line_buf, "\"longitude\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->longitude);
        }

        // Parse city
        extract_json_string(line_buf, "\"city\"", config->city, sizeof(config->city));

        // Parse state
        extract_json_string(line_buf, "\"state\"", config->state, sizeof(config->state));

        // Parse last_updated
        extract_json_string(line_buf, "\"last_updated\"", config->last_updated, sizeof(config->last_updated));
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
        // If execl returns, it's an error
        perror("execl failed for /bin/tamper_log");
        _exit(127);
    }

    // Parent: wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            printf("[Tamper Log] Event logged successfully via /bin/tamper_log\n");
        } else {
            fprintf(stderr, "[Tamper Log] /bin/tamper_log exited with code %d\n", exit_code);
        }
        return exit_code;
    }
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

    // Note: 'magnetic_tamper' is the consumer label
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
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("System Stopped");
    lcd_close();
}

// --- Print Configuration ---
void print_config(const Config *config) {
    printf("\n");
    printf("+------------------------------------------+\n");
    printf("|            CONFIGURATION LOADED          |\n");
    printf("+------------------------------------------+\n");
    printf("|  Device ID         : %-20d|\n", config->device_id);
    printf("|  Device Type       : %-20s|\n", config->device_type);
    printf("|  Calibration       : %-20.4f|\n", config->calibration_factor);
    printf("|  Tare Offset       : %-20ld|\n", config->tare_offset);
    printf("|  Zero Drift        : %-20.2f|\n", config->zero_drift);
    printf("|  Max Drift Thresh  : %-20.2f|\n", config->max_zero_drift_threshold);
    printf("|  Safe Mode         : %-20s|\n", config->safe_mode ? "true" : "false");
    printf("+------------------------------------------+\n");
    printf("|  LOCATION                                |\n");
    printf("|  Latitude          : %-20.4f|\n", config->latitude);
    printf("|  Longitude         : %-20.4f|\n", config->longitude);
    printf("|  City              : %-20s|\n", config->city);
    printf("|  State             : %-20s|\n", config->state);
    printf("|  Last Updated      : %-20s|\n", config->last_updated);
    printf("+------------------------------------------+\n");
    printf("\n");
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

    // Print loaded configuration
    print_config(&config);

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
        usleep(2000000); // Wait for user to see error
        lcd_close();     // Cleanup LCD before exit
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
            printf("|  Device ID        : %-34d|\n", config.device_id);
            printf("|  Device Type      : %-34s|\n", config.device_type);
            printf("|  City             : %-34s|\n", config.city);
            printf("|  State            : %-34s|\n", config.state);
            printf("|  GPS              : %.4f, %.4f                        |\n", config.latitude, config.longitude);
            printf("+-------------------------------------------------------+\n");

            // 1. Log to database via /bin/tamper_log
            log_tamper_event("magnetic", "Magnet removed from sensor");

            // REMOVED: update_safe_mode call

            // 2. Update LCD
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("!! SAFE MODE !!");
            lcd_set_cursor(1, 0);
            lcd_send_string("Magnet Removed");

            // 3. Stop weight measurement service
            printf("[Action] Stopping measure_weight.service...\n");
            system("systemctl stop measure_weight.service");
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

            // REMOVED: update_safe_mode call

            // Update LCD
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("System Ready");
            lcd_set_cursor(1, 0);
            lcd_send_string(device_str);

            // Start weight measurement service
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
