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
#define SAFE_MODE_BIN    "/home/pico/calibris/bin/activate_safe_mode_bin/activate_safe_mode"

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

// --- Helper:  Extract string value from JSON line ---
void extract_json_string(const char *line, const char *key, char *dest, size_t dest_size) {
    char *pos = strstr(line, key);
    if (pos) {
        char *start = strchr(pos, ': ');
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

// --- Parse config. json ---
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

// --- Call tamper_log binary with detailed error handling ---
int log_tamper_event(const char *tamper_type, const char *details) {
    // First, verify the binary exists and is executable
    if (access(TAMPER_LOG_BIN, F_OK) != 0) {
        fprintf(stderr, "[ERROR] tamper_log binary not found at:  %s\n", TAMPER_LOG_BIN);
        return -1;
    }
    
    if (access(TAMPER_LOG_BIN, X_OK) != 0) {
        fprintf(stderr, "[ERROR] tamper_log binary not executable: %s\n", TAMPER_LOG_BIN);
        fprintf(stderr, "[ERROR] Attempting to fix permissions...\n");
        // Try to fix permissions
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
        // Child process:  exec tamper_log
        fprintf(stderr, "[DEBUG] Executing:  %s --type %s --details %s\n", 
                TAMPER_LOG_BIN, tamper_type, details ?  details : "");
        
        if (details) {
            execl(TAMPER_LOG_BIN, TAMPER_LOG_BIN, "--type", tamper_type, 
                  "--details", details, (char *)NULL);
        } else {
            execl(TAMPER_LOG_BIN, TAMPER_LOG_BIN, "--type", tamper_type, (char *)NULL);
        }
        
        // If execl returns, it failed
        perror("[ERROR] execl failed");
        fprintf(stderr, "[ERROR] Failed to execute:  %s\n", TAMPER_LOG_BIN);
        _exit(127);
    }
    
    // Parent process:  wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[ERROR] waitpid failed");
        return -1;
    }
    
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            fprintf(stderr, "[SUCCESS] Tamper event logged successfully\n");
            return 0;
        } else {
            fprintf(stderr, "[ERROR] tamper_log exited with code: %d\n", exit_code);
            return -1;
        }
    }
    
    return -1;
}

// --- Call activate_safe_mode binary ---
/*int activate_safe_mode(void) {
    // First, verify the binary exists and is executable
    if (access(SAFE_MODE_BIN, F_OK) != 0) {
        fprintf(stderr, "[ERROR] activate_safe_mode binary not found at:  %s\n", SAFE_MODE_BIN);
        return -1;
    }
    
    if (access(SAFE_MODE_BIN, X_OK) != 0) {
        fprintf(stderr, "[ERROR] activate_safe_mode binary not executable: %s\n", SAFE_MODE_BIN);
        fprintf(stderr, "[ERROR] Attempting to fix permissions...\n");
        char chmod_cmd[512];
        snprintf(chmod_cmd, sizeof(chmod_cmd), "sudo chmod +x %s", SAFE_MODE_BIN);
        system(chmod_cmd);
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("[ERROR] fork failed");
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        fprintf(stderr, "[DEBUG] Executing: %s %s\n", SAFE_MODE_BIN, CONFIG_FILE);
        execl(SAFE_MODE_BIN, SAFE_MODE_BIN, CONFIG_FILE, (char *)NULL);
        
        perror("[ERROR] execl failed");
        fprintf(stderr, "[ERROR] Failed to execute: %s\n", SAFE_MODE_BIN);
        _exit(127);
    }
    
    // Parent process:  wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[ERROR] waitpid failed");
        return -1;
    }
    
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        fprintf(stderr, "[DEBUG] activate_safe_mode exited with code: %d\n", exit_code);
        return exit_code;
    }
    
    return -1;
}*/

