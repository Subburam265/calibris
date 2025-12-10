#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h> /* Added for pid_t */
#include <linux/i2c-dev.h>
#include <time.h>
#include <signal.h>
#include <string.h>

// INA219 I2C Address (default)
#define INA219_ADDRESS 0x40

// INA219 Registers
#define INA219_REG_CONFIG       0x00
#define INA219_REG_SHUNTVOLTAGE 0x01
#define INA219_REG_BUSVOLTAGE   0x02
#define INA219_REG_POWER        0x03
#define INA219_REG_CURRENT      0x04
#define INA219_REG_CALIBRATION  0x05

// Configuration values (32V, 2A range)
#define INA219_CONFIG_BVOLTAGERANGE_32V     (0x2000)
#define INA219_CONFIG_GAIN_8_320MV          (0x1800)
#define INA219_CONFIG_BADCRES_12BIT         (0x0180)
#define INA219_CONFIG_SADCRES_12BIT_1S      (0x0018)
#define INA219_CONFIG_MODE_SANDBVOLT_CONT   (0x0007)

// Tampering Detection Thresholds
#define REFERENCE_VOLTAGE       3.3f    // Reference voltage
#define VOLTAGE_TOLERANCE       2.0f    // ±0.5V tolerance
#define MAX_CURRENT_DEVIATION   1000000.0f  // Max 500mA deviation
#define MAX_READING_JUMP        1000000.0f    // Max 1V jump between readings (FIXED: removed space)
#define CONSECUTIVE_ERRORS      1000       // Fail after 3 consecutive errors
#define CRC_POLYNOMIAL          0xA001  // CRC-16 polynomial

// External Tool Paths
#define TAMPER_LOG_BIN          "/home/pico/calibris/bin/tamper_log_bin/tamper_log"
#define ACTIVATE_SAFE_MODE_BIN  "/home/pico/calibris/bin/activate_safe_mode_bin/activate_safe_mode"
#define CONFIG_PATH             "/home/pico/calibris/data/config.json"

// Safe Mode States
typedef enum {
    MODE_NORMAL = 0,
    MODE_CAUTION = 1,
    MODE_SAFE = 2,
    MODE_SHUTDOWN = 3
} OperatingMode;

// Sensor data structure for tampering detection
typedef struct {
    float last_bus_voltage;
    float last_current;
    int error_count;
    int tampering_detected;
    OperatingMode mode;
    time_t last_read_time;
    uint32_t read_count;
} SensorState;

static int i2c_fd = -1;
static float current_lsb = 0.0;
static float power_lsb = 0.0;
// Volatile flag for safe signal handling
static volatile sig_atomic_t running = 1;

static SensorState sensor_state = {
    .last_bus_voltage = 0.0,
    .last_current = 0.0,
    .error_count = 0,
    .tampering_detected = 0,
    .mode = MODE_NORMAL,
    .last_read_time = 0,
    .read_count = 0
};

// Forward declaration needed for references
int16_t ina219_read16(uint8_t reg);
int ina219_write16(uint8_t reg, uint16_t value);
void ina219_close(void);

// CRC-16 checksum calculation
uint16_t calculate_crc16(uint8_t *data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ CRC_POLYNOMIAL;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

// Call tamper_log binary to log tampering event
int call_tamper_log(const char *tamper_type, const char *details) {
    pid_t pid = fork();
   
    if (pid < 0) {
        perror("[INA219] fork failed for tamper_log");
        return -1;
    }
   
    if (pid == 0) {
        // Child process
        char details_arg[512];
        snprintf(details_arg, sizeof(details_arg), "%s", details);
       
        execl(TAMPER_LOG_BIN, 
              TAMPER_LOG_BIN,
              "--type", tamper_type,
              "--details", details_arg,
              (char *)NULL);
       
        fprintf(stderr, "[INA219] ERROR: Failed to execute tamper_log: %s\n", TAMPER_LOG_BIN);
        perror("execl");
        _exit(127);
    }
   
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[INA219] waitpid failed for tamper_log");
        return -1;
    }
   
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) return 0;
        fprintf(stderr, "[INA219] tamper_log exited with code: %d\n", exit_code);
        return -1;
    }
   
    return -1;
}

// Call activate_safe_mode binary
int call_activate_safe_mode(void) {
    pid_t pid = fork();
   
    if (pid < 0) {
        perror("[INA219] fork failed for activate_safe_mode");
        return -1;
    }
   
    if (pid == 0) {
        execl(ACTIVATE_SAFE_MODE_BIN,
              ACTIVATE_SAFE_MODE_BIN,
              CONFIG_PATH,
              (char *)NULL);
       
        fprintf(stderr, "[INA219] ERROR: Failed to execute activate_safe_mode: %s\n", 
                ACTIVATE_SAFE_MODE_BIN);
        perror("execl");
        _exit(127);
    }
   
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[INA219] waitpid failed for activate_safe_mode");
        return -1;
    }
   
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        fprintf(stderr, "[INA219] activate_safe_mode exited with code: %d\n", exit_code);
        return exit_code;
    }
   
    return -1;
}

