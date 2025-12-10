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
#include "cJSON.h" // include cJSON

#define CONFIG_JSON_PATH "/home/pico/calibris/data/config.json"

// Structure to hold config values
typedef struct {
    float calibration_factor;
    long tare_offset;
} config_struct;

// --- GPIO and Delay Functions ---
struct gpiod_chip* chip_scale; // Handles HX711 (gpiochip2)
struct gpiod_chip* chip_tare;  // NEW: Handles Tare Button (gpiochip1)
struct gpiod_line* dout_line;
struct gpiod_line* sck_line;
struct gpiod_line* tare_line;

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
// Read calibration and tare from config.json
int read_config_json(const char *path, config_struct *out) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("Error opening config.json");
        return 1;
    }
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
    if (!cJSON_IsNumber(calib) || !cJSON_IsNumber(tare)) {
        cJSON_Delete(j);
        return 3;
    }
    out->calibration_factor = (float)calib->valuedouble;
    out->tare_offset = (long)tare->valuedouble;
    cJSON_Delete(j);
    return 0;
}

// Write calibration factor and tare offset to config.json, preserving all other fields
int write_config_json(const char *path, float calibration_factor, long tare_offset) {
    FILE *fp = fopen(path, "r+");
    if (!fp) {
        perror("Error opening config.json for update");
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    rewind(fp);
    char *data = malloc(length + 1);
    fread(data, 1, length, fp);
    data[length] = '\0';

    cJSON *j = cJSON_Parse(data);
    free(data);
    if (!j) {
        fclose(fp);
        return 2;
    }
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

// --- Helper function for Taring ---
// No file writing, uses config.json
void perform_tare(hx711_t* scale) {
    printf("\n>>> Re-Taring... do not touch the scale. <<<\n");
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Re-Taring...");
    lcd_set_cursor(1, 0);
    lcd_send_string("Do not touch!");

    hx711_tare(scale, 20);
    long new_offset = hx711_get_offset(scale);

    if (write_config_json(CONFIG_JSON_PATH, scale->scale, new_offset) == 0) {
        printf(">>> Tare complete. New offset %ld saved. <<<\n", new_offset);
    } else {
        perror("Error saving tare to config.json");
    }
    my_delay_ms(1500);
}

// --- Main Program ---
int main() {
    // --- Configuration ---
    // Chip names
    const char* chipname_scale = "gpiochip2";
    const char* chipname_tare = "gpiochip1"; // NEW: Tare is on Bank 1
    
    // Scale Pins (Bank 2)
    const int DOUT_PIN = 5;
    const int SCK_PIN = 4;
    
    // Tare Pin (Bank 1)
    // GPIO1_C3_d -> Group C starts at offset 16. C3 is 16 + 3 = 19.
    const int TARE_PIN = 19; 
    
    const char* I2C_BUS = "/dev/i2c-3";
    const int I2C_ADDR = 0x27;

    float current_scale_factor = 1.0f;
    long tare_offset = 0;

    // --- Set up non-blocking keyboard input ---
    int original_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, original_flags | O_NONBLOCK);

    // --- Initialize GPIO ---
    
    // 1. Open Scale Chip (Bank 2)
    chip_scale = gpiod_chip_open_by_name(chipname_scale);
    if (!chip_scale) { perror("Error opening Scale GPIO chip (gpiochip2)"); return 1; }
    
    // 2. Open Tare Chip (Bank 1)
    chip_tare = gpiod_chip_open_by_name(chipname_tare);
    if (!chip_tare) { 
        perror("Error opening Tare GPIO chip (gpiochip1)"); 
        gpiod_chip_close(chip_scale); // Clean up previous chip before exit
        return 1; 
    }

    // 3. Get Lines
    dout_line = gpiod_chip_get_line(chip_scale, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip_scale, SCK_PIN);
    tare_line = gpiod_chip_get_line(chip_tare, TARE_PIN);

    // 4. Request Lines
    if (gpiod_line_request_input(dout_line, "hx711_dout") < 0 ||
        gpiod_line_request_output(sck_line, "hx711_sck", 0) < 0) {
        perror("Error requesting Scale GPIO lines");
        gpiod_chip_close(chip_scale);
        gpiod_chip_close(chip_tare);
        return 1;
    }

    if (gpiod_line_request_input(tare_line, "tare_button") < 0) {
        perror("Error requesting Tare GPIO line");
        gpiod_chip_close(chip_scale);
        gpiod_chip_close(chip_tare);
        return 1;
    }

    // --- Initialize LCD ---
    printf("Initializing LCD on %s at address 0x%X...\n", I2C_BUS, I2C_ADDR);
    if (lcd_init(I2C_BUS, I2C_ADDR) != 0) {
        fprintf(stderr, "Failed to initialize LCD.\n");
        gpiod_line_release(dout_line);
        gpiod_line_release(sck_line);
        gpiod_line_release(tare_line);
        gpiod_chip_close(chip_scale);
        gpiod_chip_close(chip_tare);
        return 1;
    }

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Scale Starting..");
    my_delay_ms(1500);

    // --- Initialize HX711 ---
    hx711_t scale;
    hx711_init(&scale, DOUT_PIN, SCK_PIN, my_gpio_write, my_gpio_read, my_delay_us, my_delay_ms);

    // --- Load settings from config.json ---
    printf("Loading settings from config.json...\n");
    config_struct conf;
    if (read_config_json(CONFIG_JSON_PATH, &conf) == 0) {
        current_scale_factor = conf.calibration_factor + 50;
        tare_offset = conf.tare_offset;
        hx711_set_scale(&scale, current_scale_factor);
        hx711_set_offset(&scale, tare_offset);
        printf(" -> Calibration factor loaded: %.4f\n", current_scale_factor);
        printf(" -> Tare offset loaded: %ld\n", tare_offset);
    } else {
        printf(" -> config.json not found/invalid. Please calibrate.\n");
        hx711_set_scale(&scale, 1.0);
        hx711_tare(&scale, 20);
    }

    printf("\nReady for measurements.\n");
    printf(">>> Press 't' to re-tare, or 'c' to calibrate. Use GPIO pin to tare. <<<\n\n");

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Weight:");

    // --- Measurement Loop ---
    while (1) {
        bool action_taken = false;

        // Check for tare button (GPIO)
        if (gpiod_line_get_value(tare_line) == 1) {
            perform_tare(&scale);
            action_taken = true;
            while (gpiod_line_get_value(tare_line) == 1) {
                my_delay_ms(50);
            }
        }

        // Check for keyboard commands
        char c = -1;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            action_taken = true;
            if (c == 't') {
                perform_tare(&scale);
            }
            else if (c == 'c') {
                // Temporarily make stdin blocking
                fcntl(STDIN_FILENO, F_SETFL, original_flags);

                printf("\n--- Calibration --- \n");
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_send_string("Calibration Mode");

                printf("Enter the known weight in grams (e.g., 100.0): ");
                float known_weight_g;
                scanf("%f", &known_weight_g);
                while(getchar() != '\n' && getchar() != EOF);

                printf("Place the %.2fg weight on the scale and press Enter.", known_weight_g);
                lcd_set_cursor(1, 0);
                lcd_send_string("Place weight...");
                getchar();

                printf("Measuring... please wait.\n");
                lcd_set_cursor(1, 0);
                lcd_send_string("Measuring...      ");

                long raw_reading = hx711_read_average(&scale, 20);
                tare_offset = hx711_get_offset(&scale);

                if (known_weight_g != 0) {
                    current_scale_factor = (float)(raw_reading - tare_offset) / known_weight_g;
                    hx711_set_scale(&scale, current_scale_factor);

                    // Write both calibration factor and tare to config.json
                    write_config_json(CONFIG_JSON_PATH, current_scale_factor, tare_offset);

                    printf("\n--- Calibration Complete! ---\n");
                    printf("New scale factor is: %.4f\n", current_scale_factor);
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_send_string("Calib. Complete!");
                    my_delay_ms(2000);
                } else {
                    printf("Known weight cannot be zero. Calibration cancelled.\n");
                    lcd_set_cursor(0,0);
                    lcd_send_string("Error: Weight=0");
                    my_delay_ms(2000);
                }
                fcntl(STDIN_FILENO, F_SETFL, original_flags | O_NONBLOCK);
            }
        }

        if (action_taken) {
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("Weight:");
        }

        float weight = hx711_get_units(&scale, 5);
        if (fabsf(weight) < 0.5) weight = 0.0;

        printf("Weight: %+.2f g            \r", weight);
        fflush(stdout);

        char lcd_buffer[17];
        snprintf(lcd_buffer, sizeof(lcd_buffer), "%8.2f g", weight);
        lcd_set_cursor(1, 0);
        lcd_send_string("                ");
        lcd_set_cursor(1, 0);
        lcd_send_string(lcd_buffer);

        my_delay_ms(250);
    }

    // --- Cleanup ---
    printf("\nCleaning up and exiting.\n");
    gpiod_line_release(dout_line);
    gpiod_line_release(sck_line);
    gpiod_line_release(tare_line);
    
    gpiod_chip_close(chip_scale);
    gpiod_chip_close(chip_tare);
    
    lcd_clear();
    lcd_set_cursor(0,0);
    lcd_send_string("Goodbye!");
    lcd_close();
    return 0;
}
