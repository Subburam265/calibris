#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>

// --- Configuration ---
// Corresponds to Pin #34 (GPIO2_A7) from the diagram
const char *CHIP_NAME = "gpiochip2";
const int GPIO_PIN = 7; // Line offset for GPIO2_A7 is 7

int main() {
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int value;
    int last_value = -1; // Used to print only on change

    // Open the GPIO chip
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (!chip) {
        perror("gpiod_chip_open_by_name");
        return 1;
    }

    // Get the GPIO line
    line = gpiod_chip_get_line(chip, GPIO_PIN);
    if (!line) {
        perror("gpiod_chip_get_line");
        gpiod_chip_close(chip);
        return 1;
    }

    // Request the line as an input. No internal pull-up/down needed
    // because we have a physical pull-down resistor in our circuit.
    if (gpiod_line_request_input(line, "reed-switch-reader") < 0) {
        perror("gpiod_line_request_input");
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Reading reed switch state. Press Ctrl+C to exit.\n");

    while (1) {
        value = gpiod_line_get_value(line);

        if (value != last_value) {
            if (value == 0) {
                printf("\rMagnet Present (Pin is HIGH) ");
            } else {
                printf("\rMagnet Absent  (Pin is LOW)  ");
            }
            fflush(stdout); // Make sure the text appears immediately
            last_value = value;
        }

        // Wait for 100 milliseconds before checking again
        usleep(100000);
    }

    // Cleanup (though we'll never reach here in this simple loop)
    gpiod_line_release(line);
    gpiod_chip_close(chip);

    return 0;
}
