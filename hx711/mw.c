#include <stdio.h>
#include <unistd.h>
#include <gpiod.h>
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
    
    // !!! IMPORTANT: REPLACE THIS WITH THE VALUE FROM THE CALIBRATION PROGRAM !!!
    const float YOUR_SCALE_FACTOR = 430.0; // Example value, use your own!

    // --- Initialize GPIO ---
    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip");
        return 1;
    }
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

    // --- Set the correct scale and tare ---
    hx711_set_scale(&scale, YOUR_SCALE_FACTOR);
    
    printf("GPIO and scale initialized.\n");
    printf("Taring the scale... do not touch it.\n");
    hx711_tare(&scale, 20);
    printf("Tare complete. Ready for measurements.\n\n");

    // --- Measurement Loop ---
    while (1) {
        float weight = hx711_get_units(&scale, 5);
        printf("Weight: %.2f g\n", weight);
        my_delay_ms(500);
    }

    // --- Cleanup GPIO ---
    gpiod_line_release(dout_line);
    gpiod_line_release(sck_line);
    gpiod_chip_close(chip);

    return 0;
}
