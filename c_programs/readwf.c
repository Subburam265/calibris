// File: hx711_calibrated.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <termios.h>
#include <string.h>
#include <sys/select.h>

// --- Configuration ---
#define CHIP_NAME "gpiochip2"
#define DOUT_PIN  5 // Correct line for GPIO2_A5_d (global 69)
#define SCK_PIN   4 // Correct line for GPIO2_A4_d (global 68)
#define CALIBRATION_FILE "/home/pico/hx711_calibration.dat" // Change path if needed

// --- Global Variables ---
struct gpiod_chip *chip;
struct gpiod_line *dout_line;
struct gpiod_line *sck_line;
long TARE_OFFSET = 0;
float SCALE_FACTOR = 430.0; // A reasonable default

// Function to read a raw value
long hx711_read() {
    long count = 0;
    // Simple timeout to prevent infinite loop
    int timeout_counter = 0;
    
    // Wait for the chip to be ready (DOUT goes low)
    while (gpiod_line_get_value(dout_line) == 1) {
        usleep(100);
        timeout_counter++;
        if (timeout_counter > 5000) { // 500ms timeout
            return -1;
        }
    }

    for (int i = 0; i < 24; i++) {
        gpiod_line_set_value(sck_line, 1);
        usleep(1);
        count = count << 1;
        gpiod_line_set_value(sck_line, 0);
        usleep(1);
        if (gpiod_line_get_value(dout_line)) {
            count++;
        }
    }

    gpiod_line_set_value(sck_line, 1);
    usleep(1);
    gpiod_line_set_value(sck_line, 0);

    if (count & 0x800000) {
        count |= ~0xFFFFFF;
    }
    return count;
}

// Get an average of several readings, filtering out errors
long get_averaged_reading(int samples) {
    long total = 0;
    int valid_count = 0;
    for (int i = 0; i < samples; i++) {
        long val = hx711_read();
        if (val != -1) {
            total += val;
            valid_count++;
        }
        usleep(10000); // 10ms
    }
    if (valid_count > 0) {
        return total / valid_count;
    }
    return -1;
}

// --- Calibration Functions ---
void save_calibration() {
    FILE *f = fopen(CALIBRATION_FILE, "w");
    if (f) {
        fprintf(f, "%ld\n%f\n", TARE_OFFSET, SCALE_FACTOR);
        fclose(f);
        printf("Calibration saved.\n");
    } else {
        perror("Error saving calibration file");
    }
}

void load_calibration() {
    FILE *f = fopen(CALIBRATION_FILE, "r");
    if (f) {
        fscanf(f, "%ld\n%f", &TARE_OFFSET, &SCALE_FACTOR);
        fclose(f);
        printf("Calibration loaded.\n");
    } else {
        printf("No calibration file found. Using default values.\n");
    }
}

// --- Terminal and Input Functions ---
struct termios old_tio, new_tio;

void init_terminal() {
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
}

int kbhit() {
    struct timeval tv = {0L, 0L};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return select(1, &fds, NULL, NULL, &tv) > 0;
}

// --- Main Program ---
int main() {
    init_terminal();
    
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    dout_line = gpiod_chip_get_line(chip, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip, SCK_PIN);
    gpiod_line_request_input(dout_line, "hx711");
    gpiod_line_request_output(sck_line, "hx711", 0);

    load_calibration();

    printf("\nReading weight. Press 't' to tare, 'c' to calibrate, 'q' to quit.\n");
    
    int running = 1;
    while (running) {
        long raw_val = get_averaged_reading(5);
        
        if (raw_val != -1) {
            float weight_g = (raw_val - TARE_OFFSET) / SCALE_FACTOR;
            printf("\rWeight: %.2f g          ", weight_g);
        } else {
            printf("\rError: Reading failed. Check wiring. ");
        }
        fflush(stdout);

        if (kbhit()) {
            char cmd = getchar();
            printf("\n");
            switch(cmd) {
                case 't':
                case 'T':
                    printf("Taring... please wait.\n");
                    TARE_OFFSET = get_averaged_reading(15);
                    printf("Tare complete. New offset: %ld\n", TARE_OFFSET);
                    save_calibration();
                    break;
                case 'c':
                case 'C':
                    printf("Calibration:\n1. Remove all weight and press Enter.");
                    getchar(); // Wait for Enter
                    TARE_OFFSET = get_averaged_reading(15);
                    printf("Tare complete. Offset: %ld\n", TARE_OFFSET);
                    
                    printf("2. Place a known weight on the scale.\n3. Enter the weight in grams: ");
                    float known_weight;
                    restore_terminal(); // Temporarily restore terminal for clean input
                    scanf("%f", &known_weight);
                    getchar(); // Consume newline
                    init_terminal(); // Re-enable non-canonical mode
                    
                    long cal_reading = get_averaged_reading(15);
                    if (known_weight > 0) {
                        SCALE_FACTOR = (cal_reading - TARE_OFFSET) / known_weight;
                        printf("New scale factor: %f\n", SCALE_FACTOR);
                        save_calibration();
                    } else {
                        printf("Invalid weight. Calibration cancelled.\n");
                    }
                    break;
                case 'q':
                case 'Q':
                    running = 0;
                    break;
            }
             printf("\nReading weight. Press 't' to tare, 'c' to calibrate, 'q' to quit.\n");
        }

        usleep(100000); // 100ms main loop delay
    }

    // Cleanup
    restore_terminal();
    printf("\nReleasing GPIOs and exiting.\n");
    gpiod_line_release(dout_line);
    gpiod_line_release(sck_line);
    gpiod_chip_close(chip);
    return 0;
}
