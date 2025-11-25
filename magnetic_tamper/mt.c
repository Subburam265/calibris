#include <stdio.h>
#include <unistd.h>
#include <gpiod.h>
#include <stdbool.h> // Required for bool type

/**
 * @brief A program to detect a high signal on a GPIO pin.
 *
 * This program continuously monitors a specific GPIO pin on a Luckfox board.
 * When the pin state changes from LOW to HIGH (like a reed switch closing),
 * it prints a "Magnetic tamper detected!" message. It also prints a message
 * when the condition is cleared.
 */
int main(int argc, char **argv)
{
    // --- Configuration ---
    // According to the Rockchip naming scheme:
    // GPIO1_B2_d corresponds to chip "gpiochip1"
    // Bank B is the 2nd bank (index 1), Pin 2 is index 2.
    // Offset = (bank_index * 8) + pin_index = (1 * 8) + 2 = 10
    const char *chipname = "gpiochip1";
    const unsigned int line_offset = 23; // GPIO1_B2_d

    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int ret;

    // --- GPIO Initialization ---
    // Open the GPIO chip
    chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip");
        return 1;
    }

    // Get the GPIO line
    line = gpiod_chip_get_line(chip, line_offset);
    if (!line) {
        perror("Error getting GPIO line");
        gpiod_chip_close(chip);
        return 1;
    }

    // Request the line as input. "tamper_detect" is a consumer name.
    ret = gpiod_line_request_input(line, "tamper_detect");
    if (ret < 0) {
        perror("Error requesting GPIO line as input");
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Monitoring GPIO pin %s:%u for tamper detection...\n", chipname, line_offset);
    printf("Press Ctrl+C to exit.\n");

    bool tampered_state = false; // Tracks the current state to avoid repeat messages

    // --- Monitoring Loop ---
    while (1) {
        // Read the value of the GPIO pin
        int current_value = gpiod_line_get_value(line);
        if (current_value < 0) {
            perror("Error reading GPIO line value");
            break; // Exit loop on error
        }

        // Check for a rising edge (state change from LOW to HIGH)
        if (current_value == 1 && !tampered_state) {
            tampered_state = true;
            printf("Magnetic tamper detected! (Pin HIGH)\n");
            // Ensure the message is printed immediately
            fflush(stdout);
        }
        // Check for a falling edge (state change from HIGH to LOW)
        else if (current_value == 0 && tampered_state) {
            tampered_state = false;
            printf("Tamper condition cleared. (Pin LOW)\n");
            fflush(stdout);
        }

        // Wait for a short period to avoid busy-waiting and high CPU usage
        usleep(100000); // 100 milliseconds
    }

    // --- Cleanup ---
    // This code is reached if the loop breaks (e.g., on error or Ctrl+C)
    printf("\nCleaning up and exiting.\n");
    gpiod_line_release(line);
    gpiod_chip_close(chip);

    return 0;
}
