#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <math.h>
#include <fcntl.h> 
#include "hx711.h"

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
    const char* chipname = "gpiochip2";
    const int DOUT_PIN = 5;
    const int SCK_PIN = 4;
    const char* CALIBRATION_FILE = "calibration.txt";
    const char* TARE_FILE = "tare.txt";
    
    float current_scale_factor;
    long tare_offset;
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
    
    // --- Initialize HX711 ---
    hx711_t scale;
    hx711_init(&scale, DOUT_PIN, SCK_PIN,
               my_gpio_write, my_gpio_read,
               my_delay_us, my_delay_ms);

    // --- Load settings from files ---
    printf("Loading settings...\n");

    // Load calibration factor
    file_ptr = fopen(CALIBRATION_FILE, "r");
    if (file_ptr != NULL) {
        fscanf(file_ptr, "%f", &current_scale_factor);
        fclose(file_ptr);
        hx711_set_scale(&scale, current_scale_factor);
        printf(" -> Calibration factor loaded: %.2f\n", current_scale_factor);
    } else {
        printf(" -> Calibration file not found. Please calibrate using 'c'.\n");
        hx711_set_scale(&scale, 1.0); // Default to 1.0
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
        hx711_tare(&scale, 20);
        tare_offset = hx711_get_offset(&scale);
        file_ptr = fopen(TARE_FILE, "w");
        fprintf(file_ptr, "%ld", tare_offset);
        fclose(file_ptr);
        printf(" -> Tare complete. New offset %ld saved.\n", tare_offset);
    }

    printf("\nReady for measurements.\n");
    printf(">>> Press 't' to re-tare, or 'c' to calibrate. Then press Enter. <<<\n\n");

    // --- Measurement Loop ---
    while (1) {
        char c = getchar();
        if (c == 't' || c == 'c') {
            fcntl(STDIN_FILENO, F_SETFL, flags); // Revert to blocking input
            
            if (c == 't') {
                printf("\n>>> Re-Taring... do not touch the scale. Press Enter when ready. <<<\n");
                while(getchar() != '\n' && getchar() != EOF);
                hx711_tare(&scale, 20);
                tare_offset = hx711_get_offset(&scale);
                file_ptr = fopen(TARE_FILE, "w");
                fprintf(file_ptr, "%ld", tare_offset);
                fclose(file_ptr);
                printf(">>> Tare complete. New offset %ld saved. <<<\n\n", tare_offset);
            } else if (c == 'c') {
                printf("\n--- Calibration --- \n");
                printf("Enter the known weight in grams (e.g., 100.0): ");
                float known_weight_g;
                scanf("%f", &known_weight_g);
                while(getchar() != '\n' && getchar() != EOF);
                
                printf("Place the %.2fg weight on the scale and press Enter.", known_weight_g);
                getchar();

                printf("Measuring... please wait.\n");
                long raw_reading = hx711_read_average(&scale, 20);
                tare_offset = hx711_get_offset(&scale);

                current_scale_factor = (float)(raw_reading - tare_offset) / known_weight_g;
                hx711_set_scale(&scale, current_scale_factor);

                file_ptr = fopen(CALIBRATION_FILE, "w");
                fprintf(file_ptr, "%.4f", current_scale_factor); // Save with more precision
                fclose(file_ptr);

                printf("\n--- Calibration Complete! ---\n");
                printf("New scale factor is: %.2f\n", current_scale_factor);
                printf("This value has been saved to %s\n\n", CALIBRATION_FILE);
            }
            
            fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK); // Back to non-blocking
        }

        float weight = hx711_get_units(&scale, 5);
        if (fabsf(weight) < 0.5) weight = 0.0;
        
        printf("Weight: %+.2f g          \r", weight);
        fflush(stdout); 
        
        my_delay_ms(200);
    }

    // --- Cleanup GPIO ---
    gpiod_line_release(dout_line);
    gpiod_line_release(sck_line);
    gpiod_chip_close(chip);

    return 0;
}
