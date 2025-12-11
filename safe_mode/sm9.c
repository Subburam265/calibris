#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/wait.h>
#include <gpiod.h>

// --- Configuration ---
#define CONFIG_FILE "/home/pico/calibris/data/config.json"
#define I2C_BUS_DEVICE "/dev/i2c-3"
#define I2C_ADDR_DEFAULT 0x27
#define MW7_SERVICE "measure_weight.service"
#define SAFE_MODE_SERVICE "safe_mode.service"

// --- GPIO Configuration (Bank 1) ---
#define GPIO_CHIP "gpiochip1"
// GPIO1_C1 (Pin 14) -> Offset 17
// GPIO1_C2 (Pin 15) -> Offset 18
// GPIO1_C3 (Pin 16) -> Offset 19
// GPIO1_C6 (Pin 5)  -> Offset 22 (PWM)
#define PIN_ENTER 17
#define PIN_DECR  18
#define PIN_INCR  19
#define PIN_PWM   22 

// --- TOTP Config ---
#define MASTER_SECRET "MY_SUPER_SECRET_COMPANY_MASTER_KEY"
#define TIME_STEP 60
#define TOKEN_VALIDITY_WINDOW 1

// ==========================================
//   USER PROVIDED LCD DRIVER IMPLEMENTATION
// ==========================================

// --- LCD Command Defines ---
#define LCD_CHR 1 // Mode - Sending data
#define LCD_CMD 0 // Mode - Sending command

#define LINE1 0x80 // LCD RAM address for the 1st line
#define LINE2 0xC0 // LCD RAM address for the 2nd line

#define LCD_BACKLIGHT 0x08  // On
#define ENABLE 0b00000100   // Enable bit

// Global variables for LCD
static int i2c_file = -1;

// --- Private LCD Functions ---
void lcd_toggle_enable(int bits) {
    usleep(500);
    write(i2c_file, (unsigned char[]){(bits | ENABLE)}, 1);
    usleep(500);
    write(i2c_file, (unsigned char[]){(bits & ~ENABLE)}, 1);
    usleep(500);
}

void lcd_send_byte(int bits, int mode) {
    int bits_high = mode | (bits & 0xF0) | LCD_BACKLIGHT;
    int bits_low = mode | ((bits << 4) & 0xF0) | LCD_BACKLIGHT;

    write(i2c_file, (unsigned char[]){bits_high}, 1);
    lcd_toggle_enable(bits_high);

    write(i2c_file, (unsigned char[]){bits_low}, 1);
    lcd_toggle_enable(bits_low);
}

// --- Public LCD Functions ---
int lcd_init(const char* i2c_bus, int i2c_addr) {
    i2c_file = open(i2c_bus, O_RDWR);
    if (i2c_file < 0) {
        perror("Failed to open the i2c bus");
        return -1;
    }

    if (ioctl(i2c_file, I2C_SLAVE, i2c_addr) < 0) {
        perror("Failed to acquire bus access and/or talk to slave");
        close(i2c_file);
        return -1;
    }

    lcd_send_byte(0x33, LCD_CMD);
    lcd_send_byte(0x32, LCD_CMD);
    lcd_send_byte(0x06, LCD_CMD);
    lcd_send_byte(0x0C, LCD_CMD);
    lcd_send_byte(0x28, LCD_CMD);
    lcd_send_byte(0x01, LCD_CMD);
    usleep(2000);
    usleep(5000);
    return 0;
}

void lcd_clear() {
    lcd_send_byte(0x01, LCD_CMD);
    usleep(2000);
}

void lcd_set_cursor(int row, int col) {
    int row_offsets[] = {LINE1, LINE2};
    if (row >= 0 && row < 2) {
        lcd_send_byte(row_offsets[row] + col, LCD_CMD);
    }
}

void lcd_send_string(const char *str) {
    while (*str) {
        lcd_send_byte(*(str++), LCD_CHR);
    }
}

