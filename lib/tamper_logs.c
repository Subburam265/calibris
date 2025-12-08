/**
 * Tamper Log Library for Calibris
 *
 * Implementation of blockchain-enabled tamper logging.
 *
 * Compile: gcc -c tamper_log.c -o tamper_log.o
 * Create static lib: ar rcs libtamper_log.a tamper_log.o
 * Create shared lib: gcc -shared -fPIC tamper_log.c -o libtamper_log.so -lsqlite3 -lssl -lcrypto
 */

#include "tamper_logs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdbool.h>
#include <sqlite3.h>
#include <openssl/evp.h>

// --- Helper: Compute SHA-256 hash (OpenSSL 3.0 compatible) ---
static void compute_sha256(const char *input, char *output_hex) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        memset(output_hex, '0', 64);
        output_hex[64] = '\0';
        return;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, input, strlen(input)) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        memset(output_hex, '0', 64);
        output_hex[64] = '\0';
        return;
    }

    EVP_MD_CTX_free(ctx);

    for (unsigned int i = 0; i < hash_len; i++) {
        snprintf(output_hex + (i * 2), 3, "%02x", hash[i]);
    }
    output_hex[hash_len * 2] = '\0';
}

// --- Helper: Get the last curr_hash from the database ---
static int get_last_hash(sqlite3 *db, char *prev_hash, size_t hash_size) {
    const char *sql = "SELECT curr_hash FROM tamper_logs ORDER BY log_id DESC LIMIT 1;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        strncpy(prev_hash, GENESIS_HASH, hash_size);
        prev_hash[hash_size - 1] = '\0';
        return -1;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        if (hash && strlen(hash) > 0) {
            strncpy(prev_hash, hash, hash_size);
            prev_hash[hash_size - 1] = '\0';
        } else {
            strncpy(prev_hash, GENESIS_HASH, hash_size);
            prev_hash[hash_size - 1] = '\0';
        }
    } else {
        strncpy(prev_hash, GENESIS_HASH, hash_size);
        prev_hash[hash_size - 1] = '\0';
    }

    sqlite3_finalize(stmt);
    return 0;
}

// --- Helper: Build data string for hashing ---
static void build_hash_data(char *buffer, size_t buf_size, const char *prev_hash,
                            int device_id, const char *device_type, const char *tamper_type,
                            const char *resolution_status, double settling_time, int renewal_cycle,
                            double latitude, double longitude,
                            const char *city, const char *state, double drift,
                            const char *details, const char *timestamp) {
    snprintf(buffer, buf_size,
             "%s|%d|%s|%s|%s|%.4f|%d|%.6f|%.6f|%s|%s|%.4f|%s|%s",
             prev_hash, device_id, device_type, tamper_type, resolution_status,
             settling_time, renewal_cycle,
             latitude, longitude, city, state, drift,
             details ? details : "", timestamp);
}

// --- Helper: Get current timestamp ---
static void get_timestamp(char *buf, size_t sz) {
    time_t now = time(NULL);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

// --- Helper: Extract string value from JSON line ---
static void extract_json_string(const char *line, const char *key, char *dest, size_t dest_size) {
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
int parse_config(const char *filepath, TamperConfig *config) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        perror("[tamper_log] Failed to open config file");
        return -1;
    }

    // Initialize defaults
    config->device_id = 0;
    strcpy(config->device_type, "Unknown");
    config->calibration_factor = 0.0;
    config->tare_offset = 0;
    config->zero_drift = 0.0;
    config->max_zero_drift_threshold = 0.0;
    config->settling_time = 0.0;
    config->renewal_cycle = 0;
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
            if (p) sscanf(p + 1, "%d", &config->device_id);
        }
        extract_json_string(line_buf, "\"type\"", config->device_type, sizeof(config->device_type));
        if (strstr(line_buf, "\"calibration_factor\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%lf", &config->calibration_factor);
        }
        if (strstr(line_buf, "\"tare_offset\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%ld", &config->tare_offset);
        }
        if (strstr(line_buf, "\"zero_drift\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%lf", &config->zero_drift);
        }
        if (strstr(line_buf, "\"max_zero_drift_threshold\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%lf", &config->max_zero_drift_threshold);
        }
        if (strstr(line_buf, "\"settling_time\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%lf", &config->settling_time);
        }
        if (strstr(line_buf, "\"renewal_cycle\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%d", &config->renewal_cycle);
        }
        if (strstr(line_buf, "\"safe_mode\"")) {
            config->safe_mode = (strstr(line_buf, "true") != NULL);
        }
        if (strstr(line_buf, "\"latitude\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%lf", &config->latitude);
        }
        if (strstr(line_buf, "\"longitude\"")) {
            char *p = strchr(line_buf, ':');
            if (p) sscanf(p + 1, "%lf", &config->longitude);
        }
        extract_json_string(line_buf, "\"city\"", config->city, sizeof(config->city));
        extract_json_string(line_buf, "\"state\"", config->state, sizeof(config->state));
        extract_json_string(line_buf, "\"last_updated\"", config->last_updated, sizeof(config->last_updated));
    }

    fclose(fp);
    return 0;
}

