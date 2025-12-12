#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <math.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include "hx711.h"
#include "lcd.h"
#include "cJSON.h"

// Include Tamper Log Library
// (Adjust path if your folder structure differs)
#include "../lib/tamper_logs.h" 

// --- System Paths ---
#define CONFIG_JSON_PATH "/home/pico/calibris/data/config.json"
#define SAFE_MODE_BIN    "/usr/local/bin/activate_safe_mode_bin/activate_safe_mode" // Path to your compiled activator

// --- Calibration Settings ---
#define CALIB_WEIGHT_MID  500.0f   // 500g
#define CALIB_WEIGHT_HIGH 1000.0f  // 1kg

// --- Forensic Thresholds (TUNE THESE FOR YOUR HARDWARE) ---
#define LINEARITY_TOLERANCE    0.10f   // Ratio must be 2.0 +/- 0.1
#define CALIB_FACTOR_TOLERANCE 0.15f   // New factor must be within 15% of old factor
#define MIN_RAW_COUNTS_500G    100000  // Minimum acceptable raw reading for 500g
                                       // (Prevents using a coin to simulate 500g)

// Structure to hold config values
typedef struct {
    float calibration_factor;
    long tare_offset;
} config_struct;

// --- GPIO Globals ---
struct gpiod_chip* chip_scale;   
struct gpiod_chip* chip_buttons; 
struct gpiod_line* dout_line;
struct gpiod_line* sck_line;
struct gpiod_line* tare_line;    // Pin 19
struct gpiod_line* calib_line;   // Pin 15
struct gpiod_line* enter_line;   // Pin 14

// --- Helper Functions ---

void my_gpio_write(int pin, int value) {
    gpiod_line_set_value(sck_line, value);
}

int my_gpio_read(int pin) {
    return gpiod_line_get_value(dout_line);
}

void my_delay_us(unsigned int us) {
    usleep(us);
}

void my_delay_ms(unsigned int ms) {
    usleep(ms * 1000);
}

void wait_for_enter_button() {
    while (gpiod_line_get_value(enter_line) == 0) { my_delay_ms(50); }
    while (gpiod_line_get_value(enter_line) == 1) { my_delay_ms(50); }
    my_delay_ms(200); 
}

// --- Trigger Safe Mode (The Active Defense) ---
void trigger_safe_mode() {
    // 1. Notify User
    lcd_clear(); 
    lcd_set_cursor(0, 0); lcd_send_string("SYSTEM LOCKING..");
    lcd_set_cursor(1, 0); lcd_send_string("Safe Mode Active");
    
    // 2. Execute external activator
    // Passing the config path ensures it updates the correct file
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s %s", SAFE_MODE_BIN, CONFIG_JSON_PATH);
    int ret = system(cmd);

    if (ret != 0) {
        // Fallback if binary fails: Force reboot to ensure service lockout takes effect
        system("reboot"); 
    }
    
    // 3. Infinite loop to halt current operation until service restarts/stops
    while(1) { sleep(1); } 
}

// --- JSON Config Management ---
int read_config_json(const char *path, config_struct *out) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    rewind(fp);
    char *data = malloc(length + 1);
    fread(data, 1, length, fp);
    data[length] = '\0';
    fclose(fp);

    cJSON *j = cJSON_Parse(data);
    free(data);
    if (!j) return 2;

    cJSON *calib = cJSON_GetObjectItem(j, "calibration_factor");
    cJSON *tare = cJSON_GetObjectItem(j, "tare_offset");

    if (cJSON_IsNumber(calib) && cJSON_IsNumber(tare)) {
        out->calibration_factor = (float)calib->valuedouble;
        out->tare_offset = (long)tare->valuedouble;
    } else {
        out->calibration_factor = 400.0f; // Default safety
        out->tare_offset = 0;
    }
    cJSON_Delete(j);
    return 0;
}

int write_config_json(const char *path, float calibration_factor, long tare_offset) {
    FILE *fp = fopen(path, "r+");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    rewind(fp);
    char *data = malloc(length + 1);
    fread(data, 1, length, fp);
    data[length] = '\0';

    cJSON *j = cJSON_Parse(data);
    free(data);
    if (!j) { fclose(fp); return 2; }

    cJSON_ReplaceItemInObject(j, "calibration_factor", cJSON_CreateNumber(calibration_factor));
    cJSON_ReplaceItemInObject(j, "tare_offset", cJSON_CreateNumber(tare_offset));

    char *out = cJSON_Print(j);
    fseek(fp, 0, SEEK_SET);
    fwrite(out, 1, strlen(out), fp);
    fflush(fp);
    ftruncate(fileno(fp), strlen(out));
    fclose(fp);
    cJSON_Delete(j);
    free(out);
    return 0;
}

