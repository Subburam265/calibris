#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <math.h>
#include <fcntl.h>
#include "hx711.h"
#include "lcd.h" // Include the LCD header

// --- GPIO and Delay Functions (keep these the same) ---
struct gpiod_chip* chip;
struct gpiod_line* dout_line;
struct gpiod_line* sck_line;

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

// --- Main Program ---
int main() {
    // --- Configuration ---
    const char* chipname = "gpiochip2";
    const int DOUT_PIN = 5;
    const int SCK_PIN = 4;
    const char* CALIBRATION_FILE = "calibration.txt";
    const char* TARE_FILE = "tare.txt";
    const char* I2C_BUS = "/dev/i2c-3"; // Correct I2C bus for your setup
    const int I2C_ADDR = 0x27;          // Correct I2C address for your setup

    float current_scale_factor = 1.0;
    long tare_offset = 0;
    FILE* file_ptr;

    // --- Set up non-blocking keyboard input ---
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // --- Initialize GPIO ---
    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) { perror("Error opening GPIO chip"); return 1; }
    dout_line = gpiod_chip_get_line(chip, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip, SCK_PIN);
    if (gpiod_line_request_input(dout_line, "hx711") < 0 || gpiod_line_request_output(sck_line, "hx711", 0) < 0) {
        perror("Error requesting GPIO lines");
        gpiod_chip_close(chip);
        return 1;
    }

    // --- Initialize LCD ---
    printf("Initializing LCD on %s at address 0x%X...\n", I2C_BUS, I2C_ADDR);
    if (lcd_init(I2C_BUS, I2C_ADDR) != 0) {
        fprintf(stderr, "Failed to initialize LCD. Check connections and i2cdetect.\n");
        gpiod_chip_close(chip);
        return 1;
    }
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Scale Starting..");
    my_delay_ms(1500); // Wait a moment

    // --- Initialize HX711 ---
    hx711_t scale;
    hx711_init(&scale, DOUT_PIN, SCK_PIN, my_gpio_write, my_gpio_read, my_delay_us, my_delay_ms);

    // --- Load settings from files ---
    printf("Loading settings...\n");

    // Load calibration factor
    file_ptr = fopen(CALIBRATION_FILE, "r");
    if (file_ptr != NULL) {
        fscanf(file_ptr, "%f", &current_scale_factor);
        fclose(file_ptr);
        if (current_scale_factor == 0.0) {
             printf(" -> WARNING: Calibration factor is 0. Using 1.0 temporarily.\n");
             current_scale_factor = 1.0;
        }
        hx711_set_scale(&scale, current_scale_factor);
        printf(" -> Calibration factor loaded: %.4f\n", current_scale_factor);
    } else {
        printf(" -> Calibration file not found. Please calibrate using 'c'.\n");
        hx711_set_scale(&scale, 1.0);
    }

    // Load tare offset
    file_ptr = fopen(TARE_FILE, "r");
    if (file_ptr != NULL) {
        fscanf(file_ptr, "%ld", &tare_offset);
        fclose(file_ptr);
        hx711_set_offset(&scale, tare_offset);
        printf(" -> Tare offset loaded: %ld\n", tare_offset);
    } else {
        printf(" -> Tare file not found. Performing initial tare...\n");
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_send_string("Taring...");
        hx711_tare(&scale, 20);
        tare_offset = hx711_get_offset(&scale);
        file_ptr = fopen(TARE_FILE, "w");
        fprintf(file_ptr, "%ld", tare_offset);
        fclose(file_ptr);
        printf(" -> Tare complete. New offset %ld saved.\n", tare_offset);
    }

    printf("\nReady for measurements.\n");
    printf(">>> Press 't' to re-tare, or 'c' to calibrate in this terminal. <<<\n\n");

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Weight:");

    // --- Measurement Loop ---
    while (1) {
        // Non-blocking check for keyboard input for tare/calibration
        char c = -1;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            
            // *** TARE LOGIC ADDED HERE ***
            if (c == 't') {
                printf("\n>>> Re-Taring... do not touch the scale. <<<\n");
                
                // Display on LCD
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_send_string("Re-Taring...");
                lcd_set_cursor(1, 0);
                lcd_send_string("Do not touch!");

                hx711_tare(&scale, 20); // Perform the tare
                tare_offset = hx711_get_offset(&scale); // Get the new offset

                // Save the new offset to file
                file_ptr = fopen(TARE_FILE, "w");
                if (file_ptr != NULL) {
                    fprintf(file_ptr, "%ld", tare_offset);
                    fclose(file_ptr);
                    printf(">>> Tare complete. New offset %ld saved. <<<\n", tare_offset);
                } else {
                    perror("Error saving tare file");
                }
                
                my_delay_ms(1500); // Pause to show message

                // Restore normal display
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_send_string("Weight:");
            }
            // You could add an 'else if (c == 'c')' block here for calibration
        }

        float weight = hx711_get_units(&scale, 5);
        if (fabsf(weight) < 0.5) weight = 0.0;

        // --- Display on Console ---
        printf("Weight: %+.2f g          \r", weight);
        fflush(stdout);

        // --- Display on LCD ---
        char lcd_buffer[17]; // 16 chars + null terminator
        snprintf(lcd_buffer, sizeof(lcd_buffer), "%8.2f g", weight);
        lcd_set_cursor(1, 0);       // Set cursor to the beginning of the second line
        lcd_send_string("                "); // Clear the line first
        lcd_set_cursor(1, 0);       // Reset cursor
        lcd_send_string(lcd_buffer);

        my_delay_ms(250); // A short delay
    }

    // --- Cleanup ---
    printf("\nCleaning up and exiting.\n");
    gpiod_line_release(dout_line);
    gpiod_line_release(sck_line);
    gpiod_chip_close(chip);
    lcd_clear();
    lcd_set_cursor(0,0);
    lcd_send_string("Goodbye!");
    lcd_close(); // Clean up the LCD

    return 0;
}
