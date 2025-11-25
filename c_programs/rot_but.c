#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

// --- Configuration ---
const char *chipname = "gpiochip2";
unsigned int sw_line_offset = 1;   // SW Pin (GPIO2_A1 on your board)

// Global variable to handle Ctrl+C
volatile sig_atomic_t keep_running = 1;

// Signal handler to catch Ctrl+C
void sig_handler(int sig) {
    keep_running = 0;
}

int main(int argc, char **argv) {
    struct gpiod_chip *chip;
    struct gpiod_line *sw_line;
    struct gpiod_line_event event;

    // For non-blocking button debounce
    struct timespec last_press_time = {0, 0};
    const long debounce_press_ns = 200000000; // 200 milliseconds

    // Register the signal handler
    signal(SIGINT, sig_handler);

    // Open the GPIO chip
    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip");
        return 1;
    }

    // Get the specific line for the switch
    sw_line = gpiod_chip_get_line(chip, sw_line_offset);
    if (!sw_line) {
        perror("Error getting SW line");
        gpiod_chip_close(chip);
        return 1;
    }

    // Configure the SW line as an input, waiting for a falling edge,
    // and enable the internal pull-up resistor to prevent a floating state.
    struct gpiod_line_request_config sw_config = {
        .consumer = "rotary_switch",
        .request_type = GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE,
        .flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP
    };
    if (gpiod_line_request(sw_line, &sw_config, 0) < 0) {
        perror("Error requesting SW line");
        gpiod_chip_close(chip);
        return 1;
    }
    
    printf("Switch ready. Press the button or press Ctrl+C to exit.\n");

    // Main loop
    while (keep_running) {
        // Wait for a button press event. The 'NULL' means wait indefinitely.
        int ret = gpiod_line_event_wait(sw_line, NULL);

        if (ret < 0) { // An error occurred
            perror("Error waiting for event");
            break;
        } else if (ret == 0) { // Timeout occurred (shouldn't happen with NULL)
            continue;
        }

        // An event happened, so read it
        if (gpiod_line_event_read(sw_line, &event) == 0) {
            // Check debounce timer
            struct timespec current_time;
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            long time_diff_ns = (current_time.tv_sec - last_press_time.tv_sec) * 1000000000 + (current_time.tv_nsec - last_press_time.tv_nsec);

            if (time_diff_ns > debounce_press_ns) {
                printf("Button Pressed!\n");
                last_press_time = current_time; // Update the time of the last valid press
            }
        }
    }

    // Cleanup and exit gracefully
    printf("\nCleaning up and exiting.\n");
    gpiod_line_release(sw_line);
    gpiod_chip_close(chip);

    return 0;
}
