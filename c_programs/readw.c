// File: hx711_gpiod.c
#include <stdio.h>
#include <unistd.h>
#include <gpiod.h>

// IMPORTANT: Use the pin numbers you confirmed with 'gpioinfo'
#define CHIP_NAME "gpiochip2"
#define DOUT_PIN  5 // Correct line for GPIO2_A5_d (global 69)
#define SCK_PIN   4 // Correct line for GPIO2_A4_d (global 68)

struct gpiod_chip *chip;
struct gpiod_line *dout_line;
struct gpiod_line *sck_line;

// Function to read a raw value
long hx711_read() {
    long count = 0;
    
    // Wait for the chip to be ready (DOUT goes low)
    while (gpiod_line_get_value(dout_line) == 1) {
        usleep(1000); // Wait 1ms
    }

    // Read the 24 bits
    for (int i = 0; i < 24; i++) {
        gpiod_line_set_value(sck_line, 1);
        usleep(1); // Small delay
        count = count << 1;
        gpiod_line_set_value(sck_line, 0);
        usleep(1); // Small delay
        if (gpiod_line_get_value(dout_line)) {
            count++;
        }
    }

    // Set gain for next reading (1 pulse for 128x gain)
    gpiod_line_set_value(sck_line, 1);
    usleep(1);
    gpiod_line_set_value(sck_line, 0);

    // Handle 2's complement
    if (count & 0x800000) {
        count |= ~0xFFFFFF;
    }

    return count;
}

int main() {
    // --- YOUR CALIBRATION VALUES ---
    long TARE_OFFSET = 33115;    // Replace with your actual offset value
    float SCALE_FACTOR = 425.0;  // Replace with your actual scale value
    // --------------------------------

    // Open GPIO chip and get lines (include your error handling)
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    dout_line = gpiod_chip_get_line(chip, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip, SCK_PIN);
    gpiod_line_request_input(dout_line, "hx711");
    gpiod_line_request_output(sck_line, "hx711", 0);

    printf("Reading weight. Press Ctrl+C to exit.\n");
    
    while (1) {
        int num_readings = 5;
        long readings[num_readings];
        int valid_readings_count = 0;
        long total = 0;

        // Take a burst of readings
        for (int i = 0; i < num_readings; i++) {
            long val = hx711_read();
            if (val != -1) { // <-- Check for the error
                readings[valid_readings_count] = val;
                valid_readings_count++;
            }
            usleep(10000); // 10ms delay between small reads
        }

        // If we got at least one good reading, average them
        if (valid_readings_count > 0) {
            for (int i = 0; i < valid_readings_count; i++) {
                total += readings[i];
            }
            long avg_raw = total / valid_readings_count;
            float weight_g = (avg_raw - TARE_OFFSET) / SCALE_FACTOR;
            printf("\rWeight: %.2f g     ", weight_g);
        } else {
            // Only print an error if all readings failed
            printf("\rError: Could not get a stable reading.");
        }
        
        fflush(stdout);
        usleep(100000); // 100ms delay for the main loop
    }

    // Release lines and close chip
    gpiod_line_release(dout_line);
    gpiod_line_release(sck_line);
    gpiod_chip_close(chip);

    return 0;
}
