/**
 * Magnetic Tamper Monitor for Calibris - Fully Encrypted Version
 *
 * This version encrypts ALL log fields (device_id, location, timestamp, etc.)
 * before inserting them into the SQLite database.
 * * Compile with: 
 * gcc -o tamper_mon mt7.c lcd.c -lgpiod -lsqlite3 -lcrypto
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
#include <openssl/evp.h>
#include <openssl/aes.h>
#include "lcd.h"

// --- Encryption Configuration ---
// In a production environment, these should not be hardcoded.
// Use a secure element, fuse, or obfuscation.
const unsigned char KEY[] = "Calibris_Pico_Max_Secure_Key_25"; // 32 bytes for AES-256
const unsigned char IV[]  = "InitializationVc";                 // 16 bytes for AES block size

// --- File Paths ---
#define CONFIG_FILE     "/home/pico/calibris/data/config.json"
#define DB_PATH         "/home/pico/calibris/data/mydata.db"

// --- GPIO Configuration ---
const char *chipname = "gpiochip1";
const unsigned int line_offset = 23; // GPIO1_B2_d

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

// --- Signal Handler ---
void signal_handler(int signum) {
    (void)signum;
    running = 0;
}

// --- Encryption Helper Function ---
// Encrypts 'plaintext' using AES-256-CBC and writes a hex string to 'output_hex'.
// Ensure 'output_hex' is large enough (approx 2x plaintext length + padding).
void encrypt_text(const char *plaintext, char *output_hex) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        fprintf(stderr, "Error creating cipher context\n");
        return;
    }

    int len;
    int ciphertext_len;
    unsigned char ciphertext[512]; // Buffer for raw encrypted bytes

    // Initialize Encryption
    if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, KEY, IV)) {
        fprintf(stderr, "Encryption init failed\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }

    // Encrypt Data
    if (1 != EVP_EncryptUpdate(ctx, ciphertext, &len, (const unsigned char *)plaintext, strlen(plaintext))) {
        fprintf(stderr, "Encryption update failed\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
    ciphertext_len = len;

    // Finalize Encryption
    if (1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len)) {
        fprintf(stderr, "Encryption final failed\n");
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
    ciphertext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    // Convert raw ciphertext to Hex String for SQL storage
    for(int i = 0; i < ciphertext_len; i++) {
        sprintf(output_hex + (i * 2), "%02x", ciphertext[i]);
    }
    output_hex[ciphertext_len * 2] = '\0';
}

// --- Helper: Extract string value from JSON line ---
void extract_json_string(const char *line, const char *key, char *dest, size_t dest_size) {
    char *pos = strstr(line, key);
    if (pos) {
        char *start = strchr(pos, ':');
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

    // Defaults
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
        if (strstr(line_buf, "\"calibration_factor\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->calibration_factor);
        }
        if (strstr(line_buf, "\"tare_offset\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%ld", &config->tare_offset);
        }
        if (strstr(line_buf, "\"zero_drift\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->zero_drift);
        }
        if (strstr(line_buf, "\"max_zero_drift_threshold\"")) {
             char *p = strchr(line_buf, ':');
             if(p) sscanf(p + 1, "%lf", &config->max_zero_drift_threshold);
        }
        if (strstr(line_buf, "\"safe_mode\"")) {
            config->safe_mode = (strstr(line_buf, "true") != NULL);
        }
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

    char *pos = strstr(content, "\"safe_mode\"");
    if (pos) {
        char *colon = strchr(pos, ':');
        if (colon) {
            char *value_start = colon + 1;
            while (isspace(*value_start)) value_start++;

            char *new_content = malloc(fsize + 50); 
            if (!new_content) {
                free(content);
                return -1;
            }

            size_t prefix_len = value_start - content;
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
            } else {
                perror("Failed to open config for writing");
            }
            free(new_content);
        }
    }
    free(content);
    return 0;
}

// --- Log Tamper Event to SQLite Database (Encrypted) ---
int log_tamper_to_db(const Config *config) {
    sqlite3 *db;
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    // 1. Prepare buffers for plaintext strings
    char s_dev_id[32], s_lat[32], s_lon[32], s_drift[32], s_time[64];
    
    // 2. Prepare buffers for encrypted hex strings
    // Size should be generous to handle AES padding and hex doubling
    char e_dev_id[128], e_type[256], e_tamper[128], e_status[128], e_lat[128], 
         e_lon[128], e_city[256], e_state[256], e_drift[128], e_time[256];

    // 3. Convert numbers to strings
    sprintf(s_dev_id, "%d", config->device_id);
    sprintf(s_lat, "%.6f", config->latitude);
    sprintf(s_lon, "%.6f", config->longitude);
    sprintf(s_drift, "%.2f", config->zero_drift);
    
    // Generate timestamp manually
    time_t now = time(NULL);
    strftime(s_time, sizeof(s_time), "%Y-%m-%d %H:%M:%S", localtime(&now));

    // 4. Encrypt all fields
    encrypt_text(s_dev_id, e_dev_id);
    encrypt_text(config->device_type, e_type);
    encrypt_text("magnetic", e_tamper);
    encrypt_text("detected", e_status);
    encrypt_text(s_lat, e_lat);
    encrypt_text(s_lon, e_lon);
    encrypt_text(config->city, e_city);
    encrypt_text(config->state, e_state);
    encrypt_text(s_drift, e_drift);
    encrypt_text(s_time, e_time);

    // 5. Prepare SQL Insert
    // NOTE: 'pushed_at' is omitted, so it will be NULL in the database.
    const char *insert_sql =
        "INSERT INTO tamper_logs (device_id, device_type, tamper_type, resolution_status, "
        "latitude, longitude, city, state, drift, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    // 6. Bind the Encrypted Hex Strings
    // Use SQLITE_TRANSIENT so SQLite makes its own copy of the string
    sqlite3_bind_text(stmt, 1, e_dev_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, e_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, e_tamper, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, e_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, e_lat, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, e_lon, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, e_city, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, e_state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, e_drift, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, e_time, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to insert tamper log: %s\n", sqlite3_errmsg(db));
    } else {
        long long log_id = sqlite3_last_insert_rowid(db);
        printf("[DB] Encrypted Tamper Log Saved. Row ID: %lld\n", log_id);
    }

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
    if (line) gpiod_line_release(line);
    if (chip) gpiod_chip_close(chip);
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("System Stopped");
}

// --- Print Config (Debugging) ---
void print_config(const Config *config) {
    printf("\n--- Configuration Loaded ---\n");
    printf("Device ID: %d\n", config->device_id);
    printf("Location: %s, %s\n", config->city, config->state);
    printf("Safe Mode: %s\n", config->safe_mode ? "ON" : "OFF");
}

// --- Main Program ---
int main(void) {
    Config config = {0};

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("==========================================\n");
    printf("  Magnetic Tamper Monitor - Encrypted\n");
    printf("==========================================\n");

    printf("\n[Init] Loading configuration from %s\n", CONFIG_FILE);
    if (parse_config(CONFIG_FILE, &config) != 0) {
        fprintf(stderr, "Failed to load configuration!\n");
        return 1;
    }
    print_config(&config);

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

    printf("[Init] Initializing GPIO %s:%u...\n", chipname, line_offset);
    if (init_gpio() != 0) {
        fprintf(stderr, "Failed to initialize GPIO!\n");
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_send_string("GPIO Error!");
        return 1;
    }

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("System Ready");
    char id_str[17];
    snprintf(id_str, sizeof(id_str), "ID:%d", config.device_id);
    lcd_set_cursor(1, 0);
    lcd_send_string(id_str);

    printf("[Monitor] System ready. Monitoring for magnetic tamper...\n");

    bool tampered_state = false;

    while (running) {
        int gpio_value = gpiod_line_get_value(line);
        if (gpio_value < 0) {
            perror("Error reading GPIO");
            break;
        }

        // --- TAMPER DETECTED (Rising Edge) ---
        if (gpio_value == 1 && !tampered_state) {
            tampered_state = true;
            printf("\n[ALERT] Magnetic Tamper Detected!\n");

            // 1. Log Encrypted Data
            log_tamper_to_db(&config);

            // 2. Enable Safe Mode
            update_safe_mode(CONFIG_FILE, true);
            printf("[Action] Safe mode enabled.\n");

            // 3. UI Feedback
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("!! SAFE MODE !!");
            lcd_set_cursor(1, 0);
            lcd_send_string("Magnet Removed");

            // 4. Stop Service
            printf("[Action] Stopping weight service...\n");
            system("systemctl stop measure_weight.service");
        }

        // --- TAMPER CLEARED (Falling Edge) ---
        else if (gpio_value == 0 && tampered_state) {
            tampered_state = false;
            printf("\n[INFO] Tamper Condition Cleared.\n");

            update_safe_mode(CONFIG_FILE, false);
            printf("[Action] Safe mode disabled.\n");

            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("System Ready");
            lcd_set_cursor(1, 0);
            lcd_send_string(id_str);

            printf("[Action] Starting weight service...\n");
            system("systemctl start measure_weight.service");
        }

        usleep(100000); // 100ms poll
    }

    printf("\n[Shutdown] Cleaning up...\n");
    cleanup();
    return 0;
}