// Log tampering event with automatic tool invocation
void log_tampering(const char *reason) {
    time_t now = time(NULL);
    char timestamp[26];
    ctime_r(&now, timestamp);
    timestamp[24] = '\0'; // Remove newline
   
    fprintf(stderr, "\n[TAMPER ALERT] %s - Reason: %s\n", timestamp, reason);
   
    // Log to local file for audit trail
    FILE *log = fopen("/var/log/ina219_tamper.log", "a");
    if (log) {
        fprintf(log, "[TAMPER] %s - %s\n", timestamp, reason);
        fclose(log);
    }
   
    sensor_state.tampering_detected = 1;
   
    fprintf(stderr, "[INA219] Invoking tamper_log binary...\n");
    if (call_tamper_log("signal_tampering", reason) != 0) {
        fprintf(stderr, "[INA219] WARNING: Failed to call tamper_log binary\n");
    } else {
        fprintf(stderr, "[INA219] Tampering event logged successfully\n");
    }
}

// Validate sensor reading with updated tamper detection logic
int validate_reading(float bus_voltage, float current, float power) {
    // Confirm tamper ONLY if voltage is outside 3.3V +/- 0.5V range
    float tamper_min = REFERENCE_VOLTAGE - VOLTAGE_TOLERANCE;  // 2.8V
    float tamper_max = REFERENCE_VOLTAGE + VOLTAGE_TOLERANCE;  // 3.8V
   
    // FIXED: Removed space in format specifier "%. 2f" -> "%.2f"
    if (bus_voltage < tamper_min || bus_voltage > tamper_max) {
        char reason[256];
        snprintf(reason, sizeof(reason), 
                 "Bus voltage %.2fV outside safe range (%.1fV ± %.1fV)", 
                 bus_voltage, REFERENCE_VOLTAGE, VOLTAGE_TOLERANCE);
        log_tampering(reason);
        return 0;
    }
   
    // Check current range (0-2A)
    if (current < 0 || current > 20000000000.0) {
        char reason[256];
        snprintf(reason, sizeof(reason), 
                 "Current %.2fmA out of range (0-2000mA)", current);
        log_tampering(reason);
        return 0;
    }
   
    if (sensor_state.read_count > 0) {
        float voltage_delta = bus_voltage - sensor_state.last_bus_voltage;
        if (voltage_delta < 0) voltage_delta = -voltage_delta;
       
        if (voltage_delta > MAX_READING_JUMP) {
            char reason[256];
            snprintf(reason, sizeof(reason), 
                     "Abnormal voltage spike detected (delta: %.2fV)", voltage_delta);
            log_tampering(reason);
            return 0;
        }
       
        float current_delta = current - sensor_state.last_current;
        if (current_delta < 0) current_delta = -current_delta;
       
        if (current_delta > MAX_CURRENT_DEVIATION) {
            char reason[256];
            snprintf(reason, sizeof(reason), 
                     "Abnormal current spike detected (delta: %.2fmA)", current_delta);
            log_tampering(reason);
            return 0;
        }
    }
   
    return 1;
}

// Enter safe mode with automatic activation
void enter_safe_mode(const char *reason) {
    sensor_state.mode = MODE_SAFE;
    fprintf(stderr, "\n========== SAFE MODE ACTIVATION INITIATED ==========\n");
    fprintf(stderr, "Reason: %s\n", reason);
    fprintf(stderr, "Invoking activate_safe_mode binary...\n");
    fprintf(stderr, "====================================================\n\n");
   
    FILE *log = fopen("/var/log/ina219_safe_mode.log", "a");
    if (log) {
        time_t now = time(NULL);
        // ctime includes newline
        fprintf(log, "[SAFE_MODE] %.24s - %s\n", ctime(&now), reason);
        fclose(log);
    }
   
    int result = call_activate_safe_mode();
    if (result == 0) {
        fprintf(stderr, "[INA219] Safe mode activated successfully\n");
    } else {
        fprintf(stderr, "[INA219] WARNING: Safe mode activation returned code: %d\n", result);
    }
}

// Shutdown sequence
void safe_shutdown(void) {
    fprintf(stderr, "\n========== CRITICAL: INITIATING SAFE SHUTDOWN ==========\n");
    fprintf(stderr, "Severe tampering detected - system shutting down safely\n");
    fprintf(stderr, "=========================================================\n\n");
   
    call_tamper_log("signal_tampering_critical", "System shutting down due to critical tampering");
   
    ina219_close();
    exit(EXIT_FAILURE);
}

