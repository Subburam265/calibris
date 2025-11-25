#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpiod.h>
#include <stdbool.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>
#include "lcd.h" // Add your lcd.h header here

#define DB_PATH        "/home/pico/mydata.db"
#define PRODUCT_ID_FILE "/home/pico/prod.id"
#define RENEWAL_CYCLE_FILE "/home/pico/cc.num"
#define TAMPER_TYPE    "magnetic"
#define LOG_FILE       "/home/pico/tamper_log.txt"

// Adjust these as per your hardware
const char *chipname = "gpiochip1";
const unsigned int line_offset = 23; // GPIO1_B2_d

// LCD config
#define I2C_BUS "/dev/i2c-3"
#define I2C_ADDR 0x27

// Helper to read a single line from a file
int read_single_line(const char *filename, char *buf, size_t sz) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;
    if (!fgets(buf, sz, f)) {
        fclose(f);
        return -1;
    }
    // Remove trailing newline/whitespace
    buf[strcspn(buf, "\r\n")] = 0;
    fclose(f);
    return 0;
}

// Helper to get current timestamp as string
void get_timestamp(char *buf, size_t sz) {
    time_t now = time(NULL);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

int main(int argc, char **argv) {
    char product_id[64], renewal_cycle[64];
    char timestamp[32];
    int ret;

    // --- Read product ID and renewal cycle ---
    if (read_single_line(PRODUCT_ID_FILE, product_id, sizeof(product_id)) != 0) {
        fprintf(stderr, "Product ID empty or cannot read file, exiting.\n");
        return 1;
    }
    if (read_single_line(RENEWAL_CYCLE_FILE, renewal_cycle, sizeof(renewal_cycle)) != 0) {
        strcpy(renewal_cycle, ""); // Optional: allow empty
    }

    // --- LCD Initialization ---
    if (lcd_init(I2C_BUS, I2C_ADDR) != 0) {
        fprintf(stderr, "Failed to initialize LCD.\n");
        return 1;
    }
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Magnetic Tamper");
    lcd_set_cursor(1, 0);
    lcd_send_string("Monitor Ready");
    usleep(1500000); // Show splash for 1.5s
    lcd_clear();

    // --- GPIO Initialization ---
    struct gpiod_chip *chip = gpiod_chip_open_by_name(chipname);
    if (!chip) {
        perror("Error opening GPIO chip");
        lcd_clear();
        lcd_set_cursor(0,0);
        lcd_send_string("GPIO init fail!");
        return 1;
    }
    struct gpiod_line *line = gpiod_chip_get_line(chip, line_offset);
    if (!line) {
        perror("Error getting GPIO line");
        gpiod_chip_close(chip);
        lcd_clear();
        lcd_set_cursor(0,0);
        lcd_send_string("GPIO line fail!");
        return 1;
    }
    ret = gpiod_line_request_input(line, "tamper_detect");
    if (ret < 0) {
        perror("Error requesting GPIO line as input");
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        lcd_clear();
        lcd_set_cursor(0,0);
        lcd_send_string("GPIO req fail!");
        return 1;
    }

    printf("Monitoring GPIO pin %s:%u for tamper detection...\n", chipname, line_offset);
    printf("Press Ctrl+C to exit.\n");

    bool tampered_state = false;

    // --- Monitoring Loop ---
    while (1) {
        int current_value = gpiod_line_get_value(line);
        if (current_value < 0) {
            perror("Error reading GPIO line value");
            break;
        }

        // Rising edge: tamper detected (pin HIGH)
        if (current_value == 1 && !tampered_state) {
            tampered_state = true;
            get_timestamp(timestamp, sizeof(timestamp));

            // Log to SQLite
            sqlite3 *db;
            char *err_msg = NULL;
            char sql[512];
            snprintf(sql, sizeof(sql),
                "INSERT INTO tamper_log (product_id, created_at, tamper_type, renewal_cycle) "
                "VALUES (%s, '%s', '%s', '%s');",
                product_id, timestamp, TAMPER_TYPE, renewal_cycle);

            if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
                sqlite3_exec(db, sql, 0, 0, &err_msg);
                sqlite3_close(db);
            } else {
                fprintf(stderr, "Could not open database for logging\n");
            }

            // Log to text file
            FILE *logf = fopen(LOG_FILE, "a");
            if (logf) {
                fprintf(logf, "%s: Tamper detected on GPIO pin %s:%u!\n", timestamp, chipname, line_offset);
                fclose(logf);
            }

            // Show warning and stop scale app
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_send_string("SAFE MODE");
            lcd_set_cursor(1, 0);
            lcd_send_string("Remove Magnet");
            // Stop the measure_weight.service
            system("systemctl stop measure_weight.service");

            printf("Magnetic tamper detected! (Pin HIGH)\n");
            fflush(stdout);
        }
        // Falling edge: tamper condition cleared (pin LOW)
        else if (current_value == 0 && tampered_state) {
            tampered_state = false;

            // Start the measure_weight.service
            system("systemctl start measure_weight.service");

            // Optionally clear LCD, but the scale app will take over LCD display
            // lcd_clear();
            printf("Tamper condition cleared. (Pin LOW)\n");
            fflush(stdout);
        }

        usleep(100000); // 100ms
    }

    printf("\nCleaning up and exiting.\n");
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    lcd_clear();
    lcd_set_cursor(0,0);
    lcd_send_string("Goodbye!");
    lcd_close();
    return 0;
}
