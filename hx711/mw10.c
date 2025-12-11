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

#define CONFIG_JSON_PATH "/home/pico/calibris/data/config.json"

// --- Calibration Weights for 1kg Load Cell ---
#define CALIB_WEIGHT_MID  500.0f   // 500g
#define CALIB_WEIGHT_HIGH 1000.0f  // 1kg

// Structure to hold config values
typedef struct {
    float calibration_factor;
    long tare_offset;
} config_struct;

// --- GPIO and Delay Functions ---
struct gpiod_chip* chip_scale;   // Handles HX711 (gpiochip2)
struct gpiod_chip* chip_buttons; // Handles Buttons (gpiochip1)
struct gpiod_line* dout_line;
struct gpiod_line* sck_line;
struct gpiod_line* tare_line;    // Pin 19
struct gpiod_line* calib_line;   // Pin 15 (Offset 18)
struct gpiod_line* enter_line;   // Pin 14 (Offset 17) - NEW

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

// --- Helper: Wait for Button Press (Pin 14) ---
void wait_for_enter_button() {
    // Wait for press (High)
    while (gpiod_line_get_value(enter_line) == 0) {
        my_delay_ms(50); 
    }
    // Debounce / Wait for release
    while (gpiod_line_get_value(enter_line) == 1) {
        my_delay_ms(50);
    }
    my_delay_ms(200); // Extra safety delay
}

// --- JSON Config Read/Write ---
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

// --- Helper: Standard Tare ---
void perform_tare(hx711_t* scale) {
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Re-Taring...");
    lcd_set_cursor(1, 0); lcd_send_string("Do not touch!");
    
    hx711_tare(scale, 20);
    long new_offset = hx711_get_offset(scale);
    write_config_json(CONFIG_JSON_PATH, scale->scale, new_offset);
    
    my_delay_ms(1500);
}

// --- Helper: 3-Point Calibration (1kg Load Cell) ---
void perform_3_point_calibration(hx711_t* scale) {
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("3-Point Calib");
    lcd_set_cursor(1, 0); lcd_send_string("1kg Mode");
    my_delay_ms(2000);

    // --- Point 1: ZERO ---
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("1. Empty Scale");
    lcd_set_cursor(1, 0); lcd_send_string("Press Pin 14...");
    
    wait_for_enter_button(); // Wait for GPIO Pin 14

    lcd_set_cursor(1, 0); lcd_send_string("Measuring Zero..");
    hx711_tare(scale, 20);
    long new_offset = hx711_get_offset(scale);

    // --- Point 2: 500g ---
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("2. Place 500g");
    lcd_set_cursor(1, 0); lcd_send_string("Press Pin 14...");
    
    wait_for_enter_button(); // Wait for GPIO Pin 14

    lcd_set_cursor(1, 0); lcd_send_string("Measuring...");
    long raw_w1 = hx711_read_average(scale, 20);
    float factor1 = (float)(raw_w1 - new_offset) / CALIB_WEIGHT_MID;

    // --- Point 3: 1000g ---
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("3. Place 1000g");
    lcd_set_cursor(1, 0); lcd_send_string("Press Pin 14...");

    wait_for_enter_button(); // Wait for GPIO Pin 14

    lcd_set_cursor(1, 0); lcd_send_string("Measuring...");
    long raw_w2 = hx711_read_average(scale, 20);
    float factor2 = (float)(raw_w2 - new_offset) / CALIB_WEIGHT_HIGH;

    // --- Averaging & Saving ---
    float final_factor = (factor1 + factor2) / 2.0f;

    hx711_set_scale(scale, final_factor);
    write_config_json(CONFIG_JSON_PATH, final_factor, new_offset);

    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Calib Saved!");
    char buf[17];
    snprintf(buf, 16, "F: %.1f", final_factor);
    lcd_set_cursor(1, 0); lcd_send_string(buf);
    
    my_delay_ms(3000);
}

// --- Main Program ---
int main() {
    // --- Configuration ---
    const char* chipname_scale = "gpiochip2";
    const char* chipname_buttons = "gpiochip1"; 

    const int DOUT_PIN = 5;
    const int SCK_PIN = 4;

    // Button Pins (Bank 1)
    const int TARE_PIN = 19;   // GPIO1_C3
    const int CALIB_PIN = 18;  // GPIO1_C2 (Pin 15)
    const int ENTER_PIN = 17;  // GPIO1_C1 (Pin 14) - NEW

    const char* I2C_BUS = "/dev/i2c-3";
    const int I2C_ADDR = 0x27;

    // GPIO Init
    chip_scale = gpiod_chip_open_by_name(chipname_scale);
    chip_buttons = gpiod_chip_open_by_name(chipname_buttons);
    
    if (!chip_scale || !chip_buttons) { perror("GPIO Chip Error"); return 1; }

    dout_line = gpiod_chip_get_line(chip_scale, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip_scale, SCK_PIN);
    
    tare_line = gpiod_chip_get_line(chip_buttons, TARE_PIN);
    calib_line = gpiod_chip_get_line(chip_buttons, CALIB_PIN);
    enter_line = gpiod_chip_get_line(chip_buttons, ENTER_PIN); // Init new pin

    gpiod_line_request_input(dout_line, "hx711_dout");
    gpiod_line_request_output(sck_line, "hx711_sck", 0);
    
    gpiod_line_request_input(tare_line, "tare_btn");
    gpiod_line_request_input(calib_line, "calib_btn");
    gpiod_line_request_input(enter_line, "enter_btn"); // Request input

    // LCD Init
    if (lcd_init(I2C_BUS, I2C_ADDR) != 0) {
        fprintf(stderr, "LCD Init Failed\n");
        return 1;
    }

    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Scale Starting..");

    // HX711 Init
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

    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Weight:");

    while (1) {
        bool update_screen = false;

        // 1. Tare Button (Pin 19)
        if (gpiod_line_get_value(tare_line) == 1) {
            perform_tare(&scale);
            update_screen = true;
            while (gpiod_line_get_value(tare_line) == 1) my_delay_ms(50);
        }

        // 2. Calibration Button (Pin 15) -> Triggers 3-point Calib
        if (gpiod_line_get_value(calib_line) == 1) {
            perform_3_point_calibration(&scale);
            update_screen = true;
            while (gpiod_line_get_value(calib_line) == 1) my_delay_ms(50);
        }

        if (update_screen) {
            lcd_clear();
            lcd_set_cursor(0, 0); lcd_send_string("Weight:");
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
