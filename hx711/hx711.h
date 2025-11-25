/**
 *
 * HX711 library for Embedded Linux
 *
 * Adapted from the Arduino library by Bogdan Necula
 * https://github.com/bogde/HX711
 *
 * MIT License
 *
 */
#ifndef HX711_H
#define HX711_H

#include <stdint.h>
#include <stdbool.h>

// Define a function pointer type for GPIO operations and delays
typedef void (*gpio_write_func)(int pin, int value);
typedef int (*gpio_read_func)(int pin);
typedef void (*delay_us_func)(unsigned int us);
typedef void (*delay_ms_func)(unsigned int ms);

// Main struct to hold HX711 state and configuration
typedef struct {
    int dout_pin;
    int sck_pin;
    uint8_t gain;
    long offset;
    float scale;

    // Pointers to hardware-specific functions
    gpio_write_func gpio_write;
    gpio_read_func gpio_read;
    delay_us_func delay_us;
    delay_ms_func delay_ms;

} hx711_t;

/**
 * @brief Initializes the HX711 struct with pin numbers and function pointers.
 *
 * @param hx Pointer to the hx711_t struct to initialize.
 * @param dout_pin GPIO pin number for DOUT.
 * @param sck_pin GPIO pin number for PD_SCK.
 * @param write_func Pointer to a function for writing to a GPIO pin.
 * @param read_func Pointer to a function for reading from a GPIO pin.
 * @param delay_us_func Pointer to a function for delaying in microseconds.
 * @param delay_ms_func Pointer to a function for delaying in milliseconds.
 */
void hx711_init(hx711_t* hx, int dout_pin, int sck_pin,
                gpio_write_func write_func, gpio_read_func read_func,
                delay_us_func us_func, delay_ms_func ms_func);

/**
 * @brief Check if HX711 is ready.
 * @param hx Pointer to the initialized hx711_t struct.
 * @return True if data is ready, false otherwise.
 */
bool hx711_is_ready(hx711_t* hx);

/**
 * @brief Set the gain factor.
 * @param hx Pointer to the initialized hx711_t struct.
 * @param gain Gain factor. Can be 128, 64, or 32.
 */
void hx711_set_gain(hx711_t* hx, uint8_t gain);

/**
 * @brief Waits for the chip to be ready and returns a reading.
 * @param hx Pointer to the initialized hx711_t struct.
 * @return The raw 24-bit value from the HX711.
 */
long hx711_read(hx711_t* hx);

/**
 * @brief Returns an average of multiple readings.
 * @param hx Pointer to the initialized hx711_t struct.
 * @param times Number of readings to average.
 * @return The average raw value.
 */
long hx711_read_average(hx711_t* hx, uint8_t times);

/**
 * @brief Get the current value minus the tare weight.
 * @param hx Pointer to the initialized hx711_t struct.
 * @param times Number of readings to average.
 * @return The value with offset subtracted.
 */
double hx711_get_value(hx711_t* hx, uint8_t times);

/**
 * @brief Get the weight in calibrated units.
 * @param hx Pointer to the initialized hx711_t struct.
 * @param times Number of readings to average.
 * @return The calibrated weight.
 */
float hx711_get_units(hx711_t* hx, uint8_t times);

/**
 * @brief Set the tare offset by reading the current value.
 * @param hx Pointer to the initialized hx711_t struct.
 * @param times Number of readings to average for the tare value.
 */
void hx711_tare(hx711_t* hx, uint8_t times);

/**
 * @brief Set the calibration scale factor.
 * @param hx Pointer to the initialized hx711_t struct.
 * @param scale The scale factor.
 */
void hx711_set_scale(hx711_t* hx, float scale);

/**
 * @brief Get the current scale factor.
 * @param hx Pointer to the initialized hx711_t struct.
 * @return The current scale factor.
 */
float hx711_get_scale(hx711_t* hx);

/**
 * @brief Set the offset (tare) value manually.
 * @param hx Pointer to the initialized hx711_t struct.
 * @param offset The offset value.
 */
void hx711_set_offset(hx711_t* hx, long offset);

/**
 * @brief Get the current offset value.
 * @param hx Pointer to the initialized hx711_t struct.
 * @return The current offset.
 */
long hx711_get_offset(hx711_t* hx);

/**
 * @brief Puts the chip into power down mode.
 * @param hx Pointer to the initialized hx711_t struct.
 */
void hx711_power_down(hx711_t* hx);

/**
 * @brief Wakes up the chip from power down mode.
 * @param hx Pointer to the initialized hx711_t struct.
 */
void hx711_power_up(hx711_t* hx);

#endif /* HX711_H */