// --- Get error message ---
const char* tamper_log_strerror(TamperLogResult result) {
    switch (result) {
        case TAMPER_LOG_SUCCESS:       return "Success";
        case TAMPER_LOG_ERR_CONFIG:    return "Failed to read configuration";
        case TAMPER_LOG_ERR_DATABASE:  return "Failed to open database";
        case TAMPER_LOG_ERR_INSERT:    return "Failed to insert record";
        case TAMPER_LOG_ERR_HASH:      return "Failed to compute hash";
        case TAMPER_LOG_ERR_SAFE_MODE: return "Failed to update safe_mode";
        case TAMPER_LOG_ERR_SERVICE:   return "Failed to control service";
        default:                       return "Unknown error";
    }
}

// --- Main logging function (extended version) ---
TamperLogResult log_tamper_ex(const char *tamper_type, const char *details,
                               const char *config_path, const char *db_path) {
    TamperConfig config;
    sqlite3 *db;
    int rc;

    // Validate input
    if (!tamper_type || strlen(tamper_type) == 0) {
        fprintf(stderr, "[tamper_log] Error: tamper_type is required\n");
        return TAMPER_LOG_ERR_INSERT;
    }

    // Parse configuration
    if (parse_config(config_path, &config) != 0) {
        return TAMPER_LOG_ERR_CONFIG;
    }

    // Open database
    rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[tamper_log] Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return TAMPER_LOG_ERR_DATABASE;
    }

    // Get timestamp
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    // Get previous hash for blockchain
    char prev_hash[65];
    get_last_hash(db, prev_hash, sizeof(prev_hash));

    // Build data string for hashing
    char hash_data[2048];
    build_hash_data(hash_data, sizeof(hash_data), prev_hash,
                    config.device_id, config.device_type, tamper_type, "detected",
                    config.settling_time, config.renewal_cycle,
                    config.latitude, config.longitude, config.city, config.state,
                    config.zero_drift, details, timestamp);

    // Compute current hash
    char curr_hash[65];
    compute_sha256(hash_data, curr_hash);

    // Prepare SQL statement
    const char *insert_sql =
        "INSERT INTO tamper_logs (device_id, device_type, tamper_type, resolution_status, "
        "settling_time, renewal_cycle, latitude, longitude, city, state, drift, details, "
        "prev_hash, curr_hash) "
        "VALUES (?, ?, ?, 'detected', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[tamper_log] Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return TAMPER_LOG_ERR_INSERT;
    }

    // Bind parameters
    sqlite3_bind_int(stmt, 1, config.device_id);
    sqlite3_bind_text(stmt, 2, config.device_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, tamper_type, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, config.settling_time);
    sqlite3_bind_int(stmt, 5, config.renewal_cycle);
    sqlite3_bind_double(stmt, 6, config.latitude);
    sqlite3_bind_double(stmt, 7, config.longitude);
    sqlite3_bind_text(stmt, 8, config.city, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, config.state, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 10, config.zero_drift);
    if (details) {
        sqlite3_bind_text(stmt, 11, details, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt, 11);
    }
    sqlite3_bind_text(stmt, 12, prev_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 13, curr_hash, -1, SQLITE_STATIC);

    // Execute
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[tamper_log] Failed to insert: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return TAMPER_LOG_ERR_INSERT;
    }

    long long log_id = sqlite3_last_insert_rowid(db);

    // Print success info
    printf("[tamper_log] Tamper logged successfully to %s!\n", db_path);
    printf("      log_id            : %lld\n", log_id);
    printf("      device_id         : %d\n", config.device_id);
    printf("      device_type       : %s\n", config.device_type);
    printf("      tamper_type       : %s\n", tamper_type);
    printf("      drift             : %.2f\n", config.zero_drift);
    printf("      prev_hash         : %.16s...\n", prev_hash);
    printf("      curr_hash         : %.16s...\n", curr_hash);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return TAMPER_LOG_SUCCESS;
}

// --- Main logging function (simple version targeting mydata.db) ---
TamperLogResult log_tamper(const char *tamper_type, const char *details) {
    // Explicitly using "mydata.db" as requested
    return log_tamper_ex(tamper_type, details, DEFAULT_CONFIG_FILE, "mydata.db");
}
