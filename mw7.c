#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <math.h>
#include <fcntl.h>
#include <stdbool.h> // For boolean type
#include "hx711.h"
#include "lcd.h" // Your LCD header file

// --- GPIO and Delay Functions (keep these the same) ---
struct gpiod_chip* chip;
struct gpiod_line* dout_line;
struct gpiod_line* sck_line;
struct gpiod_line* tare_line; // Line for the tare button

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

// --- Helper function for Taring ---
// This function contains the logic for performing a tare,
// displaying messages, and saving the new offset.
void perform_tare(hx711_t* scale, const char* tare_file) {
    printf("\n>>> Re-Taring... do not touch the scale. <<<\n");
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Re-Taring...");
    lcd_set_cursor(1, 0);
    lcd_send_string("Do not touch!");

    hx711_tare(scale, 20);
    long new_offset = hx711_get_offset(scale);

    FILE* file_ptr = fopen(tare_file, "w");
    if (file_ptr != NULL) {
        fprintf(file_ptr, "%ld", new_offset);
        fclose(file_ptr);
        printf(">>> Tare complete. New offset %ld saved. <<<\n", new_offset);
    } else {
        perror("Error saving tare file");
    }
    my_delay_ms(1500); // Give user time to see the message
}


// --- Main Program ---
int main() {
    // --- Configuration ---
    const char* chipname = "gpiochip2";
    const int DOUT_PIN = 5;
    const int SCK_PIN = 4;
    const int TARE_PIN = 0; // GPIO2_A0_d corresponds to line offset 0 on gpiochip2
    const char* CALIBRATION_FILE = "calibration.txt";
    const char* TARE_FILE = "tare.txt";
    const char* I2C_BUS = "/dev/i2c-3";
    const int I2C_ADDR = 0x27;

    float current_scale_factor = 1.0;
    long tare_offset = 0;
    FILE* file_ptr;

    // --- Set up non-blocking keyboard input ---
    int original_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, original_flags | O_NONBLOCK);

    // --- Initialize GPIO ---
    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) { perror("Error opening GPIO chip"); return 1; }
    dout_line = gpiod_chip_get_line(chip, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip, SCK_PIN);
    tare_line = gpiod_chip_get_line(chip, TARE_PIN); // Get the tare line

    // Request all GPIO lines
    if (gpiod_line_request_input(dout_line, "hx711_dout") < 0 ||
        gpiod_line_request_output(sck_line, "hx711_sck", 0) < 0 ||
        gpiod_line_request_input(tare_line, "tare_button") < 0) {
        perror("Error requesting GPIO lines");
        gpiod_chip_close(chip);
        return 1;
    }

    // --- Initialize LCD ---
    printf("Initializing LCD on %s at address 0x%X...\n", I2C_BUS, I2C_ADDR);
    if (lcd_init(I2C_BUS, I2C_ADDR) != 0) {
        fprintf(stderr, "Failed to initialize LCD.\n");
        gpiod_chip_close(chip);
        return 1;
    }
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Scale Starting..");
    my_delay_ms(1500);

    // --- Initialize HX711 ---
    hx711_t scale;
    hx711_init(&scale, DOUT_PIN, SCK_PIN, my_gpio_write, my_gpio_read, my_delay_us, my_delay_ms);

    // --- Load settings (same as before) ---
    printf("Loading settings...\n");
    file_ptr = fopen(CALIBRATION_FILE, "r");
    if (file_ptr != NULL) {
        fscanf(file_ptr, "%f", &current_scale_factor);
        fclose(file_ptr);
        if (current_scale_factor == 0.0) { current_scale_factor = 1.0; }
        hx711_set_scale(&scale, current_scale_factor);
        printf(" -> Calibration factor loaded: %.4f\n", current_scale_factor);
    } else {
        printf(" -> Calibration file not found. Please calibrate.\n");
        hx711_set_scale(&scale, 1.0);
    }
    file_ptr = fopen(TARE_FILE, "r");
    if (file_ptr != NULL) {
        fscanf(file_ptr, "%ld", &tare_offset);
        fclose(file_ptr);
        hx711_set_offset(&scale, tare_offset);
        printf(" -> Tare offset loaded: %ld\n", tare_offset);
    } else {
        printf(" -> Tare file not found. Performing initial tare...\n");
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

        // Check for tare button press (GPIO)
        if (gpiod_line_get_value(tare_line) == 1) {
            perform_tare(&scale, TARE_FILE);
            action_taken = true;
            // Debounce: wait here until the button is released
            while (gpiod_line_get_value(tare_line) == 1) {
                my_delay_ms(50);
            }
        }

        // Check for keyboard commands
        char c = -1;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            action_taken = true; // Any key press will trigger a screen refresh
            if (c == 't') {
                perform_tare(&scale, TARE_FILE);
            }
            else if (c == 'c') {
                // Temporarily make stdin blocking for user input
                fcntl(STDIN_FILENO, F_SETFL, original_flags);

                printf("\n--- Calibration --- \n");
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_send_string("Calibration Mode");

                printf("Enter the known weight in grams (e.g., 100.0): ");
                float known_weight_g;
                scanf("%f", &known_weight_g);
                // Clear the rest of the input buffer (the Enter key)
                while(getchar() != '\n' && getchar() != EOF);

                printf("Place the %.2fg weight on the scale and press Enter.", known_weight_g);
                lcd_set_cursor(1, 0);
                lcd_send_string("Place weight...");
                getchar(); // Wait for user to press Enter

                printf("Measuring... please wait.\n");
                lcd_set_cursor(1, 0);
                lcd_send_string("Measuring...      ");

                long raw_reading = hx711_read_average(&scale, 20);
                tare_offset = hx711_get_offset(&scale);

                if (known_weight_g != 0) {
                    current_scale_factor = (float)(raw_reading - tare_offset) / known_weight_g;
                    hx711_set_scale(&scale, current_scale_factor);

                    file_ptr = fopen(CALIBRATION_FILE, "w");
                    if (file_ptr != NULL) {
                        fprintf(file_ptr, "%.4f", current_scale_factor);
                        fclose(file_ptr);
                    }
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

                // Restore non-blocking input for the main loop
                fcntl(STDIN_FILENO, F_SETFL, original_flags | O_NONBLOCK);
            }
        }

        // If an action was taken (tare or calibrate), restore the main display
        if (action_taken) {
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("Weight:");
        }

        // --- Continuously update weight reading on console and LCD ---
        float weight = hx711_get_units(&scale, 5);
        if (fabsf(weight) < 0.5) weight = 0.0;

        printf("Weight: %+.2f g            \r", weight);
        fflush(stdout);

        char lcd_buffer[17];
        snprintf(lcd_buffer, sizeof(lcd_buffer), "%8.2f g", weight);
        lcd_set_cursor(1, 0);
        lcd_send_string("                "); // Clear the line first
        lcd_set_cursor(1, 0);
        lcd_send_string(lcd_buffer);

        my_delay_ms(250);
    }

    // --- Cleanup ---
    // This part is unreachable in an infinite loop, but good practice to have
    printf("\nCleaning up and exiting.\n");
    gpiod_line_release(dout_line);
    gpiod_line_release(sck_line);
    gpiod_line_release(tare_line); // Release the new line
    gpiod_chip_close(chip);
    lcd_clear();
    lcd_set_cursor(0,0);
    lcd_send_string("Goodbye!");
    lcd_close();

    return 0;
}

