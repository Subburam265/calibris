#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h> // For catching Ctrl+C
#include <time.h>   // For non-blocking debounce

// --- Configuration ---
const char *chipname = "gpiochip2";
unsigned int clk_line_offset = 3;  // CLK Pin (GPIO2_A3)
unsigned int dt_line_offset = 2;   // DT Pin  (GPIO2_A2)
unsigned int sw_line_offset = 1;   // SW Pin  (GPIO2_A1)

// --- Global variable for signal handling ---
volatile sig_atomic_t keep_running = 1;

// --- Signal handler function ---
void sig_handler(int sig) {
    keep_running = 0;
}

int main(int argc, char **argv) {
    struct gpiod_chip *chip;
    struct gpiod_line *clk_line, *dt_line, *sw_line;
    struct gpiod_line_bulk lines;
    struct gpiod_line_event event;
    long counter = 0;

    // For non-blocking button debounce
    struct timespec last_press_time = {0, 0};
    const long debounce_ns = 200000000; // 200 milliseconds

    // Register signal handler for SIGINT (Ctrl+C)
    signal(SIGINT, sig_handler);

    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip");
        return 1;
    }

    clk_line = gpiod_chip_get_line(chip, clk_line_offset);
    dt_line = gpiod_chip_get_line(chip, dt_line_offset);
    sw_line = gpiod_chip_get_line(chip, sw_line_offset);
    if (!clk_line || !dt_line || !sw_line) {
        perror("Error getting GPIO lines");
        gpiod_chip_close(chip);
        return 1;
    }

    // --- Request lines with event handling ---
    // We only need to watch for one edge on the CLK pin to detect a "step"
    if (gpiod_line_request_rising_edge_events(clk_line, "rotary_encoder") < 0) {
        perror("Error requesting CLK line events");
        gpiod_chip_close(chip);
        return 1;
    }
    // The button is active-low, so we watch for the falling edge
    if (gpiod_line_request_falling_edge_events(sw_line, "rotary_encoder") < 0) {
        perror("Error requesting SW line events");
        gpiod_line_release(clk_line);
        gpiod_chip_close(chip);
        return 1;
    }
    // The DT line is just for reading the state, so no events needed
    if (gpiod_line_request_input(dt_line, "rotary_encoder") < 0) {
        perror("Error requesting DT line as input");
        gpiod_line_release(clk_line);
        gpiod_line_release(sw_line);
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Rotary encoder ready. Press Ctrl+C to exit.\n");

    // Group the event lines into a bulk object for waiting
    gpiod_line_bulk_init(&lines);
    gpiod_line_bulk_add(&lines, clk_line);
    gpiod_line_bulk_add(&lines, sw_line);

    while (keep_running) {
        // Wait indefinitely for an event on either the CLK or SW line
        int ret = gpiod_line_event_wait_bulk(&lines, NULL, &lines);
        if (ret <= 0) { // 0 is timeout, < 0 is error
            if (ret < 0) perror("Error waiting for event");
            continue;
        }

        // Check which line had the event and read it
        if (gpiod_line_event_read(clk_line, &event) == 0) {
            // A CLK rising edge occurred, now check the DT line to determine direction
            if (gpiod_line_get_value(dt_line) == 0) {
                counter--;
                printf("Direction: Counter-Clockwise, Counter: %ld\n", counter);
            } else {
                counter++;
                printf("Direction: Clockwise, Counter: %ld\n", counter);
            }
        }

        if (gpiod_line_event_read(sw_line, &event) == 0) {
            // A SW falling edge occurred, now check for debounce
            struct timespec current_time;
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            long time_diff_ns = (current_time.tv_sec - last_press_time.tv_sec) * 1000000000 + (current_time.tv_nsec - last_press_time.tv_nsec);

            if (time_diff_ns > debounce_ns) {
                printf("Button Pressed!\n");
                last_press_time = current_time; // Update the time of the last valid press
            }
        }
    }

    printf("\nCleaning up and exiting.\n");
    gpiod_line_release(clk_line);
    gpiod_line_release(dt_line);
    gpiod_line_release(sw_line);
    gpiod_chip_close(chip);

    return 0;
}
