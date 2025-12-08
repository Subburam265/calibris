/**
 * Tamper Log Library for Calibris
 * * A reusable library for logging tamper events with blockchain support. 
 * Can be used by any tamper detection code (magnetic, firmware, weight drift, etc.)
 *
 * Features:
 * - Blockchain hash chain (prev_hash, curr_hash)
 * - Auto-reads device config from config.json
 * - Updates safe_mode in config.json
 * - Controls measure_weight.service
 */

#ifndef TAMPER_LOG_H
#define TAMPER_LOG_H

#include <stdbool.h>

// --- Default Paths (can be overridden) ---
#define DEFAULT_CONFIG_FILE  "/home/pico/calibris/data/config.json"
#define DEFAULT_DB_PATH      "/home/pico/calibris/data/mydata.db"
#define GENESIS_HASH         "0000000000000000000000000000000000000000000000000000000000000000"

// --- Configuration Structure ---
typedef struct {
    int device_id;
    char device_type[64];
    double calibration_factor;
    long tare_offset;
    double zero_drift;
    double max_zero_drift_threshold;
    double settling_time;
    int renewal_cycle;
    bool safe_mode;
    double latitude;
    double longitude;
    char city[64];
    char state[64];
    char last_updated[64];
} TamperConfig;

// --- Result codes ---
typedef enum {
    TAMPER_LOG_SUCCESS = 0,
    TAMPER_LOG_ERR_CONFIG = -1,
    TAMPER_LOG_ERR_DATABASE = -2,
    TAMPER_LOG_ERR_INSERT = -3,
    TAMPER_LOG_ERR_HASH = -4,
    TAMPER_LOG_ERR_SAFE_MODE = -5,
    TAMPER_LOG_ERR_SERVICE = -6
} TamperLogResult;

// --- Main API Functions ---

/**
 * Log a tamper event to the database with blockchain support. 
 * Also updates safe_mode to true and stops measure_weight.service. 
 *
 * @param tamper_type  Type of tamper (e.g., "magnetic", "firmware", "weight_drift")
 * @param details      Optional details/description (can be NULL)
 * @return             TAMPER_LOG_SUCCESS on success, error code otherwise
 */
TamperLogResult log_tamper(const char *tamper_type, const char *details);

/**
 * Log a tamper event using custom paths for config and database.
 *
 * @param tamper_type   Type of tamper
 * @param details       Optional details (can be NULL)
 * @param config_path   Path to config.json
 * @param db_path       Path to SQLite database
 * @return              TAMPER_LOG_SUCCESS on success, error code otherwise
 */
TamperLogResult log_tamper_ex(const char *tamper_type, const char *details,
                              const char *config_path, const char *db_path);

/**
 * Parse configuration from config.json
 *
 * @param filepath  Path to config.json
 * @param config    Pointer to TamperConfig structure to fill
 * @return          0 on success, -1 on failure
 */
int parse_config(const char *filepath, TamperConfig *config);

/**
 * Update safe_mode value in config.json
 *
 * @param filepath   Path to config.json
 * @param safe_mode  New value for safe_mode
 * @return           0 on success, -1 on failure
 */
int update_safe_mode(const char *filepath, bool safe_mode);

/**
 * Stop the measure_weight.service
 *
 * @return  0 on success, -1 on failure
 */
int stop_weight_service(void);

/**
 * Start the measure_weight.service
 *
 * @return  0 on success, -1 on failure
 */
int start_weight_service(void);

/**
 * Get human-readable error message for result code
 *
 * @param result  Result code from log_tamper functions
 * @return        Error message string
 */
const char* tamper_log_strerror(TamperLogResult result);

#endif // TAMPER_LOG_H
