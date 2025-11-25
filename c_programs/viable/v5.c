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
#include <sqlite3.h> // <-- NEW: For SQLite database

// --- Weighing Scale Configuration ---
#define HX711_CHIP_NAME "gpiochip2"
#define DOUT_PIN 5
#define SCK_PIN  4
#define CALIBRATION_FILE "/home/pico/hx711_calibration.dat"
#define LOG_FILE "/home/pico/weight_log.csv"
#define CALIBRATION_SAMPLES 20

// --- Tamper Detection Configuration ---
#define TAMPER_CHIP_NAME "gpiochip2" // <-- Use the correct chip for the tamper pin
#define TAMPER_PIN 7                // <-- Your GPIO pin for the reed switch (e.g., GPIO2_C7 = 71)
#define DB_FILE "/home/pico/mydata.db"
#define PRODUCT_ID_FILE "/home/pico/prod.id"
#define SAFE_MODE_DURATION_SEC 20

// --- Logging Logic Configuration ---
#define WEIGHT_THRESHOLD 10.0
#define STABLE_DURATION_SEC 3.0
#define REWEIGH_THRESHOLD 15.0

// --- Global Variables ---
struct gpiod_chip *hx711_chip, *tamper_chip;
struct gpiod_line *dout_line, *sck_line, *tamper_line;
long TARE_OFFSET = 0;
float SCALE_FACTOR = 430.0;
volatile sig_atomic_t running = 1;

// --- Signal Handler for Ctrl+C ---
void cleanup_and_exit(int signum); // Forward declaration

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

// --- File I/O & Database Functions ---
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

void log_weight_to_csv(float weight) {
    FILE *f = fopen(LOG_FILE, "a");
    if (f == NULL) {
        perror("\nError opening log file");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size == 0) {
        fprintf(f, "timedate,weight\n");
    }
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
    fprintf(f, "%s,%.2f\n", time_str, weight);
    fclose(f);
}

// --- NEW: Tamper Logging Function ---
void log_tamper_event() {
    char product_id[64] = "UNKNOWN";
    FILE *pid_file = fopen(PRODUCT_ID_FILE, "r");
    if (pid_file) {
        if (fgets(product_id, sizeof(product_id), pid_file) != NULL) {
            // Remove trailing newline, if any
            product_id[strcspn(product_id, "\n")] = 0;
        }
        fclose(pid_file);
    }

    sqlite3 *db;
    char *err_msg = 0;
    int rc = sqlite3_open(DB_FILE, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "\nCannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    char sql[256];
    snprintf(sql, sizeof(sql), "INSERT INTO tamper_log (product_id, tamper_type) VALUES ('%s', 'magnetic');", product_id);

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "\nSQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    } else {
        printf("\n--> Logged magnetic tamper event for product ID '%s' to the database.\n", product_id);
    }
    sqlite3_close(db);
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
    restore_terminal(); // Temporarily restore terminal for normal input
    printf("\n\n--- CALIBRATION MODE ---\n");
    printf("1. Place the scale/platform empty and press Enter for tare...");
    getchar();
    printf("    Taring... please wait.\n");
    long tare_raw = get_averaged_reading(CALIBRATION_SAMPLES);
    if (tare_raw == -1) {
        printf("Error during taring. Check connection.\n");
        init_terminal(); // Re-init non-blocking terminal
        return;
    }
    printf("    Tare complete. Zero point set to: %ld\n\n", tare_raw);
    printf("2. Enter the reference weight in grams (e.g., 100.0): ");
    float reference_weight;
    if (scanf("%f", &reference_weight) != 1 || reference_weight <= 0) {
        printf("    Invalid input. Calibration cancelled.\n");
        while (getchar() != '\n');
        init_terminal();
        return;
    }
    while (getchar() != '\n');
    printf("3. Place the %.2fg weight on the scale and press Enter...", reference_weight);
    getchar();
    printf("    Measuring... please wait.\n");
    long weight_raw = get_averaged_reading(CALIBRATION_SAMPLES);
    if (weight_raw == -1) {
        printf("    Error during measurement. Check connection.\n");
        init_terminal();
        return;
    }
    float new_scale_factor = (float)(weight_raw - tare_raw) / reference_weight;
    printf("\n--- Results ---\n");
    printf("    Calibration reading: %ld\n", weight_raw);
    printf("    Calculated scale factor: %g (previous: %g)\n", new_scale_factor, SCALE_FACTOR);
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
    init_terminal(); // Re-init non-blocking terminal
    printf("\n--- Returning to weight monitoring ---\n");
}