void lcd_close() {
    if (i2c_file >= 0) {
        close(i2c_file);
        i2c_file = -1;
    }
}

void lcd_command(int cmd) {
    lcd_send_byte(cmd, LCD_CMD);
}
void lcd_data(int data) {
    lcd_send_byte(data, LCD_CHR);
}

// ==========================================
//        END LCD DRIVER IMPLEMENTATION
// ==========================================

// --- Global Vars for App ---
char device_id[64] = "";
struct gpiod_chip *chip = NULL;
struct gpiod_line *line_enter = NULL;
struct gpiod_line *line_decr = NULL;
struct gpiod_line *line_incr = NULL;
struct gpiod_line *line_pwm = NULL; 

// --- HMAC-SHA1 Implementation ---
#define SHA1_BLOCK_SIZE 64
#define SHA1_DIGEST_SIZE 20
typedef struct { uint32_t state[5]; uint32_t count[2]; uint8_t buffer[64]; } SHA1_CTX;
#define ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))
void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e, block[16];
    memcpy(block, buffer, 64);
    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    #define BLK0(i) (block[i] = (block[i] << 24) | ((block[i] << 8) & 0xFF0000) | ((block[i] >> 8) & 0xFF00) | (block[i] >> 24))
    #define BLK(i) (block[i & 15] = ROL(block[(i + 13) & 15] ^ block[(i + 8) & 15] ^ block[(i + 2) & 15] ^ block[i & 15], 1))
    #define R0(v,w,x,y,z,i) z += ((w&(x^y))^y) + BLK0(i) + 0x5A827999 + ROL(v,5); w=ROL(w,30);
    #define R1(v,w,x,y,z,i) z += ((w&(x^y))^y) + BLK(i) + 0x5A827999 + ROL(v,5); w=ROL(w,30);
    #define R2(v,w,x,y,z,i) z += (w^x^y) + BLK(i) + 0x6ED9EBA1 + ROL(v,5); w=ROL(w,30);
    #define R3(v,w,x,y,z,i) z += (((w|x)&y)|(w&x)) + BLK(i) + 0x8F1BBCDC + ROL(v,5); w=ROL(w,30);
    #define R4(v,w,x,y,z,i) z += (w^x^y) + BLK(i) + 0xCA62C1D6 + ROL(v,5); w=ROL(w,30);
    R0(a,b,c,d,e,0); R0(e,a,b,c,d,1); R0(d,e,a,b,c,2); R0(c,d,e,a,b,3);
    R0(b,c,d,e,a,4); R0(a,b,c,d,e,5); R0(e,a,b,c,d,6); R0(d,e,a,b,c,7);
    R0(c,d,e,a,b,8); R0(b,c,d,e,a,9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
    R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
    R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
    R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
    R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
    R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
    R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
    R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
    R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
    R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
    R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
    R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
    R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
    R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
    R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
    R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
    R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
    R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e;
}
void sha1_init(SHA1_CTX *ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89; ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476; ctx->state[4] = 0xC3D2E1F0; ctx->count[0] = ctx->count[1] = 0;
}
void sha1_update(SHA1_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i, j; j = (ctx->count[0] >> 3) & 63;
    if ((ctx->count[0] += len << 3) < (len << 3)) ctx->count[1]++;
    ctx->count[1] += (len >> 29);
    if ((j + len) > 63) { memcpy(&ctx->buffer[j], data, (i = 64 - j)); sha1_transform(ctx->state, ctx->buffer); for (; i + 63 < len; i += 64) sha1_transform(ctx->state, &data[i]); j = 0; } else i = 0;
    memcpy(&ctx->buffer[j], &data[i], len - i);
}
void sha1_final(uint8_t digest[SHA1_DIGEST_SIZE], SHA1_CTX *ctx) {
    uint8_t finalcount[8], c = 0200; for (int i=0; i<8; i++) finalcount[i] = (uint8_t)((ctx->count[(i>=4?0:1)] >> ((3-(i&3))*8)) & 255);
    sha1_update(ctx, &c, 1); while ((ctx->count[0] & 504) != 448) { c=0000; sha1_update(ctx, &c, 1); }
    sha1_update(ctx, finalcount, 8); for (int i=0; i<SHA1_DIGEST_SIZE; i++) digest[i] = (uint8_t)((ctx->state[i>>2] >> ((3-(i&3))*8)) & 255);
}
void hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *digest) {
    uint8_t k_ipad[SHA1_BLOCK_SIZE], k_opad[SHA1_BLOCK_SIZE], tk[SHA1_DIGEST_SIZE], inner[SHA1_DIGEST_SIZE];
    if (key_len > SHA1_BLOCK_SIZE) { SHA1_CTX ctx; sha1_init(&ctx); sha1_update(&ctx, key, key_len); sha1_final(tk, &ctx); key = tk; key_len = SHA1_DIGEST_SIZE; }
    memset(k_ipad, 0x36, SHA1_BLOCK_SIZE); memset(k_opad, 0x5c, SHA1_BLOCK_SIZE);
    for (size_t i=0; i<key_len; i++) { k_ipad[i] ^= key[i]; k_opad[i] ^= key[i]; }
    SHA1_CTX ctx; sha1_init(&ctx); sha1_update(&ctx, k_ipad, SHA1_BLOCK_SIZE); sha1_update(&ctx, data, data_len); sha1_final(inner, &ctx);
    sha1_init(&ctx); sha1_update(&ctx, k_opad, SHA1_BLOCK_SIZE); sha1_update(&ctx, inner, SHA1_DIGEST_SIZE); sha1_final(digest, &ctx);
}

