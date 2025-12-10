#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <time.h>
#include <signal.h>

// I2C Configuration
#define INA219_ADDRESS  0x40
#define I2C_DEVICE      "/dev/i2c-3"

// INA219 Registers
#define INA219_REG_CONFIG       0x00
#define INA219_REG_BUSVOLTAGE   0x02

// Config: 32V Range, 12-bit ADC, Continuous Mode
// (Removes Gain/Shunt settings as they are irrelevant for Bus Voltage only)
#define INA219_CONFIG_BVOLTAGERANGE_32V    (0x2000)
#define INA219_CONFIG_BADCRES_12BIT        (0x0180)
#define INA219_CONFIG_MODE_SANDBVOLT_CONT  (0x0007)

// Voltage Monitoring Thresholds
#define REF_VOLTAGE     3.3f
#define TOLERANCE       2.0f
#define MIN_VOLTAGE     (REF_VOLTAGE - TOLERANCE) // 1.3V
#define MAX_VOLTAGE     (REF_VOLTAGE + TOLERANCE) // 5.3V

static int i2c_fd = -1;
static volatile sig_atomic_t running = 1;

// Write 16-bit value to register
int ina219_write16(uint8_t reg, uint16_t value) {
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (value >> 8) & 0xFF;
    buf[2] = value & 0xFF;

    if (write(i2c_fd, buf, 3) != 3) {
        perror("I2C Write Error");
        return -1;
    }
    return 0;
}

// Read 16-bit value from register
int16_t ina219_read16(uint8_t reg) {
    uint8_t buf[2];
    
    if (write(i2c_fd, &reg, 1) != 1) {
        perror("I2C Address Write Error");
        return -1;
    }
    
    if (read(i2c_fd, buf, 2) != 2) {
        perror("I2C Read Error");
        return -1;
    }
    
    return (int16_t)((buf[0] << 8) | buf[1]);
}

// Initialize INA219
int ina219_init() {
    i2c_fd = open(I2C_DEVICE, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C bus");
        return -1;
    }

    if (ioctl(i2c_fd, I2C_SLAVE, INA219_ADDRESS) < 0) {
        perror("Failed to acquire bus access and/or talk to slave");
        close(i2c_fd);
        return -1;
    }

    // Configure for Bus Voltage reading only
    uint16_t config = INA219_CONFIG_BVOLTAGERANGE_32V |
                      INA219_CONFIG_BADCRES_12BIT |
                      INA219_CONFIG_MODE_SANDBVOLT_CONT;

    return ina219_write16(INA219_REG_CONFIG, config);
}

// Get Bus Voltage
float get_bus_voltage() {
    int16_t raw_value = ina219_read16(INA219_REG_BUSVOLTAGE);
    
    // Shift right 3 bits to drop status flags, multiply by 4mV LSB
    return (float)((raw_value >> 3) * 4) * 0.001f;
}

void signal_handler(int sig) {
    running = 0;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Starting Voltage Monitor...\n");
    printf("Target: %.2fV (Range: %.2fV - %.2fV)\n", REF_VOLTAGE, MIN_VOLTAGE, MAX_VOLTAGE);

    if (ina219_init() < 0) {
        return 1;
    }

    while (running) {
        float voltage = get_bus_voltage();

        if (voltage < MIN_VOLTAGE || voltage > MAX_VOLTAGE) {
            printf("[ALERT] Voltage OUT OF RANGE: %.3f V\n", voltage);
        } else {
            printf("[OK] Voltage: %.3f V\n", voltage);
        }

        // Sleep 1 second
        sleep(1);
    }

    if (i2c_fd >= 0) close(i2c_fd);
    printf("\nExiting.\n");
    return 0;
}
