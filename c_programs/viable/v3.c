#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <termios.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>
#include <math.h>

// --- Configuration ---
#define CHIP_NAME "gpiochip2"
#define DOUT_PIN  5
#define SCK_PIN   4
#define CALIBRATION_FILE "/home/pico/hx711_calibration.dat"

// --- Logging Logic Configuration ---
#define WEIGHT_THRESHOLD 10.0      // (grams) Ignore weights below this value.
#define STABLE_DURATION_SEC 3.0    // (seconds) Weight must be stable for this long for all events.
#define REWEIGH_THRESHOLD 15.0     // (grams) A new weight must be added that is > this value to trigger re-weighing.
#define CALIBRATION_SAMPLES 20     // Number of samples to take during calibration

// --- Global Variables ---
struct gpiod_chip *chip;
struct gpiod_line *dout_line;
struct gpiod_line *sck_line;
long TARE_OFFSET = 0;
float SCALE_FACTOR = 430.0; // A reasonable starting default
volatile sig_atomic_t running = 1;

// --- Signal Handler for Ctrl+C ---
void cleanup_and_exit(int signum) {
    running = 0;
}

// --- Core HX711 Functions ---
long hx711_read() {
    long count = 0;
    int timeout_counter = 0;
    while (gpiod_line_get_value(dout_line) == 1) {
        usleep(100);
        timeout_counter++;
        if (timeout_counter > 5000) return -1;
    }
    for (int i = 0; i < 24; i++) {
        gpiod_line_set_value(sck_line, 1); usleep(1);
        count = count << 1;
        gpiod_line_set_value(sck_line, 0); usleep(1);
        if (gpiod_line_get_value(dout_line)) count++;
    }
    gpiod_line_set_value(sck_line, 1); usleep(1);
    gpiod_line_set_value(sck_line, 0);
    if (count & 0x800000) {
        count |= ~0xFFFFFF;
    }
    return count;
}

long get_averaged_reading(int samples) {
    long total = 0;
    int valid_count = 0;
    for (int i = 0; i < samples; i++) {
        long val = hx711_read();
        if (val != -1) {
            total += val;
            valid_count++;
        }
        usleep(10000);
    }
    return (valid_count > 0) ? (total / valid_count) : -1;
}

// --- Calibration File Functions ---
void save_calibration() {
    FILE *f = fopen(CALIBRATION_FILE, "w");
    if (f) {
        fprintf(f, "%ld\n%f\n", TARE_OFFSET, SCALE_FACTOR);
        fclose(f);
        printf("\nCalibration saved to file.\n");
    } else {
        perror("\nError saving calibration file");
    }
}

