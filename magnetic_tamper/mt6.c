/**
 * Magnetic Tamper Monitor for Calibris
 *
 * - Reads configuration from config.json
 * - Monitors GPIO for magnetic tamper detection
 * - Logs tamper events to SQLite database
 * - Displays status on I2C LCD
 * - Controls measure_weight.service
 *
 * Compile with: gcc -o tamper_mon main.c -lgpiod -lsqlite3
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <stdbool.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include "lcd.h" // Ensure you have this header and corresponding .c file

// --- File Paths ---
#define CONFIG_FILE     "/home/pico/calibris/data/config.json"
#define DB_PATH         "/home/pico/calibris/data/mydata.db"

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
    double calibration_factor;
    long tare_offset;
    bool safe_mode;
    char site_name[64];
    double latitude;
    double longitude;
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
    config->calibration_factor = 0.0;
    config->tare_offset = 0;
    config->safe_mode = false;
    strcpy(config->site_name, "Unknown");
    config->latitude = 0.0;
    config->longitude = 0.0;
    strcpy(config->last_updated, "");

    char line_buf[1024]; // Increased buffer size
    while (fgets(line_buf, sizeof(line_buf), fp)) {
        
        // Parse device_id
        if (strstr(line_buf, "\"device_id\"")) {
            char *p = strchr(line_buf, ':');
            if(p) sscanf(p + 1, "%d", &config->device_id);
        }

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

        // Parse safe_mode
        if (strstr(line_buf, "\"safe_mode\"")) {
            config->safe_mode = (strstr(line_buf, "true") != NULL);
        }

        // Parse site_name
        extract_json_string(line_buf, "\"site_name\"", config->site_name, sizeof(config->site_name));

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

        // Parse last_updated
        extract_json_string(line_buf, "\"last_updated\"", config->last_updated, sizeof(config->last_updated));
    }

    fclose(fp);
    return 0;
}

// --- Update safe_mode in config.json ---
int update_safe_mode(const char *filepath, bool safe_mode) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        perror("Failed to open config for reading");
        return -1;
    }

    // Read entire file
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(fp);
        return -1;
    }
    
    if (fread(content, 1, fsize, fp) != (size_t)fsize) {
        perror("Failed to read file");
        free(content);
        fclose(fp);
        return -1;
    }
    content[fsize] = '\0';
    fclose(fp);

    // Find "safe_mode" key
    char *pos = strstr(content, "\"safe_mode\"");
    if (pos) {
        char *colon = strchr(pos, ':');
        if (colon) {
            char *value_start = colon + 1;
            while (isspace(*value_start)) value_start++;

            // Create new buffer: old size + leeway for "false" (5 chars)
            char *new_content = malloc(fsize + 20); 
            if (!new_content) {
                free(content);
                return -1;
            }

            // Copy everything up to value start
            size_t prefix_len = value_start - content;
            strncpy(new_content, content, prefix_len);
            new_content[prefix_len] = '\0';

            // Append new value
            strcat(new_content, safe_mode ? "true" : "false");

            // Find end of the old value to skip it
            char *value_end = value_start;
            while (*value_end && *value_end != ',' && *value_end != '\n' && *value_end != '}') {
                value_end++;
            }
            
            // Append the rest of the file
            strcat(new_content, value_end);

            // Write back to file
            fp = fopen(filepath, "w");
            if (fp) {
                fprintf(fp, "%s", new_content);
                fclose(fp);
            } else {
                perror("Failed to open config for writing");
            }

            free(new_content);
        }
    }

    free(content);
    return 0;
}

// --- Build location string from config ---
void build_location_string(const Config *config, char *buf, size_t sz) {
    snprintf(buf, sz, "%s, %.4f, %.4f",
             config->site_name, config->latitude, config->longitude);
}

// --- Log Tamper Event to SQLite Database ---
int log_tamper_to_db(const Config *config) {
    sqlite3 *db;
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    char location[128];
    build_location_string(config, location, sizeof(location));

    const char *insert_sql =
        "INSERT INTO tamper_log (product_id, tamper_type, resolution_status, location) "
        "VALUES (?, 'magnetic', 'detected', ?);";

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, config->device_id);
    sqlite3_bind_text(stmt, 2, location, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to insert tamper log: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }

    long long log_id = sqlite3_last_insert_rowid(db);

    printf("[DB] Tamper logged successfully!\n");
    printf("     log_id           : %lld\n", log_id);
    printf("     product_id       : %d\n", config->device_id);
    printf("     tamper_type      : magnetic\n");
    printf("     resolution_status: detected\n");
    printf("     location         : %s\n", location);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
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
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("System Stopped");
    // lcd_close(); // Ensure your lcd.h library has this, otherwise remove
}

// --- Print Configuration ---
void print_config(const Config *config) {
    printf("\n");
    printf("+------------------------------------------+\n");
    printf("|          CONFIGURATION LOADED            |\n");
    printf("+------------------------------------------+\n");
    printf("|  Device ID         : %-20d|\n", config->device_id);
    printf("|  Calibration       : %-20.4f|\n", config->calibration_factor);
    printf("|  Tare Offset       : %-20ld|\n", config->tare_offset);
    printf("|  Safe Mode         : %-20s|\n", config->safe_mode ? "true" : "false");
    printf("+------------------------------------------+\n");
    printf("|  LOCATION                                |\n");
    printf("|  Site Name         : %-20s|\n", config->site_name);
    printf("|  Latitude          : %-20.4f|\n", config->latitude);
    printf("|  Longitude         : %-20.4f|\n", config->longitude);
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
    printf("  Magnetic Tamper Monitor - Calibris\n");
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
    lcd_send_string(config.site_name);
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
            printf("|  WARNING: TAMPER DETECTED!                            |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time         : %-34s|\n", timestamp);
            printf("|  Device ID    : %-34d|\n", config.device_id);
            printf("|  Location     : %-34s|\n", config.site_name);
            printf("|  GPS          : %.4f, %.4f                        |\n", config.latitude, config.longitude);
            printf("+-------------------------------------------------------+\n");

            // 1. Log to database
            log_tamper_to_db(&config);

            // 2. Update config.json safe_mode = true
            update_safe_mode(CONFIG_FILE, true);
            printf("[Action] Config updated: safe_mode = true\n");

            // 3. Update LCD
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("!! SAFE MODE !!");
            lcd_set_cursor(1, 0);
            lcd_send_string("Magnet Removed");

            // 4.  Stop weight measurement service
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
            printf("|  OK: TAMPER CLEARED                                   |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time         : %-34s|\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            // Update config.json safe_mode = false
            update_safe_mode(CONFIG_FILE, false);
            printf("[Action] Config updated: safe_mode = false\n");

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
