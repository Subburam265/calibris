#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#define GPIO_DOUT 69
#define GPIO_SCK  68

// --- PASTE YOUR CALIBRATION VALUES HERE ---
#define OFFSET -18500
#define SCALE  1020.0
// -----------------------------------------

// GPIO Utility Functions (no changes needed)
int gpio_export(int pin) { /* ... same as before ... */ }
int gpio_set_dir(int pin, const char *dir) { /* ... same as before ... */ }
int gpio_set_value(int pin, int value) { /* ... same as before ... */ }
int gpio_get_value(int pin) { /* ... same as before ... */ }

// HX711 Read Function (no changes needed)
int is_ready() { return gpio_get_value(GPIO_DOUT) == 0; }
long hx711_read() { /* ... same as before ... */ }

// --- MAIN PROGRAM ---
int main() {
    printf("Setting up GPIO...\n");
    gpio_export(GPIO_DOUT);
    gpio_export(GPIO_SCK);
    gpio_set_dir(GPIO_DOUT, "in");
    gpio_set_dir(GPIO_SCK, "out");
    printf("Setup complete. Place items on the scale.\n");

    while (1) {
        long raw_data = hx711_read();
        
        // Calculate the final weight in grams
        float weight_g = (raw_data - OFFSET) / SCALE;

        printf("Weight: %.2f g\n", weight_g);
        usleep(500000); // Sleep for 500ms
    }

    return 0;
}