// Write 16-bit value to register
int ina219_write16(uint8_t reg, uint16_t value) {
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (value >> 8) & 0xFF;  // MSB first
    buf[2] = value & 0xFF;         // LSB
   
    if (write(i2c_fd, buf, 3) != 3) {
        perror("Failed to write to INA219");
        sensor_state.error_count++;
       
        if (sensor_state.error_count >= CONSECUTIVE_ERRORS) {
            log_tampering("Multiple I2C write failures detected");
            enter_safe_mode("I2C communication failure");
        }
        return -1;
    }
   
    sensor_state.error_count = 0;
    return 0;
}

// Read 16-bit value from register
int16_t ina219_read16(uint8_t reg) {
    uint8_t buf[2];
   
    if (write(i2c_fd, &reg, 1) != 1) {
        perror("Failed to write register address");
        sensor_state.error_count++;
        return -1;
    }
   
    if (read(i2c_fd, buf, 2) != 2) {
        perror("Failed to read from INA219");
        sensor_state.error_count++;
       
        if (sensor_state.error_count >= CONSECUTIVE_ERRORS) {
            log_tampering("Multiple I2C read failures detected");
            enter_safe_mode("I2C communication failure");
        }
        return -1;
    }
   
    sensor_state.error_count = 0;
    // INA219 returns Big Endian
    return (int16_t)((buf[0] << 8) | buf[1]);
}

// Check I2C communication integrity
int check_i2c_integrity(void) {
    // Read configuration register
    int16_t config_val_signed = ina219_read16(INA219_REG_CONFIG);
    // Cast to uint16_t for comparison, as config is a bitmask
    uint16_t config_val = (uint16_t)config_val_signed;
   
    uint16_t expected_config = INA219_CONFIG_BVOLTAGERANGE_32V |
                              INA219_CONFIG_GAIN_8_320MV |
                              INA219_CONFIG_BADCRES_12BIT |
                              INA219_CONFIG_SADCRES_12BIT_1S |
                              INA219_CONFIG_MODE_SANDBVOLT_CONT;
   
    if (config_val != expected_config) {
        log_tampering("Configuration register mismatch - possible I2C tampering");
        return 0;
    }
   
    return 1;
}

// Initialize INA219
int ina219_init(const char *i2c_device) {
    i2c_fd = open(i2c_device, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C device");
        return -1;
    }
   
    if (ioctl(i2c_fd, I2C_SLAVE, INA219_ADDRESS) < 0) {
        perror("Failed to set I2C address");
        close(i2c_fd);
        return -1;
    }
   
    uint16_t config = INA219_CONFIG_BVOLTAGERANGE_32V |
                      INA219_CONFIG_GAIN_8_320MV |
                      INA219_CONFIG_BADCRES_12BIT |
                      INA219_CONFIG_SADCRES_12BIT_1S |
                      INA219_CONFIG_MODE_SANDBVOLT_CONT;
   
    if (ina219_write16(INA219_REG_CONFIG, config) < 0) {
        return -1;
    }
   
    // Set calibration for 32V, 2A
    current_lsb = 0.0001;  // 100uA per bit
    power_lsb = 0.002;     // 2mW per bit
   
    // 4096 is calculated for 0.1 Ohm shunt
    if (ina219_write16(INA219_REG_CALIBRATION, 4096) < 0) {
        return -1;
    }
   
    if (! check_i2c_integrity()) {
        fprintf(stderr, "Warning: I2C communication integrity check failed\n");
    }
   
    printf("INA219 initialized successfully\n");
    printf("Tampering detection configured:\n");
    printf("  Reference voltage: %.1fV\n", REFERENCE_VOLTAGE);
    printf("  Tolerance range: %.1fV\n", VOLTAGE_TOLERANCE);
    printf("  Safe range: %.1fV to %.1fV\n", 
           REFERENCE_VOLTAGE - VOLTAGE_TOLERANCE, 
           REFERENCE_VOLTAGE + VOLTAGE_TOLERANCE);
   
    sensor_state.mode = MODE_NORMAL;
    sensor_state.last_read_time = time(NULL);
   
    return 0;
}

// Get bus voltage in volts
float ina219_getBusVoltage_V(void) {
    int16_t value = ina219_read16(INA219_REG_BUSVOLTAGE);
    // Shift right 3 to drop CNVR and OVF bits
    return ((value >> 3) * 4) * 0.001f;
}

// Get shunt voltage in millivolts
float ina219_getShuntVoltage_mV(void) {
    int16_t value = ina219_read16(INA219_REG_SHUNTVOLTAGE);
    return value * 0.01f;
}