// --- GPIO Initialization ---
int init_gpio() {
    // --- HX711 GPIO ---
    hx711_chip = gpiod_chip_open_by_name(HX711_CHIP_NAME);
    if (!hx711_chip) {
        perror("Failed to open HX711 GPIO chip");
        return -1;
    }
    dout_line = gpiod_chip_get_line(hx711_chip, DOUT_PIN);
    sck_line = gpiod_chip_get_line(hx711_chip, SCK_PIN);
    if (!dout_line || !sck_line) {
        perror("Failed to get HX711 GPIO lines");
        return -1;
    }
    if (gpiod_line_request_input(dout_line, "hx711-logger") < 0) {
        perror("Request DOUT failed");
        return -1;
    }
    if (gpiod_line_request_output(sck_line, "hx711-logger", 0) < 0) {
        perror("Request SCK failed");
        return -1;
    }

    // --- Tamper GPIO ---
    tamper_chip = gpiod_chip_open_by_name(TAMPER_CHIP_NAME);
    if (!tamper_chip) {
        perror("Failed to open Tamper GPIO chip");
        return -1;
    }
    tamper_line = gpiod_chip_get_line(tamper_chip, TAMPER_PIN);
    if (!tamper_line) {
        perror("Failed to get Tamper GPIO line");
        return -1;
    }
    // Request as input with pull-down resistor if magnet holds it HIGH.
    // Or pull-up if magnet holds it LOW. Assuming pull-down for now.
    if (gpiod_line_request_input_flags(tamper_line, "tamper-detect", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN) < 0) {
        perror("Request Tamper Pin failed");
        return -1;
    }

    return 0;
}


