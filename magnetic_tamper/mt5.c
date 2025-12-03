/**
 * Magnetic Tamper Monitor for Calibris (No LCD version)
 *
 * - Reads all configuration from config.json
 * - Monitors GPIO for magnetic tamper detection
 * - Logs tamper events to SQLite database (tamper_log table)
 * - Controls measure_weight. service
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

// --- File Paths ---
#define CONFIG_FILE     "/home/pico/calibris/data/config.json"
#define DB_PATH         "/home/pico/calibris/data/mydata.db"

// --- GPIO Configuration ---
const char *chipname = "gpiochip1";
const unsigned int line_offset = 23;  // GPIO1_B2_d

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
    char last_updated[32];
} Config;

// --- Signal Handler for Clean Exit ---
void signal_handler(int signum) {
    (void)signum;
    running = 0;
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
    config->calibration_factor = 0.0;
    config->tare_offset = 0;
    config->safe_mode = false;
    strcpy(config->site_name, "Unknown");
    config->latitude = 0.0;
    config->longitude = 0.0;
    strcpy(config->last_updated, "");

    char line_buf[256];
    while (fgets(line_buf, sizeof(line_buf), fp)) {
        char *pos;

        pos = strstr(line_buf, "\"device_id\"");
        if (pos) {
            sscanf(pos, "\"device_id\": %d", &config->device_id);
            continue;
        }

        pos = strstr(line_buf, "\"calibration_factor\"");
        if (pos) {
            sscanf(pos, "\"calibration_factor\": %lf", &config->calibration_factor);
            continue;
        }

        pos = strstr(line_buf, "\"tare_offset\"");
        if (pos) {
            sscanf(pos, "\"tare_offset\": %ld", &config->tare_offset);
            continue;
        }

        pos = strstr(line_buf, "\"safe_mode\"");
        if (pos) {
            config->safe_mode = (strstr(pos, "true") != NULL);
            continue;
        }

        pos = strstr(line_buf, "\"site_name\"");
        if (pos) {
            char *start = strchr(pos, ':');
            if (start) {
                start = strchr(start, '"') + 1;
                char *end = strchr(start, '"');
                if (end) {
                    size_t len = end - start;
                    if (len >= sizeof(config->site_name)) {
                        len = sizeof(config->site_name) - 1;
                    }
                    strncpy(config->site_name, start, len);
                    config->site_name[len] = '\0';
                }
            }
            continue;
        }

        pos = strstr(line_buf, "\"latitude\"");
        if (pos) {
            sscanf(pos, "\"latitude\": %lf", &config->latitude);
            continue;
        }

        pos = strstr(line_buf, "\"longitude\"");
        if (pos) {
            sscanf(pos, "\"longitude\": %lf", &config->longitude);
            continue;
        }

        pos = strstr(line_buf, "\"last_updated\"");
        if (pos) {
            char *start = strchr(pos, ':');
            if (start) {
                start = strchr(start, '"') + 1;
                char *end = strchr(start, '"');
                if (end) {
                    size_t len = end - start;
                    if (len >= sizeof(config->last_updated)) {
                        len = sizeof(config->last_updated) - 1;
                    }
                    strncpy(config->last_updated, start, len);
                    config->last_updated[len] = '\0';
                }
            }
            continue;
        }
    }

    fclose(fp);
    return 0;
}

// --- Update safe_mode in config.json ---
int update_safe_mode(const char *filepath, bool safe_mode) {
    FILE *fp = fopen(filepath, "r");
    if (! fp) {
        perror("Failed to open config for reading");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(fp);
        return -1;
    }
    
    size_t bytes_read = fread(content, 1, fsize, fp);
    if (bytes_read != (size_t)fsize) {
        fprintf(stderr, "Warning: read %zu bytes, expected %ld\n", bytes_read, fsize);
    }
    content[fsize] = '\0';
    fclose(fp);

    char *pos = strstr(content, "\"safe_mode\"");
    if (pos) {
        char *colon = strchr(pos, ':');
        if (colon) {
            char *value_start = colon + 1;
            while (*value_start == ' ') value_start++;

            char *new_content = malloc(fsize + 10);
            if (! new_content) {
                free(content);
                return -1;
            }

            int prefix_len = value_start - content;
            strncpy(new_content, content, prefix_len);
            new_content[prefix_len] = '\0';

            strcat(new_content, safe_mode ? "true" : "false");

            char *value_end = value_start;
            while (*value_end && *value_end != ',' && *value_end != '\n' && *value_end != '}') {
                value_end++;
            }
            strcat(new_content, value_end);

            fp = fopen(filepath, "w");
            if (fp) {
                fprintf(fp, "%s", new_content);
                fclose(fp);
            }

            free(new_content);
        }
    }

    free(content);
    return 0;
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

    // Build location string
    char location[128];
    snprintf(location, sizeof(location), "%s, %. 4f, %.4f",
             config->site_name, config->latitude, config->longitude);

    // Build SQL directly (simpler)
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO tamper_log (product_id, tamper_type, resolution_status, location) "
        "VALUES (%d, 'magnetic', 'detected', '%s');",
        config->device_id, location);

    char *err_msg = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
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

    sqlite3_close(db);
    return 0;
}

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
}

// --- Print Configuration ---
void print_config(const Config *config) {
    printf("\n");
    printf("+------------------------------------------+\n");
    printf("|         CONFIGURATION LOADED             |\n");
    printf("+------------------------------------------+\n");
    printf("|  Device ID        : %-20d|\n", config->device_id);
    printf("|  Calibration      : %-20. 4f|\n", config->calibration_factor);
    printf("|  Tare Offset      : %-20ld|\n", config->tare_offset);
    printf("|  Safe Mode        : %-20s|\n", config->safe_mode ? "true" : "false");
    printf("+------------------------------------------+\n");
    printf("|  LOCATION                                |\n");
    printf("|  Site Name        : %-20s|\n", config->site_name);
    printf("|  Latitude         : %-20.4f|\n", config->latitude);
    printf("|  Longitude        : %-20.4f|\n", config->longitude);
    printf("|  Last Updated     : %-20s|\n", config->last_updated);
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

    print_config(&config);

    // --- Initialize GPIO ---
    printf("[Init] Initializing GPIO %s:%u.. .\n", chipname, line_offset);
    if (init_gpio() != 0) {
        fprintf(stderr, "Failed to initialize GPIO!\n");
        return 1;
    }

    printf("[Monitor] System ready. Monitoring for magnetic tamper...\n");
    printf("[Monitor] Press Ctrl+C to exit.\n\n");

    bool tampered_state = false;
    char device_str[17];
    snprintf(device_str, sizeof(device_str), "ID:%d", config.device_id);

    // --- Main Monitoring Loop ---
    while (running) {
        int gpio_value = gpiod_line_get_value(line);
        if (gpio_value < 0) {
            perror("Error reading GPIO");
            break;
        }

        // --- TAMPER DETECTED ---
        if (gpio_value == 1 && !tampered_state) {
            tampered_state = true;

            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  WARNING: TAMPER DETECTED!                            |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time       : %-40s|\n", timestamp);
            printf("|  Device ID  : %-40d|\n", config.device_id);
            printf("|  Location   : %-40s|\n", config.site_name);
            printf("|  GPS        : %.4f, %.4f\n", config.latitude, config.longitude);
            printf("+-------------------------------------------------------+\n");

            log_tamper_to_db(&config);

            update_safe_mode(CONFIG_FILE, true);
            printf("[Action] Config updated: safe_mode = true\n");

            printf("[Action] Stopping measure_weight.service...\n");
            int ret = system("systemctl stop measure_weight. service");
            if (ret != 0) {
                fprintf(stderr, "Warning: systemctl returned %d\n", ret);
            }
        }

        // --- TAMPER CLEARED ---
        else if (gpio_value == 0 && tampered_state) {
            tampered_state = false;

            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            printf("\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  OK: TAMPER CLEARED                                   |\n");
            printf("+-------------------------------------------------------+\n");
            printf("|  Time       : %-40s|\n", timestamp);
            printf("+-------------------------------------------------------+\n");

            update_safe_mode(CONFIG_FILE, false);
            printf("[Action] Config updated: safe_mode = false\n");

            printf("[Action] Starting measure_weight.service...\n");
            int ret = system("systemctl start measure_weight.service");
            if (ret != 0) {
                fprintf(stderr, "Warning: systemctl returned %d\n", ret);
            }
        }

        usleep(100000);
    }

    printf("\n[Shutdown] Cleaning up...\n");
    cleanup();
    printf("[Shutdown] Goodbye!\n");

    return 0;
}
