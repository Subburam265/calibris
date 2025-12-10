#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

// INA219 I2C Address (default)
#define INA219_ADDRESS 0x40

// INA219 Registers
#define INA219_REG_CONFIG       0x00
#define INA219_REG_SHUNTVOLTAGE 0x01
#define INA219_REG_BUSVOLTAGE   0x02
#define INA219_REG_POWER        0x03
#define INA219_REG_CURRENT      0x04
#define INA219_REG_CALIBRATION  0x05

// Configuration values (32V, 2A range)
#define INA219_CONFIG_BVOLTAGERANGE_32V     (0x2000)
#define INA219_CONFIG_GAIN_8_320MV          (0x1800)
#define INA219_CONFIG_BADCRES_12BIT         (0x0180)
#define INA219_CONFIG_SADCRES_12BIT_1S      (0x0018)
#define INA219_CONFIG_MODE_SANDBVOLT_CONT   (0x0007)

static int i2c_fd = -1;
static float current_lsb = 0.0;
static float power_lsb = 0.0;

// Write 16-bit value to register
int ina219_write16(uint8_t reg, uint16_t value) {
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (value >> 8) & 0xFF;  // MSB first
    buf[2] = value & 0xFF;         // LSB
    
    if (write(i2c_fd, buf, 3) != 3) {
        perror("Failed to write to INA219");
        return -1;
    }
    return 0;
}

// Read 16-bit value from register
int16_t ina219_read16(uint8_t reg) {
    uint8_t buf[2];
    
    if (write(i2c_fd, &reg, 1) != 1) {
        perror("Failed to write register address");
        return -1;
    }
    
    if (read(i2c_fd, buf, 2) != 2) {
        perror("Failed to read from INA219");
        return -1;
    }
    
    return (buf[0] << 8) | buf[1];
}

// Initialize INA219
int ina219_init(const char *i2c_device) {
    // Open I2C device
    i2c_fd = open(i2c_device, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C device");
        return -1;
    }
    
    // Set I2C slave address
    if (ioctl(i2c_fd, I2C_SLAVE, INA219_ADDRESS) < 0) {
        perror("Failed to set I2C address");
        close(i2c_fd);
        return -1;
    }
    
    // Configure for 32V, 2A
    uint16_t config = INA219_CONFIG_BVOLTAGERANGE_32V |
                      INA219_CONFIG_GAIN_8_320MV |
                      INA219_CONFIG_BADCRES_12BIT |
                      INA219_CONFIG_SADCRES_12BIT_1S |
                      INA219_CONFIG_MODE_SANDBVOLT_CONT;
    
    if (ina219_write16(INA219_REG_CONFIG, config) < 0) {
        return -1;
    }
    
    // Set calibration for 32V, 2A
    // Calibration = 0.04096 / (current_lsb * r_shunt)
    // For 2A max with 0.1 ohm shunt: cal = 4096
    current_lsb = 0.0001;  // 100uA per bit
    power_lsb = 0.002;     // 2mW per bit
    
    if (ina219_write16(INA219_REG_CALIBRATION, 4096) < 0) {
        return -1;
    }
    
    printf("INA219 initialized successfully\n");
    return 0;
}

// Get bus voltage in volts
float ina219_getBusVoltage_V(void) {
    int16_t value = ina219_read16(INA219_REG_BUSVOLTAGE);
    // Shift right 3 bits, multiply by 4mV
    return ((value >> 3) * 4) * 0.001;
}

// Get shunt voltage in millivolts
float ina219_getShuntVoltage_mV(void) {
    int16_t value = ina219_read16(INA219_REG_SHUNTVOLTAGE);
    return value * 0.01;  // 10uV per bit
}

// Get current in milliamps
float ina219_getCurrent_mA(void) {
    // Re-write calibration to ensure it's set
    ina219_write16(INA219_REG_CALIBRATION, 4096);
    
    int16_t value = ina219_read16(INA219_REG_CURRENT);
    return value * current_lsb * 1000;
}

// Get power in milliwatts
float ina219_getPower_mW(void) {
    int16_t value = ina219_read16(INA219_REG_POWER);
    return value * power_lsb * 1000;
}

// Close I2C connection
void ina219_close(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
}

// Main function
int main(int argc, char *argv[]) {
    // Use /dev/i2c-3 or appropriate I2C bus on Luckfox
    // Check your Luckfox documentation for correct bus number
    const char *i2c_dev = "/dev/i2c-3";
    
    if (argc > 1) {
        i2c_dev = argv[1];
    }
    
    printf("Using I2C device: %s\n", i2c_dev);
    
    if (ina219_init(i2c_dev) < 0) {
        fprintf(stderr, "Failed to initialize INA219\n");
        return 1;
    }
    
    printf("\nReading INA219 sensor...\n");
    printf("Press Ctrl+C to exit\n\n");
    
    while (1) {
        float bus_voltage = ina219_getBusVoltage_V();
        float shunt_voltage = ina219_getShuntVoltage_mV();
        float current = ina219_getCurrent_mA();
        float power = ina219_getPower_mW();
        float load_voltage = bus_voltage + (shunt_voltage / 1000);
        
        printf("Bus Voltage:    %.3f V\n", bus_voltage);
        printf("Shunt Voltage: %.3f mV\n", shunt_voltage);
        printf("Load Voltage:  %.3f V\n", load_voltage);
        printf("Current:       %.3f mA\n", current);
        printf("Power:         %.3f mW\n", power);
        printf("--------------------\n");
        
        sleep(1);  // Read every 1 second
    }
    
    ina219_close();
    return 0;
}
