/**
 * Magnetic Tamper Monitor for Calibris
 *
 * Simplified version - only handles GPIO-based magnetic tamper detection. 
 * - Calls /bin/tamper_log for logging tamper events
 * - Controls measure_weight.service locally
 * - Displays temporary safe mode on LCD (does NOT update config.json)
 *
 * Compile: gcc -o magnetic_monitor magnetic_monitor.c lcd.c -lgpiod
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "lcd.h"

// --- GPIO Configuration ---
static const char *chipname = "gpiochip1";
static const unsigned int line_offset = 23;

// --- LCD Configuration ---
#define I2C_BUS  "/dev/i2c-3"
#define I2C_ADDR 0x27

// --- Paths ---
#define TAMPER_LOG_BIN   "/bin/tamper_log"
#define CONFIG_FILE      "/home/pico/calibris/data/config.json"

// --- Global Variables ---
static volatile sig_atomic_t running = 1;
static struct gpiod_chip *chip = NULL;
static struct gpiod_line *line = NULL;

// --- Simple Config Structure (for display only) ---
typedef struct {
    int device_id;
    char city[64];
} SimpleConfig;

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

    // NOTE: This assumes an external pull-up resistor is present on the PCB.
    // If the pin floats, you may need to configure internal bias here depending on libgpiod version.
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
    // Attempt to leave LCD in a known state
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("System Stopped");
    // lcd_close(); // Ensure your lcd.h/c actually implements this, otherwise comment out.
}

// --- Get Current Timestamp ---
static void get_timestamp(char *buf, size_t sz) {
    time_t now = time(NULL);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

// --- Simple JSON parser (read-only, for display purposes) ---
static int parse_simple_config(const char *filepath, SimpleConfig *config) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        perror("[Config] Failed to open config file");
        return -1;
    }

    config->device_id = 0;
    strcpy(config->city, "Unknown");

    char line_buf[256];
    while (fgets(line_buf, sizeof(line_buf), fp)) {
        if (strstr(line_buf, "\"device_id\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%d", &config->device_id);
        }
        if (strstr(line_buf, "\"city\"")) {
            char *start = strchr(line_buf, ':');
            if (start) {
                start = strchr(start, '"');
                if (start) {
                    start++;
                    char *end = strchr(start, '"');
                    if (end) {
                        size_t len = end - start;
                        if (len >= sizeof(config->city)) len = sizeof(config->city) - 1;
                        strncpy(config->city, start, len);
                        config->city[len] = '\0';
                    }
                }
            }
        }
    }

    fclose(fp);
    return 0;
}

// --- Call /bin/tamper_log to log tamper event ---
static int call_tamper_log(const char *tamper_type, const char *details) {
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
        perror("execl");
        _exit(127);
    }

    // Parent: wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

// --- Stop measure_weight.service ---
static int stop_weight_service(void) {
    printf("[Service] Stopping measure_weight.service...\n");
    // FIXED: Removed space in service name
    int ret = system("systemctl stop measure_weight.service");
    if (ret == 0) {
        printf("[Service] measure_weight.service stopped.\n");
    } else {
        fprintf(stderr, "[Service] Failed to stop measure_weight.service\n");
    }
    return (ret == 0) ? 0 : -1;
}

// --- Start measure_weight.service ---
static int start_weight_service(void) {
    printf("[Service] Starting measure_weight.service...\n");
    // FIXED: Removed space in service name
    int ret = system("systemctl start measure_weight.service");
    if (ret == 0) {
        printf("[Service] measure_weight.service started.\n");
    } else {
        fprintf(stderr, "[Service] Failed to start measure_weight.service\n");
    }
    return (ret == 0) ? 0 : -1;
}

// --- Main Program ---
int main(void) {
    SimpleConfig config = {0};

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("==========================================\n");
    printf("  Magnetic Tamper Monitor - Calibris\n");
    printf("==========================================\n");

    // --- Load Configuration (for display purposes only) ---
    printf("\n[Init] Loading configuration...\n");
    if (parse_simple_config(CONFIG_FILE, &config) != 0) {
        fprintf(stderr, "Failed to load configuration!\n");
        return 1;
    }
    // FIXED: Removed space in config.city
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
    // FIXED: Removed space in config.city
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

    char device_str[17];
    snprintf(device_str, sizeof(device_str), "ID:%d", config.device_id);

    printf("[Monitor] System ready.  Monitoring for magnetic tamper...\n");
    printf("[Monitor] Press Ctrl+C to exit.\n\n");

    // FIXED: Initial state sync
    // We read the GPIO immediately to set the initial state correctly.
    // If we start the program while the magnet is ALREADY removed, we need to know.
    int initial_val = gpiod_line_get_value(line);
    bool tampered_state = (initial_val == 1); 

    if (tampered_state) {
        // If we boot up in a tampered state, ensure service is stopped and LCD is updated
        printf("[Init] DETECTED TAMPER ON STARTUP!\n");
        stop_weight_service();
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_send_string("!!  SAFE MODE !!");
        lcd_set_cursor(1, 0);
        lcd_send_string("Magnet Removed");
    } else {
        // If we boot up safe, ensure service is running and LCD is ready
        start_weight_service();
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_send_string("System Ready");
        lcd_set_cursor(1, 0);
        lcd_send_string(device_str);
    }

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

            // Log tamper event using /bin/tamper_log
            int log_result = call_tamper_log("magnetic", "Magnet removed from sensor");
            if (log_result != 0) {
                fprintf(stderr, "[ERROR] Failed to log tamper (exit code: %d)\n", log_result);
            } else {
                printf("[OK] Tamper event logged via /bin/tamper_log\n");
            }

            // Stop the weight service
            stop_weight_service();

            // Update LCD to show temporary safe mode
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

            // Start the weight service
            start_weight_service();

            // Update LCD back to ready state
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