// --- Main Program ---
int main() {
    // Main application state
    enum AppState { STATE_NORMAL_OPERATION, STATE_TAMPER_MODE };
    enum AppState app_state = STATE_NORMAL_OPERATION;

    // Weighing state machine
    enum WeighingState { WSTATE_IDLE, WSTATE_WEIGHING, WSTATE_LOGGED, WSTATE_REWEIGHING };
    enum WeighingState weighing_state = WSTATE_IDLE;
    struct timespec stability_timer_start;
    float logged_weight = 0.0;

    // Tamper state
    struct timespec tamper_timer_start;
    int is_tamper_logged = 0; // Flag to ensure we log only once per event

    signal(SIGINT, cleanup_and_exit);
    init_terminal();

    if (init_gpio() != 0) {
        cleanup_and_exit(0);
        return 1;
    }

    load_calibration();
    printf("Weight logger started.\n");
    printf("Commands: 't' to tare, 'c' to calibrate, Ctrl+C to exit.\n");

    while (running) {
        // --- Tamper Check (highest priority) ---
        int tamper_value = gpiod_line_get_value(tamper_line);
        // LOGIC: GPIO value 1 (HIGH) indicates tamper (magnet removed)
        if (tamper_value == 1) {
            if (app_state == STATE_NORMAL_OPERATION) {
                app_state = STATE_TAMPER_MODE;
                clock_gettime(CLOCK_MONOTONIC, &tamper_timer_start);
                printf("\n\n--- TAMPER DETECTED! Entering SAFE MODE. ---");
                if (!is_tamper_logged) {
                    log_tamper_event();
                    is_tamper_logged = 1;
                }
            }
        } else { // tamper_value == 0 (secure)
            if (app_state == STATE_TAMPER_MODE) {
                printf("\n--- Tamper Resolved. Returning to normal operation. ---\n");
                app_state = STATE_NORMAL_OPERATION;
                weighing_state = WSTATE_IDLE; // Reset weighing state
                is_tamper_logged = 0; // Reset log flag for next event
            }
        }

        // --- Main State Machine ---
        if (app_state == STATE_NORMAL_OPERATION) {
            long raw_val = get_averaged_reading(3);
            if (raw_val == -1) {
                printf("\rError: Reading failed. Check wiring.      ");
                fflush(stdout);
                usleep(100000);
                continue;
            }
            float current_weight = (raw_val - TARE_OFFSET) / SCALE_FACTOR;

            switch (weighing_state) {
                case WSTATE_IDLE:
                    if (current_weight > WEIGHT_THRESHOLD) {
                        weighing_state = WSTATE_WEIGHING;
                        clock_gettime(CLOCK_MONOTONIC, &stability_timer_start);
                    }
                    break;
                case WSTATE_WEIGHING:
                    if (current_weight < WEIGHT_THRESHOLD) {
                        weighing_state = WSTATE_IDLE;
                    } else {
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        double elapsed = (now.tv_sec - stability_timer_start.tv_sec) + (now.tv_nsec - stability_timer_start.tv_nsec) / 1e9;
                        if (elapsed >= STABLE_DURATION_SEC) {
                            printf("\nLogged weight: %.2f g\n", current_weight);
                            log_weight_to_csv(current_weight);
                            logged_weight = current_weight;
                            weighing_state = WSTATE_LOGGED;
                        }
                    }
                    break;
                case WSTATE_LOGGED:
                    if (current_weight < WEIGHT_THRESHOLD) {
                        printf("\nObject removed. Returning to Idle.\n");
                        weighing_state = WSTATE_IDLE;
                    } else if (fabsf(current_weight - logged_weight) > REWEIGH_THRESHOLD) {
                        weighing_state = WSTATE_REWEIGHING;
                        clock_gettime(CLOCK_MONOTONIC, &stability_timer_start);
                    }
                    break;
                case WSTATE_REWEIGHING:
                     if (fabsf(current_weight - logged_weight) < (REWEIGH_THRESHOLD / 2.0)) {
                        weighing_state = WSTATE_LOGGED;
                     } else {
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        double elapsed = (now.tv_sec - stability_timer_start.tv_sec) + (now.tv_nsec - stability_timer_start.tv_nsec) / 1e9;
                        if (elapsed >= STABLE_DURATION_SEC) {
                            printf("\nLogged updated weight: %.2f g\n", current_weight);
                            log_weight_to_csv(current_weight);
                            logged_weight = current_weight;
                            weighing_state = WSTATE_LOGGED;
                        }
                    }
                    break;
            }

            const char *state_str = "UNKNOWN";
            if (weighing_state == WSTATE_IDLE) state_str = "IDLE";
            else if (weighing_state == WSTATE_WEIGHING) state_str = "WEIGHING";
            else if (weighing_state == WSTATE_LOGGED) state_str = "LOGGED";
            else if (weighing_state == WSTATE_REWEIGHING) state_str = "RE-WEIGHING";

            printf("\rState: %-11s | Weight: %8.2f g", state_str, current_weight);
            fflush(stdout);

        } else { // app_state == STATE_TAMPER_MODE
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - tamper_timer_start.tv_sec) + (now.tv_nsec - tamper_timer_start.tv_nsec) / 1e9;
            if (elapsed < SAFE_MODE_DURATION_SEC) {
                int countdown = SAFE_MODE_DURATION_SEC - (int)elapsed;
                printf("\rSAFE MODE: System locked. Resecure to proceed. Countdown: %2ds ", countdown);
            } else {
                printf("\rSAFE MODE: System LOCKED. Resecure the device.             ");
            }
            fflush(stdout);
        }

        // --- Keyboard Input Check ---
        if (kbhit()) {
            char cmd = getchar();
            if (app_state == STATE_NORMAL_OPERATION) {
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
        }
        usleep(150000); // Main loop delay
    }

    cleanup_and_exit(0);
    return 0;
}

// --- Cleanup Function ---
void cleanup_and_exit(int signum) {
    if (running == 0) return; // Prevent double-execution
    running = 0;

    restore_terminal();
    printf("\nReleasing GPIOs and exiting.\n");

    if (dout_line) gpiod_line_release(dout_line);
    if (sck_line) gpiod_line_release(sck_line);
    if (tamper_line) gpiod_line_release(tamper_line);

    if (hx711_chip) gpiod_chip_close(hx711_chip);
    if (tamper_chip) gpiod_chip_close(tamper_chip);

    // If called by signal handler, exit explicitly
    if (signum != 0) {
        exit(0);
    }
}
