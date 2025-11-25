#include <gpiod.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ls.h" // Your lock screen header

// --- MODIFIED: Configuration section is now here ---
const char *CHIP_NAME = "gpiochip2";
const int GPIO_PIN = 7;
#define DB_FILE "/home/pico/mydata.db"
#define PRODUCT_ID_FILE "/home/pico/prod.id"

// --- MODIFIED: log_tamper_event function is now here ---
void log_tamper_event() {
    char product_id[64] = "UNKNOWN";
    FILE *pid_file = fopen(PRODUCT_ID_FILE, "r");
    if (pid_file) {
        if (fgets(product_id, sizeof(product_id), pid_file) != NULL) {
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

    char *m_sql = sqlite3_mprintf("INSERT INTO tamper_log (product_id, tamper_type) VALUES ('%q', 'magnetic');", product_id);
    rc = sqlite3_exec(db, m_sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "\nSQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    } else {
        printf("--> Tamper event logged for product ID '%s'.\n", product_id);
    }

    sqlite3_free(m_sql);
    sqlite3_close(db);
}


int main() {
    struct gpiod_chip *chip;
    struct gpiod_line *line;

    enum State { STATE_SECURE, STATE_COOLDOWN };
    enum State current_state = STATE_SECURE;
    time_t cooldown_start_time;
    const int COOLDOWN_SECONDS = 10;

    chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (!chip) { perror("gpiod_chip_open_by_name"); return 1; }

    line = gpiod_chip_get_line(chip, GPIO_PIN);
    if (!line) { perror("gpiod_chip_get_line"); gpiod_chip_close(chip); return 1; }

    if (gpiod_line_request_input(line, "tamper-detector") < 0) {
        perror("gpiod_line_request_input"); gpiod_chip_close(chip); return 1; }

    printf("Tamper detection system armed.\n");
    int initial_value = gpiod_line_get_value(line);
    printf("Initial state: %s\n", initial_value == 1 ? "TAMPERED (Magnet Present)" : "Secure (Magnet Absent)");

    if (initial_value == 1) {
        current_state = STATE_COOLDOWN;
        cooldown_start_time = time(NULL);
        printf("WARNING: Starting in a tampered state! Cooldown initiated.\n");
        log_tamper_event();
    }

    while (1) {
        int value = gpiod_line_get_value(line);

        switch (current_state) {
            case STATE_SECURE:
                if (value == 1) {
                    printf("\n\nTAMPER DETECTED (Magnet Present)!\n");
                    log_tamper_event();
                    cooldown_start_time = time(NULL);
                    current_state = STATE_COOLDOWN;
                }
                break;

            case STATE_COOLDOWN:
                if (value == 0) {
                    printf("\nCooldown cancelled. System is Secure.\n");
                    current_state = STATE_SECURE;
                } else {
                    time_t now = time(NULL);
                    double elapsed = difftime(now, cooldown_start_time);

                    if (elapsed >= COOLDOWN_SECONDS) {
                        printf("\nCooldown finished. Entering safe mode...\n");
                        sleep(1);
    			enter_safe_mode();
                        goto exit_program;
                    } else {
                        int remaining = COOLDOWN_SECONDS - (int)elapsed;
                        printf("Cooldown active... locking in %d seconds. Remove magnet to cancel.", remaining);
                        fflush(stdout);
                    }
                }
                break;
        }
        usleep(200000);
    }

exit_program:
    printf("\nExiting program.\n");
    gpiod_line_release(line);
    gpiod_chip_close(chip);

    return 0;
}