void load_calibration() {
    FILE *f = fopen(CALIBRATION_FILE, "r");
    if (f) {
        if (fscanf(f, "%ld\n%f", &TARE_OFFSET, &SCALE_FACTOR) == 2) {
             printf("Calibration loaded from file.\n");
        } else {
             printf("Calibration file corrupt, using defaults.\n");
        }
        fclose(f);
    } else {
        printf("No calibration file found, using defaults.\n");
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

// --- Calibration Function ---
void perform_calibration() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    printf("\n\n--- CALIBRATION MODE ---\n");
    printf("1. Place the scale/platform empty and press Enter for tare...");
    getchar();
    printf("   Taring... please wait.\n");
    long tare_raw = get_averaged_reading(CALIBRATION_SAMPLES);
    if (tare_raw == -1) {
        printf("Error during taring. Check connection.\n");
        tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
        return;
    }
    printf("   Tare complete. Zero point set to: %ld\n\n", tare_raw);
    printf("2. Enter the reference weight in grams (e.g., 100.0): ");
    float reference_weight;
    if (scanf("%f", &reference_weight) != 1 || reference_weight <= 0) {
        printf("   Invalid input. Calibration cancelled.\n");
        while (getchar() != '\n');
        tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
        return;
    }
    while (getchar() != '\n');
    printf("3. Place the %.2fg weight on the scale and press Enter...", reference_weight);
    getchar();
    printf("   Measuring... please wait.\n");
    long weight_raw = get_averaged_reading(CALIBRATION_SAMPLES);
    if (weight_raw == -1) {
        printf("   Error during measurement. Check connection.\n");
        tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
        return;
    }
    float new_scale_factor = (float)(weight_raw - tare_raw) / reference_weight;
    printf("\n--- Results ---\n");
    printf("   Calibration reading: %ld\n", weight_raw);
    printf("   Calculated scale factor: %g (previous: %g)\n", new_scale_factor, SCALE_FACTOR);
    printf("\nAccept new calibration? (y/n): ");
    char choice = getchar();
    if (choice == 'y' || choice == 'Y') {
        SCALE_FACTOR = new_scale_factor;
        TARE_OFFSET = tare_raw;
        save_calibration();
        printf("Calibration completed successfully.\n");
    } else {
        printf("Calibration cancelled. No changes were made.\n");
    }
    while (getchar() != '\n');
    printf("\nPress Enter to continue...");
    getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    printf("\n--- Returning to weight monitoring ---\n");
}

// --- Main Program ---
int main() {
    enum State { STATE_IDLE, STATE_WEIGHING, STATE_LOGGED, STATE_REWEIGHING };
    enum State current_state = STATE_IDLE;
    struct timespec stability_timer_start;
    float logged_weight = 0.0;

    signal(SIGINT, cleanup_and_exit);
    init_terminal();

    chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (!chip) {
        perror("Failed to open GPIO chip");
        restore_terminal();
        return 1;
    }

    dout_line = gpiod_chip_get_line(chip, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip, SCK_PIN);
    if (!dout_line || !sck_line) {
        perror("Failed to get GPIO lines");
        gpiod_chip_close(chip);
        restore_terminal();
        return 1;
    }

    if (gpiod_line_request_input(dout_line, "hx711-logger") < 0) {
        perror("Request DOUT failed");
        gpiod_chip_close(chip);
        restore_terminal();
        return 1;
    }

    if (gpiod_line_request_output(sck_line, "hx711-logger", 0) < 0) {
        perror("Request SCK failed");
        gpiod_line_release(dout_line);
        gpiod_chip_close(chip);
        restore_terminal();
        return 1;
    }

    load_calibration();
    printf("Weight logger started.\n");
    printf("Commands: 't' to tare, 'c' to calibrate with known weight, Ctrl+C to exit.\n");

    while (running) {
        long raw_val = get_averaged_reading(3);
        if (raw_val == -1) {
            printf("\rError: Reading failed. Check wiring.      ");
            fflush(stdout);
            usleep(100000);
            continue;
        }
        float current_weight = (raw_val - TARE_OFFSET) / SCALE_FACTOR;

        switch (current_state) {
            case STATE_IDLE:
                if (current_weight > WEIGHT_THRESHOLD) {
                    current_state = STATE_WEIGHING;
                    clock_gettime(CLOCK_MONOTONIC, &stability_timer_start);
                }
                break;
            case STATE_WEIGHING:
                if (current_weight < WEIGHT_THRESHOLD) {
                    current_state = STATE_IDLE;
                } else {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    double elapsed = (now.tv_sec - stability_timer_start.tv_sec) +
                                     (now.tv_nsec - stability_timer_start.tv_nsec) / 1e9;
                    if (elapsed >= STABLE_DURATION_SEC) {
                        printf("\nLogged weight: %.2f g\n", current_weight);
                        logged_weight = current_weight;
                        current_state = STATE_LOGGED;
                    }
                }
                break;
            case STATE_LOGGED:
                if (current_weight < WEIGHT_THRESHOLD) {
                    printf("\nObject removed. Returning to Idle.\n");
                    current_state = STATE_IDLE;
                }
                else if (fabsf(current_weight - logged_weight) > REWEIGH_THRESHOLD) {
                    // *** CHANGE: REMOVED THE VERBOSE PRINTF HERE ***
                    current_state = STATE_REWEIGHING;
                    clock_gettime(CLOCK_MONOTONIC, &stability_timer_start);
                }
                break;
            case STATE_REWEIGHING:
                 if (fabsf(current_weight - logged_weight) < (REWEIGH_THRESHOLD / 2.0)) {
                    current_state = STATE_LOGGED;
                } else {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    double elapsed = (now.tv_sec - stability_timer_start.tv_sec) +
                                     (now.tv_nsec - stability_timer_start.tv_nsec) / 1e9;
                    if (elapsed >= STABLE_DURATION_SEC) {
                        printf("\nLogged updated weight: %.2f g\n", current_weight);
                        logged_weight = current_weight;
                        current_state = STATE_LOGGED;
                    }
                }
                break;
        }

        const char *state_str = "UNKNOWN";
        if (current_state == STATE_IDLE) state_str = "IDLE";
        else if (current_state == STATE_WEIGHING) state_str = "WEIGHING";
        else if (current_state == STATE_LOGGED) state_str = "LOGGED";
        else if (current_state == STATE_REWEIGHING) state_str = "RE-WEIGHING";
        
        // This line updates continuously without making a new line
        printf("\rState: %-11s | Weight: %8.2f g | Raw: %8ld   ", state_str, current_weight, raw_val);
        fflush(stdout);

        if (kbhit()) {
            char cmd = getchar();
            if (cmd == 't' || cmd == 'T') {
                printf("\n\nTaring... please wait.\n");
                long new_tare = get_averaged_reading(15);
                if (new_tare != -1) {
                    TARE_OFFSET = new_tare;
                    save_calibration();
                    printf("Tare complete. New offset: %ld\n", TARE_OFFSET);
                } else {
                    printf("Tare failed. Check connection.\n");
                }
            } else if (cmd == 'c' || cmd == 'C') {
                perform_calibration();
            }
        }
        usleep(200000);
    }

    restore_terminal();
    printf("\nReleasing GPIOs and exiting.\n");
    gpiod_line_release(dout_line);
    gpiod_line_release(sck_line);
    gpiod_chip_close(chip);
    return 0;
}
