#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>
#include <termios.h>

#define HX711_DOUT_PIN 69
#define HX711_SCK_PIN 68

#define CAL_FILE "/home/pico/hx711_cal.txt"

// File descriptors for GPIO value files (kept open for faster access)
static int fd_dout_value = -1;
static int fd_sck_value = -1;

// Export GPIO pin
int gpio_export(int pin) {
    char buf[64];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
        // Pin might already be exported
        return 0;
    }
    snprintf(buf, sizeof(buf), "%d", pin);
    int ret = write(fd, buf, strlen(buf));
    close(fd);
    usleep(100000); // Wait 100ms for sysfs to create files
    return ret < 0 ? -1 : 0;
}

// Unexport GPIO pin
int gpio_unexport(int pin) {
    char buf[64];
    int fd = open("/sys/class/gpio/unexport", O_WRONLY);
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
    
    // Retry a few times in case the file isn't ready yet
    for (int i = 0; i < 10; i++) {
        fd = open(path, O_WRONLY);
        if (fd >= 0) break;
        usleep(50000); // Wait 50ms
    }
    
    if (fd < 0) {
        printf("Error opening %s: %s\n", path, strerror(errno));
        return -1;
    }
    
    int ret = write(fd, dir, strlen(dir));
    close(fd);
    return ret < 0 ? -1 : 0;
}

// Open GPIO value files for faster access
int gpio_open_value_files() {
    char path[64];
    
    // Open DOUT pin value file for reading
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", HX711_DOUT_PIN);
    fd_dout_value = open(path, O_RDONLY);
    if (fd_dout_value < 0) {
        printf("Error opening DOUT value file: %s\n", strerror(errno));
        return -1;
    }
    
    // Open SCK pin value file for writing
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", HX711_SCK_PIN);
    fd_sck_value = open(path, O_WRONLY);
    if (fd_sck_value < 0) {
        printf("Error opening SCK value file: %s\n", strerror(errno));
        close(fd_dout_value);
        return -1;
    }
    
    return 0;
}

// Close GPIO value files
void gpio_close_value_files() {
    if (fd_dout_value >= 0) close(fd_dout_value);
    if (fd_sck_value >= 0) close(fd_sck_value);
}

// Write GPIO value using cached file descriptor
int gpio_write_fast(int value) {
    char val = value ? '1' : '0';
    if (fd_sck_value < 0) return -1;
    lseek(fd_sck_value, 0, SEEK_SET);
    return write(fd_sck_value, &val, 1) == 1 ? 0 : -1;
}

// Read GPIO value using cached file descriptor
int gpio_read_fast() {
    char val;
    if (fd_dout_value < 0) return -1;
    lseek(fd_dout_value, 0, SEEK_SET);
    if (read(fd_dout_value, &val, 1) != 1) return -1;
    return val == '1' ? 1 : 0;
}

