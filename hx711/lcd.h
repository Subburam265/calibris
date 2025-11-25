#ifndef LCD_I2C_H
#define LCD_I2C_H

// Function prototypes
int lcd_init(const char* i2c_bus, int i2c_addr);
void lcd_send_string(const char *str);
void lcd_set_cursor(int row, int col);
void lcd_clear(void);
void lcd_close(void);

#endif // LCD_I2C_H