void perform_tare(hx711_t* scale) {
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Re-Taring...");
    lcd_set_cursor(1, 0); lcd_send_string("Do not touch!");
    
    hx711_tare(scale, 20);
    long new_offset = hx711_get_offset(scale);
    write_config_json(CONFIG_JSON_PATH, scale->scale, new_offset);
    
    my_delay_ms(1500);
}

// --- SECURE CALIBRATION FUNCTION ---
void perform_secure_calibration(hx711_t* scale) {
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Secure Calib");
    lcd_set_cursor(1, 0); lcd_send_string("Init Check...");
    my_delay_ms(1500);

    // 1. Retrieve Historical Data
    config_struct old_conf;
    float old_factor = 400.0f; // Default fallback
    if (read_config_json(CONFIG_JSON_PATH, &old_conf) == 0) {
        old_factor = old_conf.calibration_factor;
        if (old_factor < 10.0f) old_factor = 400.0f;
    }

    // --- Point 1: ZERO ---
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("1. Empty Scale");
    lcd_set_cursor(1, 0); lcd_send_string("Press Enter...");
    wait_for_enter_button();
    
    lcd_set_cursor(1, 0); lcd_send_string("Measuring Zero..");
    hx711_tare(scale, 20);
    long new_offset = hx711_get_offset(scale);

    // --- Point 2: 500g ---
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("2. Place 500g");
    lcd_set_cursor(1, 0); lcd_send_string("Press Enter...");
    wait_for_enter_button();
    
    lcd_set_cursor(1, 0); lcd_send_string("Measuring...");
    long raw_w1 = hx711_read_average(scale, 20);
    double signal_mid = (double)(raw_w1 - new_offset);

    // [SECURITY CHECK 1] Absolute Raw Value Check
    // Detects "Coin Attacks" (using light objects to fake heavy weights)
    if (signal_mid < MIN_RAW_COUNTS_500G) {
        log_tamper("calib_underweight", "Raw signal too low for 500g");
        lcd_clear(); lcd_send_string("ERR: INVALID WGT");
        lcd_set_cursor(1, 0); lcd_send_string("Check Sensor!");
        my_delay_ms(3000);
        return; // Abort safely (no safe mode, just reject)
    }

    float factor1 = (float)signal_mid / CALIB_WEIGHT_MID;

    // --- Point 3: 1000g ---
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("3. Place 1000g");
    lcd_set_cursor(1, 0); lcd_send_string("Press Enter...");
    wait_for_enter_button();
    
    lcd_set_cursor(1, 0); lcd_send_string("Measuring...");
    long raw_w2 = hx711_read_average(scale, 20);
    double signal_high = (double)(raw_w2 - new_offset);
    float factor2 = (float)signal_high / CALIB_WEIGHT_HIGH;

    // [SECURITY CHECK 2] Linearity Ratio
    // Detects "Double Tap" (using same weight twice)
    double actual_ratio = signal_high / signal_mid;
    if (fabs(actual_ratio - 2.0) > LINEARITY_TOLERANCE) {
        char details[128];
        snprintf(details, sizeof(details), "Linearity Fail: Ratio %.2f", actual_ratio);
        
        log_tamper("calib_linearity", details);
        lcd_clear(); lcd_send_string("TAMPER DETECTED!");
        lcd_set_cursor(1,0); lcd_send_string("Linearity Err");
        my_delay_ms(2000);
        
        trigger_safe_mode(); // LOCK SYSTEM
        return; 
    }

    // --- Calculate New Factor ---
    float new_factor = (factor1 + factor2) / 2.0f;

    // [SECURITY CHECK 3] Historical Factor Deviation
    // Detects "Scaling Attack" (using 50g & 100g to fake 500g & 1000g)
    float deviation = fabsf(new_factor - old_factor) / old_factor;
    
    if (deviation > CALIB_FACTOR_TOLERANCE) {
        char details[128];
        snprintf(details, sizeof(details), "Drift: Old:%.1f New:%.1f (%.0f%%)", 
                 old_factor, new_factor, deviation * 100);
        
        log_tamper("calib_sensitivity", details);
        
        lcd_clear(); lcd_send_string("TAMPER DETECTED!");
        lcd_set_cursor(1,0); lcd_send_string("Sensor Drift");
        my_delay_ms(2000);
        
        trigger_safe_mode(); // LOCK SYSTEM
        return;
    }

    // --- Success: Save Data ---
    hx711_set_scale(scale, new_factor);
    write_config_json(CONFIG_JSON_PATH, new_factor, new_offset);

    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Calib Secured!");
    char buf[17];
    snprintf(buf, 16, "F: %.1f", new_factor);
    lcd_set_cursor(1, 0); lcd_send_string(buf);
    my_delay_ms(3000);
}