// Delay microseconds (more accurate version)
void delay_us(int us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

// Initialize HX711
int hx711_init() {
    // Reset HX711 by toggling SCK
    gpio_write_fast(1);
    usleep(100);
    gpio_write_fast(0);
    usleep(100);
    
    // Wait for HX711 to be ready
    int timeout = 0;
    while (gpio_read_fast() == 1 && timeout < 100) {
        usleep(10000); // 10ms
        timeout++;
    }
    
    if (timeout >= 100) {
        printf("HX711 initialization timeout\n");
        return -1;
    }
    
    return 0;
}

// Check if HX711 is ready
int hx711_is_ready() {
    return gpio_read_fast() == 0;
}

// Read 24 bits from HX711
long hx711_read_raw() {
    long count = 0;
    int i;
    
    // Wait for data ready (DOUT goes LOW)
    int timeout = 0;
    while (gpio_read_fast() == 1) {
        if (timeout++ > 1000) { // 1 second timeout
            printf("HX711 timeout waiting for data\n");
            return -1;
        }
        usleep(1000);
    }
    
    // Read 24 bits
    // Correct sequence
	for (i = 0; i < 24; i++) {
    		gpio_write_fast(1);     // SCK goes HIGH
    		delay_us(100);
    
    		count = count << 1;     // Shift the bits to make room for the new one

    		// --- Read the bit WHILE SCK is HIGH ---
    		int val = gpio_read_fast();
    		if (val < 0) {
        		printf("Error reading bit %d\n", i);
        		return -1;
    		}
    		if (val) {
        	count++; // Or use: count |= 1;
    		}
    		// -------------------------------------

    		gpio_write_fast(0);     // SCK goes LOW
    		delay_us(100);
}
    
    // Set gain for next reading (1 pulse = gain 128, channel A)
    gpio_write_fast(1);
    delay_us(100);
    gpio_write_fast(0);
    delay_us(100);
    
    // Convert from 24-bit 2's complement
    if (count & 0x800000) {
        count |= 0xFF000000; // Sign extend
    }
    
    return count;
}

// Get average reading with outlier rejection
long hx711_read_average(int samples) {
    if (samples < 1) samples = 1;
    
    long readings[samples];
    int valid_count = 0;
    
    // Collect readings
    for (int i = 0; i < samples; i++) {
        long val = hx711_read_raw();
        if (val != -1) {
            readings[valid_count++] = val;
        }
        usleep(10000); // 10ms between readings
    }
    
    if (valid_count == 0) {
        printf("No valid readings obtained\n");
        return -1;
    }
    
    // Simple average (could implement median for better outlier rejection)
    long sum = 0;
    for (int i = 0; i < valid_count; i++) {
        sum += readings[i];
    }
    
    return sum / valid_count;
}

// Load calibration data
int load_calibration(float *cal_factor, long *tare_val) {
    FILE *f = fopen(CAL_FILE, "r");
    if (!f) {
        *cal_factor = 1.0;
        *tare_val = 0;
        return 0;
    }
    
    if (fscanf(f, "%f %ld", cal_factor, tare_val) != 2) {
        *cal_factor = 1.0;
        *tare_val = 0;
        fclose(f);
        return 0;
    }
    
    fclose(f);
    return 1;
}

// Save calibration data
void save_calibration(float cal_factor, long tare_val) {
    FILE *f = fopen(CAL_FILE, "w");
    if (!f) {
        perror("Error saving calibration");
        return;
    }
    fprintf(f, "%f %ld\n", cal_factor, tare_val);
    fclose(f);
    printf("Calibration saved: factor=%.3f, tare=%ld\n", cal_factor, tare_val);
}

// Check for keyboard input without blocking
int kbhit() {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

int main() {
    printf("HX711 Load Cell Reader for Luckfox Pico\n");
    printf("Initializing GPIO pins...\n");
    
    // Clean up any previous exports
    gpio_unexport(HX711_DOUT_PIN);
    gpio_unexport(HX711_SCK_PIN);
    usleep(100000);
    
    // Export GPIO pins
    if (gpio_export(HX711_DOUT_PIN) < 0 || gpio_export(HX711_SCK_PIN) < 0) {
        printf("Warning: GPIO export returned error (pins may already be exported)\n");
    }
    
    // Set directions
    if (gpio_set_dir(HX711_DOUT_PIN, "in") < 0) {
        printf("Failed to set DOUT pin as input\n");
        return 1;
    }
    
    if (gpio_set_dir(HX711_SCK_PIN, "out") < 0) {
        printf("Failed to set SCK pin as output\n");
        return 1;
    }
    
    // Open value files for faster access
    if (gpio_open_value_files() < 0) {
        printf("Failed to open GPIO value files\n");
        return 1;
    }
    
    // Initialize HX711
    printf("Initializing HX711...\n");
    if (hx711_init() < 0) {
        printf("Failed to initialize HX711\n");
        gpio_close_value_files();
        return 1;
    }
    
    // Load calibration
    float cal_factor;
    long tare_val;
    if (load_calibration(&cal_factor, &tare_val)) {
        printf("Loaded calibration: factor=%.3f, tare=%ld\n", cal_factor, tare_val);
    } else {
        cal_factor = 430.0; // Default calibration factor (adjust based on your load cell)
        tare_val = 0;
        printf("Using default calibration factor: %.3f\n", cal_factor);
    }
    
    printf("\nCommands:\n");
    printf("  t - Tare/Zero the scale\n");
    printf("  c - Calibrate with known weight\n");
    printf("  f - Manually set calibration factor\n");
    printf("  r - Reset HX711\n");
    printf("  q - Quit\n\n");
    
    // Set terminal to non-canonical mode for immediate input
    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    
    int running = 1;
    while (running) {
        // Read weight
        long raw = hx711_read_average(3);
        if (raw != -1) {
            float weight = (raw - tare_val) / cal_factor;
            printf("\rRaw: %ld | Weight: %.2f g          ", raw, weight);
            fflush(stdout);
        } else {
            printf("\rError reading HX711                    ");
            fflush(stdout);
        }
        
        // Check for keyboard input
        if (kbhit()) {
            char cmd = getchar();
            printf("\n");
            
            switch (cmd) {
                case 't':
                case 'T':
                    printf("Taring... Remove all weight from scale.\n");
                    usleep(2000000); // Give user time to remove weight
                    tare_val = hx711_read_average(10);
                    if (tare_val != -1) {
                        printf("Tare complete: %ld\n", tare_val);
                        save_calibration(cal_factor, tare_val);
                    } else {
                        printf("Tare failed!\n");
                    }
                    break;
                    
                case 'c':
                case 'C':
                    printf("Calibration procedure:\n");
                    printf("1. Remove all weight and press Enter to tare...");
                    getchar(); // Wait for Enter
                    tare_val = hx711_read_average(10);
                    if (tare_val == -1) {
                        printf("Tare failed!\n");
                        break;
                    }
                    printf("Tare value: %ld\n", tare_val);
                    
                    printf("2. Place known weight on scale and enter weight in grams: ");
                    float known_weight;
                    scanf("%f", &known_weight);
                    getchar(); // Consume newline
                    
                    long cal_raw = hx711_read_average(10);
                    if (cal_raw != -1 && known_weight > 0) {
                        cal_factor = (cal_raw - tare_val) / known_weight;
                        printf("New calibration factor: %.3f\n", cal_factor);
                        save_calibration(cal_factor, tare_val);
                    } else {
                        printf("Calibration failed!\n");
                    }
                    break;
                    
                case 'f':
                case 'F':
                    printf("Enter new calibration factor: ");
                    scanf("%f", &cal_factor);
                    getchar(); // Consume newline
                    save_calibration(cal_factor, tare_val);
                    printf("Calibration factor set to: %.3f\n", cal_factor);
                    break;
                    
                case 'r':
                case 'R':
                    printf("Resetting HX711...\n");
                    hx711_init();
                    break;
                    
                case 'q':
                case 'Q':
                    running = 0;
                    break;
            }
        }
        
        usleep(100000); // 100ms delay between readings
    }
    
    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    
    printf("\nShutting down...\n");
    gpio_close_value_files();
    
    // Optionally unexport pins
    gpio_unexport(HX711_DOUT_PIN);
    gpio_unexport(HX711_SCK_PIN);
    
    return 0;
}
