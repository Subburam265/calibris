#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

// --- Configuration ---
#define CHIP_NAME "gpiochip1"
#define TRIGGER_PIN 21  // GPIO1_C5 = 16 + 5 = 21
#define CONSUMER "calibris_trigger"

#define CONFIG_FILE "/home/pico/calibris/data/config.json"
#define NORMAL_SERVICE "measure_weight.service"
#define SAFE_SERVICE "safe_mode.service"

// --- Function to Activate Safe Mode ---
void activate_safe_mode() {
    printf("[Trigger] GPIO1_C5 went LOW! Activating Safe Mode...\n");

    // 1. Update config.json to "safe_mode": true
    // We use sed for atomic file editing safety
    int ret = system("sed -i 's/\"safe_mode\"[[:space:]]*:[[:space:]]*false/\"safe_mode\": true/' " CONFIG_FILE);
    if (ret == 0) {
        printf("[Trigger] Config updated.\n");
    } else {
        fprintf(stderr, "[Trigger] Failed to update config.json\n");
    }

    // 2. Switch Services
    // Stop the normal scale operation
    printf("[Trigger] Stopping %s...\n", NORMAL_SERVICE);
    system("systemctl stop " NORMAL_SERVICE);

    // Disable normal service so it doesn't auto-start on reboot if currently broken
    system("systemctl disable " NORMAL_SERVICE);

    // Start and Enable the safe mode service
    printf("[Trigger] Starting %s...\n", SAFE_SERVICE);
    system("systemctl enable --now " SAFE_SERVICE);

    printf("[Trigger] Safe Mode Activation Complete.\n");
}

int main() {
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int ret;

    printf("Starting Calibris Safe Mode Trigger Service...\n");
    printf("Monitoring %s Line %d (GPIO1_C5)\n", CHIP_NAME, TRIGGER_PIN);

    // 1. Open GPIO Chip
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (!chip) {
        perror("Open chip failed");
        return 1;
    }

    // 2. Get the Line
    line = gpiod_chip_get_line(chip, TRIGGER_PIN);
    if (!line) {
        perror("Get line failed");
        gpiod_chip_close(chip);
        return 1;
    }

    // 3. Request Falling Edge Events (Triggers when pin goes High -> Low)
    // We request INPUT with a FALLING_EDGE event listener
    struct gpiod_line_request_config config = {
        .consumer = CONSUMER,
        .request_type = GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE,
        .flags = 0 // You might need GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP if you don't have an external resistor
    };

    ret = gpiod_line_request(line, &config, 0);
    if (ret < 0) {
        perror("Request line event failed");
        gpiod_chip_close(chip);
        return 1;
    }

    // 4. Main Event Loop
    while (1) {
        // Wait for an event (timeout -1 means wait forever)
        // This puts the process to sleep until the pin changes state
        ret = gpiod_line_event_wait(line, NULL);

        if (ret < 0) {
            perror("Wait event failed");
            break;
        } else if (ret > 0) {
            // Event occurred, read it to clear the buffer
            struct gpiod_line_event event;
            ret = gpiod_line_event_read(line, &event);
            if (ret < 0) {
                perror("Read event failed");
                continue;
            }

            // Double check it was a falling edge
            if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
                // Debounce: Wait 50ms and check if still low
                usleep(50000);
                if (gpiod_line_get_value(line) == 0) {
                    activate_safe_mode();
                    
                    // Optional: Sleep a bit so we don't spam commands if the button is held
                    sleep(2); 
                }
            }
        }
    }

    gpiod_line_release(line);
    gpiod_chip_close(chip);
    return 0;
}