// --- SHA256 (Condensed) ---
#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32
typedef struct { uint32_t state[8]; uint64_t count; uint8_t buffer[64]; } SHA256_CTX;
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
void sha256_transform(SHA256_CTX *ctx, const uint8_t data[64]) {
    uint32_t a,b,c,d,e,f,g,h,t1,t2,m[64]; int i;
    for(i=0;i<16;i++) m[i]=(data[i*4]<<24)|(data[i*4+1]<<16)|(data[i*4+2]<<8)|(data[i*4+3]);
    for(;i<64;i++) m[i]= (ROR32(m[i-2],17)^ROR32(m[i-2],19)^(m[i-2]>>10)) + m[i-7] + (ROR32(m[i-15],7)^ROR32(m[i-15],18)^(m[i-15]>>3)) + m[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3]; e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for(i=0;i<64;i++) {
        t1=h+(ROR32(e,6)^ROR32(e,11)^ROR32(e,25))+((e&f)^(~e&g))+K256[i]+m[i];
        t2=(ROR32(a,2)^ROR32(a,13)^ROR32(a,22))+((a&b)^(a&c)^(b&c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d; ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}
void sha256_init(SHA256_CTX *ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19; ctx->count=0;
}
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i; for(i=0; i<len; i++) { ctx->buffer[ctx->count%64]=data[i]; ctx->count++; if(ctx->count%64==0) sha256_transform(ctx,ctx->buffer); }
}
void sha256_final(SHA256_CTX *ctx, uint8_t hash[32]) {
    uint64_t bits=ctx->count*8; size_t pad=(ctx->count%64<56)?(56-ctx->count%64):(120-ctx->count%64); uint8_t padbuf[128]={0x80};
    sha256_update(ctx, padbuf, pad); uint8_t len[8]; for(int i=0;i<8;i++) len[i]=(bits>>(56-i*8))&0xff;
    sha256_update(ctx, len, 8); for(int i=0;i<8;i++) { hash[i*4]=(ctx->state[i]>>24)&0xff; hash[i*4+1]=(ctx->state[i]>>16)&0xff; hash[i*4+2]=(ctx->state[i]>>8)&0xff; hash[i*4+3]=ctx->state[i]&0xff; }
}
void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *digest) {
    uint8_t k_ipad[SHA256_BLOCK_SIZE], k_opad[SHA256_BLOCK_SIZE], tk[SHA256_DIGEST_SIZE], inner[SHA256_DIGEST_SIZE];
    if (key_len > SHA256_BLOCK_SIZE) { SHA256_CTX ctx; sha256_init(&ctx); sha256_update(&ctx,key,key_len); sha256_final(&ctx, tk); key=tk; key_len=SHA256_DIGEST_SIZE; }
    memset(k_ipad,0x36,SHA256_BLOCK_SIZE); memset(k_opad,0x5c,SHA256_BLOCK_SIZE);
    for(size_t i=0;i<key_len;i++) { k_ipad[i]^=key[i]; k_opad[i]^=key[i]; }
    SHA256_CTX ctx; sha256_init(&ctx); sha256_update(&ctx,k_ipad,SHA256_BLOCK_SIZE); sha256_update(&ctx,data,data_len); sha256_final(&ctx, inner);
    sha256_init(&ctx); sha256_update(&ctx,k_opad,SHA256_BLOCK_SIZE); sha256_update(&ctx,inner,SHA256_DIGEST_SIZE); sha256_final(&ctx, digest);
}

// --- TOTP Logic ---
void get_device_key(const char *product_id, uint8_t device_key[20]) {
    uint8_t full_hash[SHA256_DIGEST_SIZE];
    hmac_sha256((const uint8_t *)MASTER_SECRET, strlen(MASTER_SECRET), (const uint8_t *)product_id, strlen(product_id), full_hash);
    memcpy(device_key, full_hash, 20);
}
uint32_t generate_totp(const char *product_id, uint64_t counter) {
    uint8_t device_key[20], digest[SHA1_DIGEST_SIZE], counter_bytes[8];
    get_device_key(product_id, device_key);
    for (int i=7; i>=0; i--) { counter_bytes[i] = counter & 0xff; counter >>= 8; }
    hmac_sha1(device_key, 20, counter_bytes, 8, digest);
    int offset = digest[SHA1_DIGEST_SIZE - 1] & 0x0f;
    uint32_t code = ((digest[offset]&0x7f)<<24)|((digest[offset+1]&0xff)<<16)|((digest[offset+2]&0xff)<<8)|(digest[offset+3]&0xff);
    return code % 1000000;
}
int verify_totp(const char *product_id, const char *token_str) {
    uint64_t current = time(NULL) / TIME_STEP;
    uint32_t input = (uint32_t)atoi(token_str);
    for (int i = -TOKEN_VALIDITY_WINDOW; i <= TOKEN_VALIDITY_WINDOW; i++) {
        if (generate_totp(product_id, current + i) == input) return 1;
    }
    return 0;
}

// --- GPIO Functions ---
int gpio_init() {
    chip = gpiod_chip_open_by_name(GPIO_CHIP);
    if (!chip) { perror("GPIO Chip"); return -1; }

    line_enter = gpiod_chip_get_line(chip, PIN_ENTER);
    line_decr  = gpiod_chip_get_line(chip, PIN_DECR);
    line_incr  = gpiod_chip_get_line(chip, PIN_INCR);
    line_pwm   = gpiod_chip_get_line(chip, PIN_PWM); // Init new pin

    if (!line_enter || !line_decr || !line_incr || !line_pwm) { fprintf(stderr, "Failed to get lines\n"); return -1; }

    if (gpiod_line_request_input(line_enter, "sm_enter") < 0 ||
        gpiod_line_request_input(line_decr, "sm_decr") < 0 ||
        gpiod_line_request_input(line_incr, "sm_incr") < 0) {
        perror("Request input"); return -1;
    }

    // Request PWM pin as OUTPUT
    if (gpiod_line_request_output(line_pwm, "sm_pwm", 0) < 0) {
        perror("Request PWM output failed"); return -1;
    }

    return 0;
}
void gpio_close() {
    // Turn off PWM before closing - ENSURE LOW (0)
    if (line_pwm) { gpiod_line_set_value(line_pwm, 0); gpiod_line_release(line_pwm); }
    if (line_enter) gpiod_line_release(line_enter);
    if (line_decr)  gpiod_line_release(line_decr);
    if (line_incr)  gpiod_line_release(line_incr);
    if (chip)       gpiod_chip_close(chip);
}
int read_enter() { return gpiod_line_get_value(line_enter); }
int read_decr()  { return gpiod_line_get_value(line_decr); }
int read_incr()  { return gpiod_line_get_value(line_incr); }

// --- System/Config Ops ---
char* skip_whitespace(char* s) { while(*s && isspace((unsigned char)*s)) s++; return s; }
int check_safe_mode() {
    FILE *fp = fopen(CONFIG_FILE, "r"); if (!fp) return 0;
    char buf[1024]; size_t len = fread(buf, 1, 1023, fp); fclose(fp); buf[len] = 0;
    char *p = strstr(buf, "\"safe_mode\"");
    if (p) { p = strchr(p, ':'); if(p && strncmp(skip_whitespace(p+1), "true", 4)==0) return 1; }
    return 0;
}
int load_device_id() {
    FILE *fp = fopen(CONFIG_FILE, "r"); if (!fp) return -1;
    char buf[1024]; size_t len = fread(buf, 1, 1023, fp); fclose(fp); buf[len] = 0;
    char *p = strstr(buf, "\"device_id\""); if(!p) return -1;
    p = strchr(p, ':'); if(!p) return -1; p = skip_whitespace(p+1);
    if (*p == '"') { p++; char *e = strchr(p, '"'); if(e) { strncpy(device_id, p, e-p); device_id[e-p]=0; } }
    else { char *e = p; while(isdigit(*e)) e++; strncpy(device_id, p, e-p); device_id[e-p]=0; }
    printf("ID: %s\n", device_id); return 0;
}
int update_config(int mode) {
    FILE *fp = fopen(CONFIG_FILE, "r"); if(!fp) return -1;
    fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    char *dat=malloc(sz+1); fread(dat,1,sz,fp); fclose(fp); dat[sz]=0;
    char *p = strstr(dat, "\"safe_mode\""); if(!p) { free(dat); return -1; }
    char *val = skip_whitespace(strchr(p,':')+1);
    int old_len = (strncmp(val,"true",4)==0)?4:5;
    char *new_dat = malloc(sz+10);
    strncpy(new_dat, dat, val-dat); new_dat[val-dat]=0;
    strcat(new_dat, mode?"true":"false");
    strcat(new_dat, val+old_len);
    fp = fopen(CONFIG_FILE, "w"); if(fp) { fputs(new_dat, fp); fclose(fp); }
    free(dat); free(new_dat); return 0;
}

// --- Exit Procedures ---
int exit_safe_mode() {
    update_config(0);
    pid_t pid = fork();
    if(pid==0) { execl("/usr/bin/sudo", "sudo", "systemctl", "disable", SAFE_MODE_SERVICE, NULL); exit(1); }
    wait(NULL);
    pid = fork();
    if(pid==0) { execl("/usr/bin/sudo", "sudo", "systemctl", "enable", "--now", MW7_SERVICE, NULL); exit(1); }
    wait(NULL);
    return 0;
}

void cleanup(int s) { lcd_clear(); lcd_send_string("Shutting Down"); gpio_close(); lcd_close(); exit(0); }

// --- Main State Machine ---
int main() {
    signal(SIGINT, cleanup); signal(SIGTERM, cleanup);

    if (load_device_id() != 0 || !check_safe_mode()) return 0;

    // LCD Init with your custom driver
    if (lcd_init(I2C_BUS_DEVICE, I2C_ADDR_DEFAULT) != 0) {
        fprintf(stderr, "LCD Init Failed\n");
        return 1;
    }

    if (gpio_init() != 0) return 1;

    enum { STATE_IDLE, STATE_TOKEN } state = STATE_IDLE;
    char token[7] = "000000";
    int digit_idx = 0;
    int current_val = 0;

    int last_ent = 0, last_inc = 0, last_dec = 0;
    
    // PWM State Variables
    int pwm_counter = 0;
    int pwm_state = 0;

    lcd_clear();
    lcd_send_string("** SAFE MODE **");
    // User driver: set_cursor(row, col)
    lcd_set_cursor(1, 0);
    lcd_send_string("Press Enter...");

    while(1) {
        int ent = read_enter();
        int inc = read_incr();
        int dec = read_decr();
        
        // --- PWM Logic (0.5 duty ratio) ---
        // Loop delay is 50ms.
        // Toggle every 5 counts = 250ms high, 250ms low (2Hz)
        pwm_counter++;
        if (pwm_counter >= 5) {
            pwm_state = !pwm_state;
            gpiod_line_set_value(line_pwm, pwm_state);
            pwm_counter = 0;
        }

        if (state == STATE_IDLE) {
            if (inc && !last_inc) {
                lcd_clear(); lcd_send_string("DEV BYPASS");
                lcd_set_cursor(1, 0); lcd_send_string("Access Granted");
                usleep(2000000);
                
                // === FIX: Ensure PWM LOW before Exit ===
                gpiod_line_set_value(line_pwm, 0);
                gpio_close();
                // =======================================
                
                exit_safe_mode();
                return 0;
            }
            if (ent && !last_ent) {
                state = STATE_TOKEN;
                digit_idx = 0;
                current_val = 0;
                memset(token, '0', 6); token[6]=0;
                lcd_clear(); lcd_send_string("Enter Token:");
                lcd_set_cursor(1, 0); lcd_send_string("0_____");
                lcd_set_cursor(1, 0);
                lcd_command(0x0F); // Blink cursor ON
            }
        }
        else if (state == STATE_TOKEN) {
            int update_screen = 0;

            if (inc && !last_inc) {
                current_val++;
                if (current_val > 9) current_val = 0;
                token[digit_idx] = current_val + '0';
                update_screen = 1;
            }

            if (dec && !last_dec) {
                current_val--;
                if (current_val < 0) current_val = 9;
                token[digit_idx] = current_val + '0';
                update_screen = 1;
            }

            if (update_screen) {
                lcd_set_cursor(1, digit_idx);
                lcd_data(token[digit_idx]);
                lcd_set_cursor(1, digit_idx);
            }

            if (ent && !last_ent) {
                digit_idx++;
                if (digit_idx < 6) {
                    current_val = 0;
                    token[digit_idx] = '0';
                    lcd_set_cursor(1, digit_idx);
                    lcd_data('0');
                    lcd_set_cursor(1, digit_idx);
                } else {
                    lcd_command(0x0C); // Cursor OFF
                    lcd_clear(); lcd_send_string("Verifying...");
                    usleep(500000);

                    if (verify_totp(device_id, token)) {
                        lcd_set_cursor(1, 0); lcd_send_string("Success!");
                        usleep(1500000);
                        
                        // === FIX: Ensure PWM LOW before Exit ===
                        gpiod_line_set_value(line_pwm, 0);
                        gpio_close();
                        // =======================================
                        
                        exit_safe_mode();
                        return 0;
                    } else {
                        lcd_set_cursor(1, 0); lcd_send_string("Invalid Token");
                        usleep(2000000);
                        state = STATE_IDLE;
                        lcd_clear(); lcd_send_string("** SAFE MODE **");
                        lcd_set_cursor(1, 0); lcd_send_string("Press Enter...");
                    }
                }
            }
        }
        last_ent = ent; last_inc = inc; last_dec = dec;
        usleep(50000); // 50ms loop delay
    }
    return 0;
}
