/**
 *
 * HX711 library for Embedded Linux
 *
 * Final version with real-time scheduling (SCHED_FIFO) and signal masking
 * to achieve maximum timing stability on a preemptive OS.
 *
 */
#include "hx711.h"
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h> // <-- THE MISSING HEADER

// Initialize the HX711 struct
void hx711_init(hx711_t* hx, int dout_pin, int sck_pin,
                gpio_write_func write_func, gpio_read_func read_func,
                delay_us_func us_func, delay_ms_func ms_func) {
    hx->dout_pin = dout_pin;
    hx->sck_pin = sck_pin;
    hx->gpio_write = write_func;
    hx->gpio_read = read_func;
    hx->delay_us = us_func;
    hx->delay_ms = ms_func;
    hx->offset = 0;
    hx->scale = 1.0f;

    hx->gpio_write(hx->sck_pin, 0); // Start with clock low
    hx711_set_gain(hx, 128);
}

bool hx711_is_ready(hx711_t* hx) {
    return hx->gpio_read(hx->dout_pin) == 0;
}

void hx711_set_gain(hx711_t* hx, uint8_t gain) {
    switch (gain) {
        case 128: hx->gain = 1; break;
        case 64:  hx->gain = 3; break;
        case 32:  hx->gain = 2; break;
    }
}

long hx711_read(hx711_t* hx) {
    while (!hx711_is_ready(hx)) {
        hx->delay_ms(0);
    }

    unsigned long value = 0;
    
    // --- BEGIN CRITICAL TIMING SECTION ---
    struct sched_param old_param, new_param;
    int old_policy;
    
    // Get current scheduling policy and priority
    pthread_getschedparam(pthread_self(), &old_policy, &old_param);
    
    // Set to real-time FIFO scheduling with high priority
    memset(&new_param, 0, sizeof(new_param));
    new_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &new_param) != 0) {
        // Non-fatal error, we can still try. Might fail if not run with sudo.
    }
    
    // Block signals
    sigset_t old_mask, new_mask;
    sigfillset(&new_mask);
    pthread_sigmask(SIG_BLOCK, &new_mask, &old_mask);

    // --- Bit-banging read loop ---
    for (int i = 0; i < 24; ++i) {
        hx->gpio_write(hx->sck_pin, 1);
        hx->delay_us(1);
        value <<= 1;
        if (hx->gpio_read(hx->dout_pin)) {
            value++;
        }
        hx->gpio_write(hx->sck_pin, 0);
        hx->delay_us(1);
    }
    
    // Restore original signal mask
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    
    // Restore original scheduling policy
    sched_setscheduler(0, old_policy, &old_param);
    
    // --- END CRITICAL TIMING SECTION ---

    // Set gain for the next reading
    for (unsigned int i = 0; i < hx->gain; i++) {
        hx->gpio_write(hx->sck_pin, 1);
        hx->delay_us(1);
        hx->gpio_write(hx->sck_pin, 0);
        hx->delay_us(1);
    }

    if (value & 0x800000) {
        value |= 0xFF000000;
    }

    return (long)value;
}

long hx711_read_average(hx711_t* hx, uint8_t times) {
    long sum = 0;
    for (uint8_t i = 0; i < times; i++) {
        sum += hx711_read(hx);
        hx->delay_ms(10); // A small rest between full readings
    }
    return sum / times;
}

double hx711_get_value(hx711_t* hx, uint8_t times) {
    return hx711_read_average(hx, times) - hx->offset;
}

float hx711_get_units(hx711_t* hx, uint8_t times) {
    return (float)hx711_get_value(hx, times) / hx->scale;
}

void hx711_tare(hx711_t* hx, uint8_t times) {
    long sum = hx711_read_average(hx, times);
    hx711_set_offset(hx, sum);
}

void hx711_set_scale(hx711_t* hx, float scale) {
    hx->scale = scale;
}

float hx711_get_scale(hx711_t* hx) {
    return hx->scale;
}

void hx711_set_offset(hx711_t* hx, long offset) {
    hx->offset = offset;
}

long hx711_get_offset(hx711_t* hx) {
    return hx->offset;
}

void hx711_power_down(hx711_t* hx) {
    hx->gpio_write(hx->sck_pin, 0);
    hx->gpio_write(hx->sck_pin, 1);
}

void hx711_power_up(hx711_t* hx) {
    hx->gpio_write(hx->sck_pin, 0);
}
