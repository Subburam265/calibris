/**
 * Magnetic Tamper Monitor for Calibris
 * Modified to mirror Input (GPIO1_C7_d) to Output (GPIO1_C6_d) AND GPIO2_A0_d
 *
 * Compile with: gcc -o mt_monitor mt13.c ../lcd/lcd.c -I../lcd -lgpiod
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
#define SAFE_MODE_BIN    "/home/pico/calibris/bin/activate_safe_mode_bin/activate_safe_mode"

// --- GPIO Configuration (Bank 1) ---
const char *chipname = "gpiochip1";
// Input: GPIO1_C7_d
const unsigned int line_offset_input = 23;
// Output: GPIO1_C6_d
const unsigned int line_offset_output = 22;

// --- GPIO Configuration (Bank 2) ---
const char *chipname2 = "gpiochip2";
// Output: GPIO2_A0_d (Pin 24) -> Offset 0
const unsigned int line_offset_status = 0;

// --- LCD Configuration ---
#define I2C_BUS  "/dev/i2c-3"
#define I2C_ADDR 0x27

// --- Global Variables ---
volatile sig_atomic_t running = 1;
bool lcd_is_active = false;

// Bank 1 Pointers
struct gpiod_chip *chip = NULL;
struct gpiod_line *line_in = NULL;
struct gpiod_line *line_out = NULL;

// Bank 2 Pointers
struct gpiod_chip *chip2 = NULL;
struct gpiod_line *line_status = NULL; // GPIO2_A0

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
        char *start = strchr(pos, ': ');
        if (start) {
            start = strchr(start, '"');
            if (start) {
                start++;
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
            char *p = strchr(line_buf, ': ');
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

// --- Call tamper_log binary ---
int log_tamper_event(const char *tamper_type, const char *details) {
    if (access(TAMPER_LOG_BIN, F_OK) != 0) {
        fprintf(stderr, "[ERROR] tamper_log binary not found at: %s\n", TAMPER_LOG_BIN);
        return -1;
    }

    if (access(TAMPER_LOG_BIN, X_OK) != 0) {
        char chmod_cmd[512];
        snprintf(chmod_cmd, sizeof(chmod_cmd), "sudo chmod +x %s", TAMPER_LOG_BIN);
        system(chmod_cmd);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[ERROR] fork failed");
        return -1;
    }

    if (pid == 0) {
        if (details) {
            execl(TAMPER_LOG_BIN, TAMPER_LOG_BIN, "--type", tamper_type,
                  "--details", details, (char *)NULL);
        } else {
            execl(TAMPER_LOG_BIN, TAMPER_LOG_BIN, "--type", tamper_type, (char *)NULL);
        }
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;

    if (WIFEXITED(status)) {
        return (WEXITSTATUS(status) == 0) ? 0 : -1;
    }
    return -1;
}

// --- Initialize GPIO ---
int init_gpio(void) {
    // === BANK 1 (Existing) ===
    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip 1");
        return -1;
    }

    // 1. Initialize Input Line (C7)
    line_in = gpiod_chip_get_line(chip, line_offset_input);
    if (!line_in) {
        perror("Error getting GPIO input line");
        gpiod_chip_close(chip);
        return -1;
    }

    if (gpiod_line_request_input(line_in, "magnetic_tamper") < 0) {
        perror("Error requesting GPIO line as input");
        gpiod_chip_close(chip);
        return -1;
    }

    // 2. Initialize Output Line (C6)
    line_out = gpiod_chip_get_line(chip, line_offset_output);
    if (!line_out) {
        perror("Error getting GPIO output line");
        return -1;
    }

    if (gpiod_line_request_output(line_out, "tamper_mirror", 0) < 0) {
        perror("Error requesting GPIO line as output");
        return -1;
    }

    // === BANK 2 (New for GPIO2_A0) ===
    chip2 = gpiod_chip_open_by_name(chipname2);
    if (!chip2) {
        perror("Error opening GPIO chip 2");
        // Non-fatal if chip2 fails, but good to report
        return -1;
    }

    line_status = gpiod_chip_get_line(chip2, line_offset_status);
    if (!line_status) {
        perror("Error getting GPIO status line (GPIO2_A0)");
        gpiod_chip_close(chip2);
        return -1;
    }

    // Request as output, default value 0 (LOW - Safe)
    if (gpiod_line_request_output(line_status, "mt_status", 0) < 0) {
        perror("Error requesting Status GPIO");
        return -1;
    }

    return 0;
}

// --- Cleanup Resources ---
void cleanup(void) {
    // Cleanup Bank 1
    if (line_in) {
        gpiod_line_release(line_in);
    }
    if (line_out) {
        gpiod_line_set_value(line_out, 0);
        gpiod_line_release(line_out);
    }
    if (chip) {
        gpiod_chip_close(chip);
    }

    // Cleanup Bank 2 (Status Pin)
    if (line_status) {
        gpiod_line_set_value(line_status, 0); // Ensure LOW on exit
        gpiod_line_release(line_status);
    }
    if (chip2) {
        gpiod_chip_close(chip2);
    }

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

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("==========================================\n");
    printf("    Magnetic Tamper Monitor - Calibris\n");
    printf("==========================================\n");

    printf("\n[Init] Loading configuration from %s\n", CONFIG_FILE);
    if (parse_config(CONFIG_FILE, &config) != 0) {
        fprintf(stderr, "Failed to load configuration!\n");
        return 1;
    }

    printf("[Init] Initializing GPIOs...\n");
    printf("       Input:  %u (GPIO1_C7_d)\n", line_offset_input);
    printf("       Mirror: %u (GPIO1_C6_d)\n", line_offset_output);
    printf("       Status: %u (GPIO2_A0_d)\n", line_offset_status);

    if (init_gpio() != 0) {
        fprintf(stderr, "Failed to initialize GPIO!\n");
        return 1;
    }

    // Check binaries
    if (access(TAMPER_LOG_BIN, F_OK) != 0) {
        fprintf(stderr, "[WARNING] tamper_log binary not found!\n");
    } else if (access(TAMPER_LOG_BIN, X_OK) != 0) {
          system("sudo chmod +x /home/pico/calibris/bin/tamper_log_bin/tamper_log");
    }

    printf("[Monitor] System ready.\n");
    printf("[Monitor] Press Ctrl+C to exit.\n\n");

    bool tampered_state = false;

    // --- Main Monitoring Loop ---
    while (running) {
        int gpio_value = gpiod_line_get_value(line_in);
        if (gpio_value < 0) {
            perror("Error reading GPIO");
            break;
        }

        // --- UPDATE OUTPUT PINS (Mirroring Input) ---
        // 1. Mirror to GPIO1_C6 (Existing)
        if (line_out) {
            gpiod_line_set_value(line_out, gpio_value);
        }
        
        // 2. Mirror to GPIO2_A0 (New Status Pin)
        // High = Tamper Detected (Safe Mode Active)
        // Low  = Tamper Cleared (Safe Mode Cleared)
        if (line_status) {
            gpiod_line_set_value(line_status, gpio_value);
        }
        // --------------------------------------------

        // --- TAMPER DETECTED (Rising Edge: LOW -> HIGH) ---
        if (gpio_value == 1 && !tampered_state) {
            tampered_state = true;
            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n+-------------------------------------------------------+\n");
            printf("|    WARNING: TAMPER DETECTED!                          |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time             : %-34s|\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            printf("[Action] Logging tamper event to database...\n");
            log_tamper_event("magnetic", "Magnet removed from sensor");

            printf("[Action] Stopping measure_weight.service...\n");
            system("systemctl stop measure_weight.service");

            printf("[Action] Initializing LCD for warning display...\n");
            if (lcd_init(I2C_BUS, I2C_ADDR) == 0) {
                lcd_is_active = true;
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_send_string("!!  SAFE MODE ! !");
                lcd_set_cursor(1, 0);
                lcd_send_string("Remove Magnet");
            }
        }

        // --- TAMPER CLEARED (Falling Edge: HIGH -> LOW) ---
        else if (gpio_value == 0 && tampered_state) {
            tampered_state = false;
            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n+-------------------------------------------------------+\n");
            printf("|    OK: TAMPER CLEARED                                 |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time             :  %-34s|\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            if (lcd_is_active) {
                printf("[Action] Closing LCD...\n");
                lcd_clear();
                lcd_close();
                lcd_is_active = false;
            }

            printf("[Action] Starting measure_weight.service...\n");
            system("systemctl start measure_weight.service");
        }

        usleep(100000); // 100ms polling interval
    }

    cleanup();
    printf("[Shutdown] Goodbye!\n");
    return 0;
}
