/**
 * Magnetic Tamper Monitor for Calibris - Fully Encrypted Version
 *
 * Fixed by Gemini:
 * - Removed illegal spaces in variable names and headers.
 * - Fixed systemctl service name typos (critical for runtime).
 * - Fixed malformed float literals.
 * - [NEW] Updated SHA256 and MD5 calls to use modern OpenSSL EVP interface 
 * to fix deprecation warnings in OpenSSL 3.0+.
 *
 * Compile with:
 * gcc -o tamper_mon magnetic_tamper_monitor.c lcd.c -lgpiod -lsqlite3 -lcrypto
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
#include <openssl/rand.h>
#include "lcd.h"

// --- Master Secret for Key Derivation ---
// WARNING: In production, do not hardcode this. Use a TPM or Secure Element.
#define MASTER_SECRET "Calibris_Pico_Max_Master_Secret_2025_Secure"

// --- File Paths ---
#define CONFIG_FILE     "/home/pico/calibris/data/config.json"
#define DB_PATH         "/home/pico/calibris/data/mydata.db"

// --- GPIO Configuration ---
const char *chipname = "gpiochip1";
const unsigned int line_offset = 23; // GPIO1_B2_d

// --- LCD Configuration ---
#define I2C_BUS  "/dev/i2c-3"
#define I2C_ADDR 0x27

// --- Encryption Key Structure ---
typedef struct {
    unsigned char key[32];  // AES-256 key (32 bytes)
    unsigned char iv[16];   // AES IV (16 bytes)
    bool initialized;
} DerivedKey;

// --- Global Variables ---
volatile sig_atomic_t running = 1;
struct gpiod_chip *chip = NULL;
struct gpiod_line *line = NULL;
DerivedKey g_derived_key = {0};

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

// --- Dynamic Key Derivation Function ---
int derive_encryption_key(int device_id, DerivedKey *dk) {
    if (!dk) return -1;
    
    char device_id_str[32];
    char combined_key_input[256];
    char combined_iv_input[256];
    char reversed_id[32];
    
    // Convert device_id to string
    snprintf(device_id_str, sizeof(device_id_str), "%d", device_id);
    
    // Create reversed device_id for IV derivation
    size_t id_len = strlen(device_id_str);
    for (size_t i = 0; i < id_len; i++) {
        reversed_id[i] = device_id_str[id_len - 1 - i];
    }
    reversed_id[id_len] = '\0';
    
    // --- Derive AES-256 Key using SHA-256 (EVP Interface) ---
    snprintf(combined_key_input, sizeof(combined_key_input), 
             "%s:%s:KEY", MASTER_SECRET, device_id_str);
    
    unsigned char sha256_hash[EVP_MAX_MD_SIZE];
    unsigned int sha_len;

    EVP_MD_CTX *mdctx_key = EVP_MD_CTX_new();
    if (mdctx_key == NULL) return -1;

    if (1 != EVP_DigestInit_ex(mdctx_key, EVP_sha256(), NULL) ||
        1 != EVP_DigestUpdate(mdctx_key, combined_key_input, strlen(combined_key_input)) ||
        1 != EVP_DigestFinal_ex(mdctx_key, sha256_hash, &sha_len)) {
        EVP_MD_CTX_free(mdctx_key);
        return -1;
    }
    EVP_MD_CTX_free(mdctx_key);
    
    memcpy(dk->key, sha256_hash, 32);
    
    // --- Derive IV using MD5 (EVP Interface) ---
    snprintf(combined_iv_input, sizeof(combined_iv_input), 
             "%s:%s:IV", MASTER_SECRET, reversed_id);
    
    unsigned char md5_hash[EVP_MAX_MD_SIZE];
    unsigned int md5_len;

    EVP_MD_CTX *mdctx_iv = EVP_MD_CTX_new();
    if (mdctx_iv == NULL) return -1;

    if (1 != EVP_DigestInit_ex(mdctx_iv, EVP_md5(), NULL) ||
        1 != EVP_DigestUpdate(mdctx_iv, combined_iv_input, strlen(combined_iv_input)) ||
        1 != EVP_DigestFinal_ex(mdctx_iv, md5_hash, &md5_len)) {
        EVP_MD_CTX_free(mdctx_iv);
        return -1;
    }
    EVP_MD_CTX_free(mdctx_iv);
    
    // MD5 produces 16 bytes, which fits AES IV exactly
    memcpy(dk->iv, md5_hash, 16);
    
    dk->initialized = true;
    
    // Debug output
    printf("[KeyDerivation] Derived unique key for Device ID: %d\n", device_id);
    printf("[KeyDerivation] Key (first 8 bytes): ");
    for (int i = 0; i < 8; i++) printf("%02x", dk->key[i]);
    printf("...\n");
    printf("[KeyDerivation] IV (first 8 bytes): ");
    for (int i = 0; i < 8; i++) printf("%02x", dk->iv[i]);
    printf("...\n");
    
    return 0;
}

// --- Secure Memory Wipe ---
void secure_wipe(void *ptr, size_t len) {
    volatile unsigned char *p = ptr;
    while (len--) *p++ = 0;
}

// --- Encryption Helper Function ---
int encrypt_text(const char *plaintext, char *output_hex, size_t output_size) {
    if (!g_derived_key.initialized) {
        fprintf(stderr, "[Encrypt] Error: Encryption key not initialized\n");
        return -1;
    }
    
    if (!plaintext || !output_hex || output_size == 0) {
        return -1;
    }
    
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        fprintf(stderr, "[Encrypt] Error creating cipher context\n");
        return -1;
    }

    int len;
    int ciphertext_len;
    size_t plaintext_len = strlen(plaintext);
    
    size_t max_ciphertext = plaintext_len + EVP_MAX_BLOCK_LENGTH;
    unsigned char *ciphertext = malloc(max_ciphertext);
    if (!ciphertext) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, 
                                 g_derived_key.key, g_derived_key.iv)) {
        fprintf(stderr, "[Encrypt] Encryption init failed\n");
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        return -1;
    }

    if (1 != EVP_EncryptUpdate(ctx, ciphertext, &len, 
                                (const unsigned char *)plaintext, plaintext_len)) {
        fprintf(stderr, "[Encrypt] Encryption update failed\n");
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        return -1;
    }
    ciphertext_len = len;

    if (1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len)) {
        fprintf(stderr, "[Encrypt] Encryption final failed\n");
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        return -1;
    }
    ciphertext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    if (output_size < (size_t)(ciphertext_len * 2 + 1)) {
        fprintf(stderr, "[Encrypt] Output buffer too small\n");
        free(ciphertext);
        return -1;
    }

    for (int i = 0; i < ciphertext_len; i++) {
        sprintf(output_hex + (i * 2), "%02x", ciphertext[i]);
    }
    output_hex[ciphertext_len * 2] = '\0';

    secure_wipe(ciphertext, max_ciphertext);
    free(ciphertext);

    return ciphertext_len;
}

// --- Decryption Helper Function ---
int decrypt_text(const char *hex_ciphertext, char *output_plain, size_t output_size) {
    if (!g_derived_key.initialized) {
        fprintf(stderr, "[Decrypt] Error: Encryption key not initialized\n");
        return -1;
    }
    
    if (!hex_ciphertext || !output_plain || output_size == 0) {
        return -1;
    }
    
    size_t hex_len = strlen(hex_ciphertext);
    if (hex_len % 2 != 0) {
        fprintf(stderr, "[Decrypt] Invalid hex string length\n");
        return -1;
    }
    
    size_t ciphertext_len = hex_len / 2;
    unsigned char *ciphertext = malloc(ciphertext_len);
    if (!ciphertext) {
        return -1;
    }
    
    for (size_t i = 0; i < ciphertext_len; i++) {
        sscanf(hex_ciphertext + (i * 2), "%2hhx", &ciphertext[i]);
    }
    
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        free(ciphertext);
        return -1;
    }
    
    int len;
    int plaintext_len;
    // Buffer for plaintext + padding
    unsigned char *plaintext = malloc(ciphertext_len + EVP_MAX_BLOCK_LENGTH + 1);
    if (!plaintext) {
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        return -1;
    }
    
    if (1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
                                 g_derived_key.key, g_derived_key.iv)) {
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        free(plaintext);
        return -1;
    }
    
    if (1 != EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len)) {
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        free(plaintext);
        return -1;
    }
    plaintext_len = len;
    
    if (1 != EVP_DecryptFinal_ex(ctx, plaintext + len, &len)) {
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        free(plaintext);
        return -1;
    }
    plaintext_len += len;
    
    EVP_CIPHER_CTX_free(ctx);
    free(ciphertext);
    
    if ((size_t)plaintext_len >= output_size) {
        plaintext_len = output_size - 1;
    }
    memcpy(output_plain, plaintext, plaintext_len);
    output_plain[plaintext_len] = '\0';
    
    secure_wipe(plaintext, ciphertext_len + EVP_MAX_BLOCK_LENGTH + 1);
    free(plaintext);
    
    return plaintext_len;
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

// --- Generate Unique Log ID ---
void generate_unique_log_id(char *log_id, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    // Generate random suffix
    unsigned char random_bytes[3];
    RAND_bytes(random_bytes, 3);
    
    snprintf(log_id, size, "%04d%02d%02d%02d%02d%02d_%02X%02X%02X",
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             random_bytes[0],
             random_bytes[1],
             random_bytes[2]);
}

// --- Log Tamper Event to SQLite Database (Fully Encrypted) ---
int log_tamper_to_db(const Config *config) {
    sqlite3 *db;
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    // Create table if not exists
    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS tamper_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "log_id TEXT NOT NULL, "
        "device_id TEXT NOT NULL, "
        "device_type TEXT, "
        "tamper_type TEXT, "
        "resolution_status TEXT, "
        "latitude TEXT, "
        "longitude TEXT, "
        "city TEXT, "
        "state TEXT, "
        "drift TEXT, "
        "created_at TEXT, "
        "pushed_at TEXT"
        ");";
    
    rc = sqlite3_exec(db, create_sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to create table: %s\n", sqlite3_errmsg(db));
    }

    // Buffer size for encrypted hex strings
    #define ENC_BUF_SIZE 1024

    // 1. Prepare buffers for plaintext strings
    char s_log_id[64], s_dev_id[32], s_lat[32], s_lon[32], s_drift[32], s_time[64];

    // 2. Prepare buffers for encrypted hex strings
    char e_log_id[ENC_BUF_SIZE], e_dev_id[ENC_BUF_SIZE], e_type[ENC_BUF_SIZE];
    char e_tamper[ENC_BUF_SIZE], e_status[ENC_BUF_SIZE], e_lat[ENC_BUF_SIZE];
    char e_lon[ENC_BUF_SIZE], e_city[ENC_BUF_SIZE], e_state[ENC_BUF_SIZE];
    char e_drift[ENC_BUF_SIZE], e_time[ENC_BUF_SIZE];

    // 3. Generate unique log_id and convert numbers to strings
    generate_unique_log_id(s_log_id, sizeof(s_log_id));
    snprintf(s_dev_id, sizeof(s_dev_id), "%d", config->device_id);
    snprintf(s_lat, sizeof(s_lat), "%.6f", config->latitude);
    snprintf(s_lon, sizeof(s_lon), "%.6f", config->longitude);
    snprintf(s_drift, sizeof(s_drift), "%.2f", config->zero_drift);

    // Generate timestamp
    time_t now = time(NULL);
    strftime(s_time, sizeof(s_time), "%Y-%m-%d %H:%M:%S", localtime(&now));

    printf("[DB] Encrypting all fields with device-specific key...\n");

    // 4. Encrypt ALL fields including log_id
    if (encrypt_text(s_log_id, e_log_id, ENC_BUF_SIZE) < 0 ||
        encrypt_text(s_dev_id, e_dev_id, ENC_BUF_SIZE) < 0 ||
        encrypt_text(config->device_type, e_type, ENC_BUF_SIZE) < 0 ||
        encrypt_text("magnetic", e_tamper, ENC_BUF_SIZE) < 0 ||
        encrypt_text("detected", e_status, ENC_BUF_SIZE) < 0 ||
        encrypt_text(s_lat, e_lat, ENC_BUF_SIZE) < 0 ||
        encrypt_text(s_lon, e_lon, ENC_BUF_SIZE) < 0 ||
        encrypt_text(config->city, e_city, ENC_BUF_SIZE) < 0 ||
        encrypt_text(config->state, e_state, ENC_BUF_SIZE) < 0 ||
        encrypt_text(s_drift, e_drift, ENC_BUF_SIZE) < 0 ||
        encrypt_text(s_time, e_time, ENC_BUF_SIZE) < 0) {
        
        fprintf(stderr, "[DB] Encryption failed for one or more fields\n");
        sqlite3_close(db);
        return -1;
    }

    // 5. Prepare SQL Insert with encrypted log_id
    const char *insert_sql =
        "INSERT INTO tamper_logs (log_id, device_id, device_type, tamper_type, "
        "resolution_status, latitude, longitude, city, state, drift, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    // 6. Bind the Encrypted Hex Strings
    sqlite3_bind_text(stmt, 1, e_log_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, e_dev_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, e_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, e_tamper, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, e_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, e_lat, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, e_lon, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, e_city, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, e_state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, e_drift, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, e_time, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to insert tamper log: %s\n", sqlite3_errmsg(db));
    } else {
        long long row_id = sqlite3_last_insert_rowid(db);
        printf("[DB] Encrypted Tamper Log Saved.\n");
        printf("[DB] SQLite Row ID: %lld\n", row_id);
        printf("[DB] Encrypted Log ID: %.32s...\n", e_log_id);
        printf("[DB] Original Log ID: %s\n", s_log_id);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Secure wipe sensitive plaintext data
    secure_wipe(s_log_id, sizeof(s_log_id));
    secure_wipe(s_dev_id, sizeof(s_dev_id));
    secure_wipe(s_lat, sizeof(s_lat));
    secure_wipe(s_lon, sizeof(s_lon));
    secure_wipe(s_drift, sizeof(s_drift));
    secure_wipe(s_time, sizeof(s_time));

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
    // Secure wipe encryption keys from memory
    secure_wipe(&g_derived_key, sizeof(g_derived_key));
    
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
    printf("Device Type: %s\n", config->device_type);
    printf("Location: %s, %s\n", config->city, config->state);
    printf("Coordinates: %.6f, %.6f\n", config->latitude, config->longitude);
    printf("Safe Mode: %s\n", config->safe_mode ? "ON" : "OFF");
    printf("Zero Drift: %.2f\n", config->zero_drift);
}

// --- Verify Encryption (Debug Function) ---
void verify_encryption_system(void) {
    printf("\n--- Encryption System Verification ---\n");
    
    const char *test_plain = "TestData123";
    char test_encrypted[256];
    char test_decrypted[256];
    
    if (encrypt_text(test_plain, test_encrypted, sizeof(test_encrypted)) > 0) {
        printf("Original:  %s\n", test_plain);
        printf("Encrypted: %.32s...\n", test_encrypted);
        
        if (decrypt_text(test_encrypted, test_decrypted, sizeof(test_decrypted)) > 0) {
            printf("Decrypted: %s\n", test_decrypted);
            
            if (strcmp(test_plain, test_decrypted) == 0) {
                printf("✓ Encryption/Decryption VERIFIED\n");
            } else {
                printf("✗ Encryption/Decryption MISMATCH\n");
            }
        }
    }
    printf("-----------------------------------\n\n");
}

// --- Main Program ---
int main(void) {
    Config config = {0};

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("==========================================\n");
    printf("  Magnetic Tamper Monitor - Encrypted\n");
    printf("  Dynamic Key Derivation Enabled\n");
    printf("==========================================\n");

    // Step 1: Load configuration
    printf("\n[Init] Loading configuration from %s\n", CONFIG_FILE);
    if (parse_config(CONFIG_FILE, &config) != 0) {
        fprintf(stderr, "Failed to load configuration!\n");
        return 1;
    }
    print_config(&config);

    // Step 2: Derive encryption keys based on device_id
    printf("\n[Init] Deriving encryption keys for Device ID: %d\n", config.device_id);
    if (derive_encryption_key(config.device_id, &g_derived_key) != 0) {
        fprintf(stderr, "Failed to derive encryption keys!\n");
        return 1;
    }

    // Step 3: Verify encryption system
    verify_encryption_system();

    // Step 4: Initialize LCD
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

    // Step 5: Initialize GPIO
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
    printf("[Monitor] Encryption: AES-256-CBC with device-specific key\n\n");

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
            printf("\n[ALERT] ====================================\n");
            printf("[ALERT] Magnetic Tamper Detected!\n");
            printf("[ALERT] ====================================\n");

            // 1. Log Encrypted Data
            log_tamper_to_db(&config);

            // 2. Enable Safe Mode
            update_safe_mode(CONFIG_FILE, true);
            config.safe_mode = true;
            printf("[Action] Safe mode enabled.\n");

            // 3. UI Feedback
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("!!  SAFE MODE !!");
            lcd_set_cursor(1, 0);
            lcd_send_string("Magnet Removed");

            // 4. Stop Service
            printf("[Action] Stopping weight service...\n");
            // Fixed typo: removed space in service name
            system("systemctl stop measure_weight.service");
        }

        // --- TAMPER CLEARED (Falling Edge) ---
        else if (gpio_value == 0 && tampered_state) {
            tampered_state = false;
            printf("\n[INFO] ====================================\n");
            printf("[INFO] Tamper Condition Cleared.\n");
            printf("[INFO] ====================================\n");

            update_safe_mode(CONFIG_FILE, false);
            config.safe_mode = false;
            printf("[Action] Safe mode disabled.\n");

            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("System Ready");
            lcd_set_cursor(1, 0);
            lcd_send_string(id_str);

            printf("[Action] Starting weight service...\n");
            // Fixed typo: removed space in service name
            system("systemctl start measure_weight.service");
        }

        usleep(100000); // 100ms poll
    }

    printf("\n[Shutdown] Cleaning up...\n");
    cleanup();
    return 0;
}
