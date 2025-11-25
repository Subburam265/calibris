#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define HX711_DOUT_PIN 69
#define HX711_SCK_PIN 68

#define CAL_FILE "/home/pico/hx711_cal.txt"

// Export GPIO pin
int gpio_export(int pin) {
    char buf[64];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return -1;
    snprintf(buf, sizeof(buf), "%d", pin);
    write(fd, buf, strlen(buf));
    close(fd);
    return 0;
}

// Set GPIO direction: "in" or "out"
int gpio_set_dir(int pin, const char *dir) {
    char path[64];
    int fd;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, dir, strlen(dir));
    close(fd);
    return 0;
}

// Write GPIO value: 0 or 1
int gpio_write(int pin, int value) {
    char path[64], val = value ? '1' : '0';
    int fd;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, &val, 1);
    close(fd);
    return 0;
}

// Read GPIO value: returns 0 or 1
int gpio_read(int pin) {
    char path[64], val;
    int fd;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    read(fd, &val, 1);
    close(fd);
    return val == '1' ? 1 : 0;
}

// Delay microseconds
void delay_us(int us) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = us * 1000;
    nanosleep(&ts, NULL);
}

// Read 24 bits from HX711
long hx711_read_raw() {
    long count = 0;
    int i;
    // Wait for data ready pin to go LOW
    while (gpio_read(HX711_DOUT_PIN) == 1)
        usleep(1000);

    // Read 24 bits
    for (i = 0; i < 24; i++) {
        gpio_write(HX711_SCK_PIN, 1);
        delay_us(1); // 1 us delay
        count = count << 1;
        gpio_write(HX711_SCK_PIN, 0);
        delay_us(1);
        int val = gpio_read(HX711_DOUT_PIN);
        if (val < 0) return -1;
        if (val) count++;
    }

    // Set gain and channel (1 pulse for 128 gain)
    gpio_write(HX711_SCK_PIN, 1);
    delay_us(1);
    gpio_write(HX711_SCK_PIN, 0);
    delay_us(1);

    // Convert from 24-bit 2's complement format
    if (count & 0x800000)
        count |= ~(0xffffff);

    return count;
}

// Load calibration factor from file
float load_calibration() {
    FILE *f = fopen(CAL_FILE, "r");
    if (!f) return 1.0; // default calibration
    float val;
    if (fscanf(f, "%f", &val) != 1) val = 1.0;
    fclose(f);
    return val;
}

// Save calibration factor to file
void save_calibration(float val) {
    FILE *f = fopen(CAL_FILE, "w");
    if (!f) {
        perror("Error saving calibration");
        return;
    }
    fprintf(f, "%f\n", val);
    fclose(f);
}

// Get average raw reading for tare
long get_tare_value(int samples) {
    long sum = 0;
    for (int i = 0; i < samples; i++) {
        long val = hx711_read_raw();
        if (val < 0) {
            printf("Error reading HX711\n");
            return 0;
        }
        sum += val;
        usleep(10000);
    }
    return sum / samples;
}

int main() {
    // Setup GPIO pins
    if (gpio_export(HX711_DOUT_PIN) < 0 || gpio_export(HX711_SCK_PIN) < 0) {
        printf("Failed to export GPIO pins\n");
        return 1;
    }
    if (gpio_set_dir(HX711_DOUT_PIN, "in") < 0 || gpio_set_dir(HX711_SCK_PIN, "out") < 0) {
        printf("Failed to set GPIO directions\n");
        return 1;
    }
    gpio_write(HX711_SCK_PIN, 0);

    float cal_factor = load_calibration();
    long tare_val = 0;

    printf("HX711 Load Cell Reader Started\n");
    printf("Current calibration factor: %.3f\n", cal_factor);
    printf("Commands: t - tare, r - recalibrate, c - change calibration factor, q - quit\n");

    char input;
    while (1) {
        printf("Reading weight...\n");
        long raw = hx711_read_raw();
        float weight = (raw - tare_val) / cal_factor;
        printf("Weight: %.3f\n", weight);

        // Non-blocking input check
        // For simplicity, here polling stdin
        if (scanf(" %c", &input) == 1) {
            if (input == 't') {
                printf("Taring in progress...\n");
                tare_val = get_tare_value(10);
                printf("Tare complete: %ld\n", tare_val);
            } else if (input == 'r') {
                printf("Recalibration procedure:\n");
                printf("Place known weight on sensor, enter weight value:\n");
                float known_weight;
                scanf("%f", &known_weight);
                long raw_val = get_tare_value(10);
                cal_factor = (raw_val - tare_val) / known_weight;
                printf("New calibration factor: %.3f\n", cal_factor);
                save_calibration(cal_factor);
            } else if (input == 'c') {
                printf("Enter new calibration factor:\n");
                float new_cal;
                scanf("%f", &new_cal);
                cal_factor = new_cal;
                save_calibration(cal_factor);
                printf("Calibration factor updated.\n");
            } else if (input == 'q') {
                printf("Exiting.\n");
                break;
            }
        }
        usleep(500000); // 500ms delay
    }
    return 0;
}
