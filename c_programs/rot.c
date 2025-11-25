#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

// --- Configuration ---
// NOTE: You must identify the correct chip and line numbers for your board.
// Use `gpiodetect` in the terminal to list available chips.
const char *chipname = "gpiochip2";

// These are the GPIO line offsets on the chip (e.g., GPIO1_C0 -> line 72)
// You WILL need to verify and change these for your specific setup.
unsigned int clk_line_offset = 3; // CLK Pin
unsigned int dt_line_offset = 2;  // DT Pin
unsigned int sw_line_offset = 1;  // SW Pin

int main(int argc, char **argv) {
    struct gpiod_chip *chip;
    struct gpiod_line *clk_line, *dt_line, *sw_line;
    int clk_val, dt_val, sw_val;
    int clk_last_state;
    long counter = 0;

    // Open the GPIO chip
    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip");
        return 1;
    }

    // Get the GPIO lines
    clk_line = gpiod_chip_get_line(chip, clk_line_offset);
    dt_line = gpiod_chip_get_line(chip, dt_line_offset);
    sw_line = gpiod_chip_get_line(chip, sw_line_offset);

    if (!clk_line || !dt_line || !sw_line) {
        perror("Error getting GPIO lines");
        gpiod_chip_close(chip);
        return 1;
    }

    // Request lines as input. "consumer" is a name for your app.
    if (gpiod_line_request_input(clk_line, "rotary_encoder") < 0 ||
        gpiod_line_request_input(dt_line, "rotary_encoder") < 0 ||
        gpiod_line_request_input(sw_line, "rotary_encoder") < 0) {
        perror("Error requesting GPIO lines");
        gpiod_line_release(clk_line);
        gpiod_line_release(dt_line);
        gpiod_line_release(sw_line);
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Rotary encoder ready. Press Ctrl+C to exit.\n");

    // Read the initial state of CLK
    clk_last_state = gpiod_line_get_value(clk_line);

    while (1) {
        clk_val = gpiod_line_get_value(clk_line);
        dt_val = gpiod_line_get_value(dt_line);
        sw_val = gpiod_line_get_value(sw_line);

        if (clk_val != clk_last_state) { // A change on CLK pin has occurred
            if (dt_val != clk_val) {
                counter++;
                printf("Direction: Clockwise, Counter: %ld\n", counter);
            } else {
                counter--;
                printf("Direction: Counter-Clockwise, Counter: %ld\n", counter);
            }
        }
        clk_last_state = clk_val;

        if (sw_val == 0) { // Button is pressed (active low)
            printf("Button Pressed!\n");
            usleep(200000); // Debounce delay of 200ms
        }

        usleep(1000); // Small delay to prevent high CPU usage
    }

    // --- Cleanup (won't be reached in this infinite loop) ---
    // In a real application, you would handle signals (like Ctrl+C)
    // to call this cleanup code.
    gpiod_line_release(clk_line);
    gpiod_line_release(dt_line);
    gpiod_line_release(sw_line);
    gpiod_chip_close(chip);

    return 0;
}