// --- Main Program ---
int main() {
    // GPIO Config
    const char* chipname_scale = "gpiochip2";
    const char* chipname_buttons = "gpiochip1";
    const int DOUT_PIN = 5;
    const int SCK_PIN = 4;
    const int TARE_PIN = 19;   
    const int CALIB_PIN = 18;  
    const int ENTER_PIN = 17;  

    const char* I2C_BUS = "/dev/i2c-3";
    const int I2C_ADDR = 0x27;

    // Init GPIO
    chip_scale = gpiod_chip_open_by_name(chipname_scale);
    chip_buttons = gpiod_chip_open_by_name(chipname_buttons);
    if (!chip_scale || !chip_buttons) { perror("GPIO Error"); return 1; }

    dout_line = gpiod_chip_get_line(chip_scale, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip_scale, SCK_PIN);
    tare_line = gpiod_chip_get_line(chip_buttons, TARE_PIN);
    calib_line = gpiod_chip_get_line(chip_buttons, CALIB_PIN);
    enter_line = gpiod_chip_get_line(chip_buttons, ENTER_PIN);

    gpiod_line_request_input(dout_line, "hx711_dout");
    gpiod_line_request_output(sck_line, "hx711_sck", 0);
    gpiod_line_request_input(tare_line, "tare_btn");
    gpiod_line_request_input(calib_line, "calib_btn");
    gpiod_line_request_input(enter_line, "enter_btn");

    // Init LCD
    if (lcd_init(I2C_BUS, I2C_ADDR) != 0) {
        fprintf(stderr, "LCD Init Failed\n");
        return 1;
    }
    lcd_clear(); lcd_send_string("System Start...");

    // Init HX711
    hx711_t scale;
    hx711_init(&scale, DOUT_PIN, SCK_PIN, my_gpio_write, my_gpio_read, my_delay_us, my_delay_ms);

    // Load Config
    config_struct conf;
    if (read_config_json(CONFIG_JSON_PATH, &conf) == 0) {
        hx711_set_scale(&scale, conf.calibration_factor);
        hx711_set_offset(&scale, conf.tare_offset);
    } else {
        hx711_set_scale(&scale, 1.0);
        hx711_tare(&scale, 20);
    }

    lcd_clear(); lcd_send_string("Ready to Weigh");

    // Main Loop
    while (1) {
        bool update_screen = false;

        // 1. Tare
        if (gpiod_line_get_value(tare_line) == 1) {
            perform_tare(&scale);
            update_screen = true;
            while (gpiod_line_get_value(tare_line) == 1) my_delay_ms(50);
        }

        // 2. Secure Calibration
        if (gpiod_line_get_value(calib_line) == 1) {
            perform_secure_calibration(&scale);
            update_screen = true;
            while (gpiod_line_get_value(calib_line) == 1) my_delay_ms(50);
        }

        if (update_screen) {
            lcd_clear(); lcd_send_string("Weight:");
        }

        float weight = hx711_get_units(&scale, 5);
        if (fabsf(weight) < 0.5) weight = 0.0;

        char lcd_buffer[17];
        snprintf(lcd_buffer, sizeof(lcd_buffer), "%8.2f g", weight);
        lcd_set_cursor(1, 0); lcd_send_string("                ");
        lcd_set_cursor(1, 0); lcd_send_string(lcd_buffer);

        my_delay_ms(250);
    }

    return 0;
}
