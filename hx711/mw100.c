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
#define CALIBRATION_WEIGHT_G 200.0f  // HARDCODED CALIBRATION WEIGHT

// Structure to hold config values
typedef struct {
    float calibration_factor;
    long tare_offset;
} config_struct;

// --- GPIO and Delay Functions ---
struct gpiod_chip* chip_scale;
struct gpiod_chip* chip_buttons;
struct gpiod_line* dout_line;
struct gpiod_line* sck_line;
struct gpiod_line* tare_line;
struct gpiod_line* calib_line;

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
    
    my_delay_ms(1000);
}

// --- Helper: Simple 200g Calibration ---
void perform_200g_calibration(hx711_t* scale, int original_flags) {
    // Restore blocking input just in case
    fcntl(STDIN_FILENO, F_SETFL, original_flags);

    printf("\n=== 200g Calibration Mode ===\n");
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Calibrate 200g");

    // --- Step 1: Zero ---
    printf("1. Remove all weight. Press Enter...\n");
    lcd_set_cursor(1, 0); lcd_send_string("Empty Scale...  ");
    while(getchar() != '\n'); // Wait for Enter

    printf("   Zeroing...\n");
    hx711_tare(scale, 20);
    long new_offset = hx711_get_offset(scale);

    // --- Step 2: Measure 200g ---
    printf("2. Place 200g weight. Press Enter...\n");
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Place 200g Wgt");
    lcd_set_cursor(1, 0); lcd_send_string("Then Press Enter");
    while(getchar() != '\n'); // Wait for Enter

    printf("   Measuring...\n");
    lcd_set_cursor(1, 0); lcd_send_string("Measuring...    ");
    
    long raw_reading = hx711_read_average(scale, 20);
    float new_factor = (float)(raw_reading - new_offset) / CALIBRATION_WEIGHT_G;

    printf("   New Factor: %.4f\n", new_factor);
    
    // --- Step 3: Save ---
    hx711_set_scale(scale, new_factor);
    write_config_json(CONFIG_JSON_PATH, new_factor, new_offset);
    
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Calib Saved!");
    lcd_set_cursor(1, 0); lcd_send_string("Factor updated");
    
    // Restore non-blocking
    fcntl(STDIN_FILENO, F_SETFL, original_flags | O_NONBLOCK);
    my_delay_ms(2000);
}

// --- Main Program ---
int main() {
    // Pins
    const char* chipname_scale = "gpiochip2";
    const char* chipname_buttons = "gpiochip1"; 
    const int DOUT_PIN = 5;
    const int SCK_PIN = 4;
    const int TARE_PIN = 19;
    const int CALIB_PIN = 18; // Pin 15
    
    // LCD
    const char* I2C_BUS = "/dev/i2c-3";
    const int I2C_ADDR = 0x27;

    // Setup Input
    int original_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, original_flags | O_NONBLOCK);

    // GPIO Init
    chip_scale = gpiod_chip_open_by_name(chipname_scale);
    chip_buttons = gpiod_chip_open_by_name(chipname_buttons);
    
    dout_line = gpiod_chip_get_line(chip_scale, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip_scale, SCK_PIN);
    tare_line = gpiod_chip_get_line(chip_buttons, TARE_PIN);
    calib_line = gpiod_chip_get_line(chip_buttons, CALIB_PIN);

    gpiod_line_request_input(dout_line, "hx711_dout");
    gpiod_line_request_output(sck_line, "hx711_sck", 0);
    gpiod_line_request_input(tare_line, "tare_btn");
    gpiod_line_request_input(calib_line, "calib_btn");

    // LCD Init
    lcd_init(I2C_BUS, I2C_ADDR);
    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Scale Starting");

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

    printf("Ready. Press Pin 15 (Calib) to calibrate for 200g.\n");

    lcd_clear();
    lcd_set_cursor(0, 0); lcd_send_string("Weight:");

    while (1) {
        bool update_screen = false;

        // Tare Button
        if (gpiod_line_get_value(tare_line) == 1) {
            perform_tare(&scale);
            update_screen = true;
            while (gpiod_line_get_value(tare_line) == 1) my_delay_ms(50);
        }

        // Calib Button (200g fixed)
        if (gpiod_line_get_value(calib_line) == 1) {
            perform_200g_calibration(&scale, original_flags);
            update_screen = true;
            while (gpiod_line_get_value(calib_line) == 1) my_delay_ms(50);
        }

        // Keyboard 'c' or 't'
        char c = -1;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == 't') { perform_tare(&scale); update_screen = true; }
            if (c == 'c') { perform_200g_calibration(&scale, original_flags); update_screen = true; }
        }

        if (update_screen) {
            lcd_clear();
            lcd_set_cursor(0, 0); lcd_send_string("Weight:");
        }

        // Display Weight
        float weight = hx711_get_units(&scale, 5);
        if (fabsf(weight) < 0.5) weight = 0.0;

        printf("Weight: %+.2f g            \r", weight);
        fflush(stdout);

        char lcd_buffer[17];
        snprintf(lcd_buffer, sizeof(lcd_buffer), "%8.2f g", weight);
        lcd_set_cursor(1, 0); lcd_send_string("                ");
        lcd_set_cursor(1, 0); lcd_send_string(lcd_buffer);

        my_delay_ms(250);
    }
    return 0;
}