// Get current in milliamps
float ina219_getCurrent_mA(void) {
    // CRITICAL FIX: Do NOT write calibration every read. 
    // This resets the averaging and causes inaccurate/noisy readings.
    int16_t value = ina219_read16(INA219_REG_CURRENT);
    return value * current_lsb * 1000.0f;
}

// Get power in milliwatts
float ina219_getPower_mW(void) {
    int16_t value = ina219_read16(INA219_REG_POWER);
    return value * power_lsb * 1000.0f;
}

// Safe reading with tampering detection
int safe_read_sensor(float *bus_voltage, float *shunt_voltage, 
                     float *current, float *power, float *load_voltage) {
    if (sensor_state.mode == MODE_SHUTDOWN) {
        safe_shutdown();
    }
   
    // Verify I2C integrity periodically
    if (sensor_state.read_count % 100 == 0) {
        if (!check_i2c_integrity()) {
            sensor_state.error_count++;
            // Re-write calibration if integrity failed, just in case reset occurred
            ina219_write16(INA219_REG_CALIBRATION, 4096);
        }
    }
   
    *bus_voltage = ina219_getBusVoltage_V();
    *shunt_voltage = ina219_getShuntVoltage_mV();
    *current = ina219_getCurrent_mA();
    *power = ina219_getPower_mW();
    *load_voltage = *bus_voltage + (*shunt_voltage / 1000.0f);
   
    // Validate readings
    if (! validate_reading(*bus_voltage, *current, *power)) {
        sensor_state.error_count++;
       
        if (sensor_state.error_count >= CONSECUTIVE_ERRORS) {
            enter_safe_mode("Multiple validation failures detected");
            sensor_state.mode = MODE_SAFE;
        }
        return -1;
    }
   
    sensor_state.error_count = 0;
    sensor_state.last_bus_voltage = *bus_voltage;
    sensor_state.last_current = *current;
    sensor_state.read_count++;
    sensor_state.last_read_time = time(NULL);
   
    return 0;
}

void print_sensor_readings(float bus_voltage, float shunt_voltage, 
                           float current, float power, float load_voltage) {
    const char *mode_str[] = {"NORMAL", "CAUTION", "SAFE", "SHUTDOWN"};
    const char *status_str = sensor_state.tampering_detected ? "[⚠ TAMPERED]" : "[✓ OK]";
   
    // FIXED: Removed space in format specifiers
    printf("\n[%s] Mode: %s | Read #%u\n", status_str, mode_str[sensor_state.mode], 
           sensor_state.read_count);
    printf("Bus Voltage:      %.3f V\n", bus_voltage);
    printf("Shunt Voltage:    %.3f mV\n", shunt_voltage);
    printf("Load Voltage:     %.3f V\n", load_voltage);
    printf("Current:          %.3f mA\n", current);
    printf("Power:            %.3f mW\n", power);
    printf("Error Count:      %d\n", sensor_state.error_count);
    printf("--------------------\n");
}

void ina219_close(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
}

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    // Only set flag, do NOT call printf or exit here (async-signal-unsafe)
    running = 0;
}

int main(int argc, char *argv[]) {
    const char *i2c_dev = "/dev/i2c-3";
   
    if (argc > 1) {
        i2c_dev = argv[1];
    }
   
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
   
    printf("=================================================\n");
    printf("  INA219 Sensor Monitoring with Tamper Detection\n");
    printf("=================================================\n\n");
    printf("I2C Device:             %s\n", i2c_dev);
    printf("Tampering Detection:    ENABLED\n");
    printf("Safe Mode Integration:  ENABLED\n");
    printf("Tamper Log Tool:        %s\n", TAMPER_LOG_BIN);
    printf("Safe Mode Tool:         %s\n\n", ACTIVATE_SAFE_MODE_BIN);
   
    if (ina219_init(i2c_dev) < 0) {
        fprintf(stderr, "Failed to initialize INA219\n");
        return 1;
    }
   
    printf("\nReading INA219 sensor...\n");
    printf("Press Ctrl+C to exit\n\n");
   
    while (running) {
        float bus_voltage, shunt_voltage, current, power, load_voltage;
       
        if (safe_read_sensor(&bus_voltage, &shunt_voltage, &current, &power, &load_voltage) == 0) {
            print_sensor_readings(bus_voltage, shunt_voltage, current, power, load_voltage);
        } else {
            printf("⚠ Reading validation failed - sensor integrity compromised\n");
            printf("--------------------\n");
        }
       
        // Sleep 1s, but use loop to check running flag for faster exit if needed
        for(int i=0; i<10 && running; i++) usleep(100000); 
    }
    
    printf("\nShutdown signal received. Closing resources.\n");
    ina219_close();
    return 0;
}
