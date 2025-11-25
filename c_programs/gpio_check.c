#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>

#define CHIP_NAME "gpiochip2"  // Bank 2 for GPIO2_A4_d
#define LINE_NUM 4             // "A" group starts at 0, "A4" = 4
#define CONSUMER "gpio2_a4_input_check"

int main()
{
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int value;

    // Open GPIO chip for bank 1
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (!chip) {
        perror("Open chip failed");
        return EXIT_FAILURE;
    }

    // Get the specific GPIO line
    line = gpiod_chip_get_line(chip, LINE_NUM);
    if (!line) {
        perror("Get line failed");
        gpiod_chip_close(chip);
        return EXIT_FAILURE;
    }

    // Request the line as input
    if (gpiod_line_request_input(line, CONSUMER) < 0) {
        perror("Request line as input failed");
        gpiod_chip_close(chip);
        return EXIT_FAILURE;
    }

    // Read the value of the line
    value = gpiod_line_get_value(line);
    if (value < 0) {
        perror("Read line failed");
    } else {
        printf("GPIO1_B2_d (chip1, line10) value: %d\n", value);
    }

    // Release resources
    gpiod_line_release(line);
    gpiod_chip_close(chip);

    return EXIT_SUCCESS;
}
