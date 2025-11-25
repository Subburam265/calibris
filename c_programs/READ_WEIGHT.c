#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <termios.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>

// --- Configuration ---
#define CHIP_NAME "gpiochip2"
#define DOUT_PIN  5
#define SCK_PIN   4
#define CALIBRATION_FILE "/home/pico/hx711_calibration.dat"

// --- Logging Logic Configuration ---
#define WEIGHT_THRESHOLD 10.0      // (grams) Ignore weights below this value.
#define STABLE_DURATION_SEC 3.0    // (seconds) Weight must be stable for this long for all events.
#define REWEIGH_THRESHOLD 15.0     // (grams) A new weight must be added that is > this value to trigger re-weighing.

// --- Global Variables ---
struct gpiod_chip *chip;
struct gpiod_line *dout_line;
struct gpiod_line *sck_line;
long TARE_OFFSET = 0;
float SCALE_FACTOR = 430.0;
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
    if (count & 0x800000) count |= ~0xFFFFFF;
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
    } else {
        perror("Error saving calibration file");
    }
}

void load_calibration() {
    FILE *f = fopen(CALIBRATION_FILE, "r");
    if (f) {
        fscanf(f, "%ld\n%f", &TARE_OFFSET, &SCALE_FACTOR);
        fclose(f);
        printf("Calibration loaded from file.\n");
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

// --- Main Program ---
int main() {
    enum State { STATE_IDLE, STATE_WEIGHING, STATE_LOGGED, STATE_REWEIGHING };
    enum State current_state = STATE_IDLE;
    struct timespec stability_timer_start;
    float logged_weight = 0.0;

    signal(SIGINT, cleanup_and_exit);
    init_terminal();
    
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    dout_line = gpiod_chip_get_line(chip, DOUT_PIN);
    sck_line = gpiod_chip_get_line(chip, SCK_PIN);
    
    struct gpiod_line_request_config dout_config;
    dout_config.consumer = "hx711-logger";
    dout_config.request_type = GPIOD_LINE_REQUEST_DIRECTION_INPUT;
    dout_config.flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN;
    if (gpiod_line_request(dout_line, &dout_config, 0) < 0) { perror("Request DOUT failed"); return 1; }
    if (gpiod_line_request_output(sck_line, "hx711-logger", 0) < 0) { perror("Request SCK failed"); return 1; }
    
    load_calibration();
    printf("Weight logger started. Press 't' to tare, or Ctrl+C to exit.\n");

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
                    double elapsed = (now.tv_sec - stability_timer_start.tv_sec) + (now.tv_nsec - stability_timer_start.tv_nsec) / 1e9;
                    if (elapsed >= STABLE_DURATION_SEC) {
                        printf("\n--- Logged new weight: %.2f g ---\n", current_weight);
                        logged_weight = current_weight;
                        current_state = STATE_LOGGED;
                    }
                }
                break;
            case STATE_LOGGED:
                if (current_weight < WEIGHT_THRESHOLD) {
                    printf("\n--- Object removed, resetting ---\n");
                    current_state = STATE_IDLE;
                } 
                else if (current_weight > (logged_weight + REWEIGH_THRESHOLD)) {
                    printf("\n--- Potential new weight added, checking stability... ---\n");
                    current_state = STATE_REWEIGHING;
                    clock_gettime(CLOCK_MONOTONIC, &stability_timer_start);
                }
                break;
            case STATE_REWEIGHING:
                if (current_weight < (logged_weight + REWEIGH_THRESHOLD)) {
                    printf("\n--- Re-weigh cancelled, returning to logged state. ---\n");
                    current_state = STATE_LOGGED;
                } else {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    double elapsed = (now.tv_sec - stability_timer_start.tv_sec) + (now.tv_nsec - stability_timer_start.tv_nsec) / 1e9;
                    if (elapsed >= STABLE_DURATION_SEC) {
                        printf("\n--- Logged updated weight: %.2f g ---\n", current_weight);
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
        else if (current_state == STATE_REWEIGHING) state_str = "REWEIGHING";
        printf("\rState: %-10s | Weight: %8.2f g", state_str, current_weight);
        fflush(stdout);

        if (kbhit()) {
            char cmd = getchar();
            if (cmd == 't' || cmd == 'T') {
                printf("\n\nTaring... please wait.\n");
                TARE_OFFSET = get_averaged_reading(15);
                save_calibration();
                printf("Tare complete. New offset: %ld\n", TARE_OFFSET);
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