// --- Initialize GPIO ---
int init_gpio(void) {
    chip = gpiod_chip_open_by_name(chipname);
    if (! chip) {
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
    printf("[Init] Initializing GPIO %s:%u.. .\n", chipname, line_offset);
    if (init_gpio() != 0) {
        fprintf(stderr, "Failed to initialize GPIO!\n");
        return 1;
    }

    // --- Verify tamper_log binary exists ---
    printf("[Init] Verifying tamper_log binary at:  %s\n", TAMPER_LOG_BIN);
    if (access(TAMPER_LOG_BIN, F_OK) != 0) {
        fprintf(stderr, "[WARNING] tamper_log binary not found!\n");
        fprintf(stderr, "[WARNING] Tamper events will not be logged to database\n");
    } else if (access(TAMPER_LOG_BIN, X_OK) != 0) {
        fprintf(stderr, "[WARNING] tamper_log binary exists but is not executable\n");
        fprintf(stderr, "[ACTION] Making tamper_log executable.. .\n");
        system("sudo chmod +x /home/pico/calibris/bin/tamper_log_bin/tamper_log");
    } else {
        printf("[OK] tamper_log binary is ready\n");
    }

    printf("[Monitor] System ready.  Monitoring for magnetic tamper.. .\n");
    printf("[Monitor] Press Ctrl+C to exit.\n\n");

    bool tampered_state = false;

    // --- Main Monitoring Loop ---
    while (running) {
        int gpio_value = gpiod_line_get_value(line);
        if (gpio_value < 0) {
            perror("Error reading GPIO");
            break;
        }

        // --- TAMPER DETECTED (Rising Edge:  LOW -> HIGH) ---
        if (gpio_value == 1 && !tampered_state) {
            tampered_state = true;

            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n");
            printf("+-------------------------------------------------------+\n");
            printf("|   WARNING: TAMPER DETECTED!                            |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time             : %-34s|\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            // 1. Log to database via tamper_log binary
            printf("[Action] Logging tamper event to database...\n");
            if (log_tamper_event("magnetic", "Magnet removed from sensor") == 0) {
                printf("[OK] Tamper event logged successfully\n");
            } else {
                printf("[WARNING] Failed to log tamper event - continuing anyway\n");
            }

            // 2. STOP weight measurement service FIRST (Release resources)
            printf("[Action] Stopping measure_weight. service...\n");
            int ret = system("systemctl stop measure_weight.service");
            if (ret == 0) {
                printf("[OK] measure_weight.service stopped\n");
            } else {
                printf("[WARNING] Failed to stop measure_weight.service\n");
            }

            // 3. Initialize LCD (Only now that service is stopped)
            printf("[Action] Initializing LCD for warning display...\n");
            if (lcd_init(I2C_BUS, I2C_ADDR) == 0) {
                lcd_is_active = true;
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_send_string("!!  SAFE MODE ! !");
                lcd_set_cursor(1, 0);
                lcd_send_string("Remove Magnet");
                printf("[OK] LCD initialized and displaying warning\n");
            } else {
                fprintf(stderr, "[ERROR] Failed to initialize LCD during tamper event!\n");
            }

            // 4. Activate safe mode
            /*printf("[Action] Activating safe mode...\n");
            if (activate_safe_mode() == 0) {
                printf("[OK] Safe mode activated\n");
            } else {
                printf("[WARNING] Safe mode activation may have failed\n");
            }*/
        }

        // --- TAMPER CLEARED (Falling Edge:  HIGH -> LOW) ---
        else if (gpio_value == 0 && tampered_state) {
            tampered_state = false;

            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n");
            printf("+-------------------------------------------------------+\n");
            printf("|   OK: TAMPER CLEARED                                  |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time             :  %-34s|\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            // 1. Close/Release LCD FIRST (Free resources for service)
            if (lcd_is_active) {
                printf("[Action] Closing LCD.. .\n");
                lcd_clear();
                lcd_close();
                lcd_is_active = false;
                printf("[OK] LCD closed\n");
            }

            // 2. START weight measurement service
            printf("[Action] Starting measure_weight.service.. .\n");
            int ret = system("systemctl start measure_weight.service");
            if (ret == 0) {
                printf("[OK] measure_weight.service started\n");
            } else {
                printf("[WARNING] Failed to start measure_weight.service\n");
            }
        }

        usleep(100000);  // 100ms polling interval
    }

    // --- Cleanup ---
    printf("\n[Shutdown] Cleaning up...\n");
    cleanup();
    printf("[Shutdown] Goodbye!\n");

    return 0;
}
