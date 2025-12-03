#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h> // Fixed typo: removed space before 'h'
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h> // Added for isspace()

// Configuration
#define CONFIG_FILE "/home/pico/calibris/data/config.json"
#define I2C_DEVICE "/dev/i2c-3"
#define I2C_ADDR 0x27
#define MW7_SERVICE "measure_weight.service"

// TOTP Configuration
#define MASTER_SECRET "MY_SUPER_SECRET_COMPANY_MASTER_KEY"
#define TIME_STEP 60  // Note: Standard TOTP (Google Auth) is usually 30s. Ensure server matches 60s.
#define TOKEN_VALIDITY_WINDOW 1  // Accept tokens from previous/next window too

// PCF8574 to LCD Pin Mapping
#define LCD_RS 0x01
#define LCD_RW 0x02
#define LCD_E  0x04
#define LCD_BACKLIGHT 0x08

// LCD Commands
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_FUNCTIONSET 0x20
#define LCD_SETDDRAMADDR 0x80

// Global file descriptor for the I2C bus
int i2c_fd = -1; // Initialize to -1 to indicate closed

// Terminal settings for restoring later
struct termios old_tio, new_tio;

// Device ID from config
char device_id[64] = "";

// --- HMAC-SHA1 Implementation ---

#define SHA1_BLOCK_SIZE 64
#define SHA1_DIGEST_SIZE 20

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} SHA1_CTX;

#define ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

#define BLK0(i) (block[i] = (block[i] << 24) | ((block[i] << 8) & 0xFF0000) | \
                 ((block[i] >> 8) & 0xFF00) | (block[i] >> 24))
#define BLK(i) (block[i & 15] = ROL(block[(i + 13) & 15] ^ block[(i + 8) & 15] ^ \
                block[(i + 2) & 15] ^ block[i & 15], 1))

#define R0(v, w, x, y, z, i) z += ((w & (x ^ y)) ^ y) + BLK0(i) + 0x5A827999 + ROL(v, 5); w = ROL(w, 30);
#define R1(v, w, x, y, z, i) z += ((w & (x ^ y)) ^ y) + BLK(i) + 0x5A827999 + ROL(v, 5); w = ROL(w, 30);
#define R2(v, w, x, y, z, i) z += (w ^ x ^ y) + BLK(i) + 0x6ED9EBA1 + ROL(v, 5); w = ROL(w, 30);
#define R3(v, w, x, y, z, i) z += (((w | x) & y) | (w & x)) + BLK(i) + 0x8F1BBCDC + ROL(v, 5); w = ROL(w, 30);
#define R4(v, w, x, y, z, i) z += (w ^ x ^ y) + BLK(i) + 0xCA62C1D6 + ROL(v, 5); w = ROL(w, 30);

void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e;
    uint32_t block[16];
    
    memcpy(block, buffer, 64);
    
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    
    R0(a, b, c, d, e, 0);  R0(e, a, b, c, d, 1);  R0(d, e, a, b, c, 2);  R0(c, d, e, a, b, 3);
    R0(b, c, d, e, a, 4);  R0(a, b, c, d, e, 5);  R0(e, a, b, c, d, 6);  R0(d, e, a, b, c, 7);
    R0(c, d, e, a, b, 8);  R0(b, c, d, e, a, 9);  R0(a, b, c, d, e, 10); R0(e, a, b, c, d, 11);
    R0(d, e, a, b, c, 12); R0(c, d, e, a, b, 13); R0(b, c, d, e, a, 14); R0(a, b, c, d, e, 15);
    R1(e, a, b, c, d, 16); R1(d, e, a, b, c, 17); R1(c, d, e, a, b, 18); R1(b, c, d, e, a, 19);
    R2(a, b, c, d, e, 20); R2(e, a, b, c, d, 21); R2(d, e, a, b, c, 22); R2(c, d, e, a, b, 23);
    R2(b, c, d, e, a, 24); R2(a, b, c, d, e, 25); R2(e, a, b, c, d, 26); R2(d, e, a, b, c, 27);
    R2(c, d, e, a, b, 28); R2(b, c, d, e, a, 29); R2(a, b, c, d, e, 30); R2(e, a, b, c, d, 31);
    R2(d, e, a, b, c, 32); R2(c, d, e, a, b, 33); R2(b, c, d, e, a, 34); R2(a, b, c, d, e, 35);
    R2(e, a, b, c, d, 36); R2(d, e, a, b, c, 37); R2(c, d, e, a, b, 38); R2(b, c, d, e, a, 39);
    R3(a, b, c, d, e, 40); R3(e, a, b, c, d, 41); R3(d, e, a, b, c, 42); R3(c, d, e, a, b, 43);
    R3(b, c, d, e, a, 44); R3(a, b, c, d, e, 45); R3(e, a, b, c, d, 46); R3(d, e, a, b, c, 47);
    R3(c, d, e, a, b, 48); R3(b, c, d, e, a, 49); R3(a, b, c, d, e, 50); R3(e, a, b, c, d, 51);
    R3(d, e, a, b, c, 52); R3(c, d, e, a, b, 53); R3(b, c, d, e, a, 54); R3(a, b, c, d, e, 55);
    R3(e, a, b, c, d, 56); R3(d, e, a, b, c, 57); R3(c, d, e, a, b, 58); R3(b, c, d, e, a, 59);
    R4(a, b, c, d, e, 60); R4(e, a, b, c, d, 61); R4(d, e, a, b, c, 62); R4(c, d, e, a, b, 63);
    R4(b, c, d, e, a, 64); R4(a, b, c, d, e, 65); R4(e, a, b, c, d, 66); R4(d, e, a, b, c, 67);
    R4(c, d, e, a, b, 68); R4(b, c, d, e, a, 69); R4(a, b, c, d, e, 70); R4(e, a, b, c, d, 71);
    R4(d, e, a, b, c, 72); R4(c, d, e, a, b, 73); R4(b, c, d, e, a, 74); R4(a, b, c, d, e, 75);
    R4(e, a, b, c, d, 76); R4(d, e, a, b, c, 77); R4(c, d, e, a, b, 78); R4(b, c, d, e, a, 79);
    
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_init(SHA1_CTX *context) {
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}

void sha1_update(SHA1_CTX *context, const uint8_t *data, size_t len) {
    size_t i, j;
    
    j = (context->count[0] >> 3) & 63;
    if ((context->count[0] += len << 3) < (len << 3)) {
        context->count[1]++;
    }
    context->count[1] += (len >> 29);
    
    if ((j + len) > 63) {
        memcpy(&context->buffer[j], data, (i = 64 - j));
        sha1_transform(context->state, context->buffer);
        for (; i + 63 < len; i += 64) {
            sha1_transform(context->state, &data[i]);
        }
        j = 0;
    } else {
        i = 0;
    }
    memcpy(&context->buffer[j], &data[i], len - i);
}

void sha1_final(uint8_t digest[SHA1_DIGEST_SIZE], SHA1_CTX *context) {
    uint8_t finalcount[8];
    uint8_t c;
    
    for (int i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)((context->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    
    c = 0200;
    sha1_update(context, &c, 1);
    while ((context->count[0] & 504) != 448) {
        c = 0000;
        sha1_update(context, &c, 1);
    }
    sha1_update(context, finalcount, 8);
    
    for (int i = 0; i < SHA1_DIGEST_SIZE; i++) {
        digest[i] = (uint8_t)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

void sha1(const uint8_t *data, size_t len, uint8_t digest[SHA1_DIGEST_SIZE]) {
    SHA1_CTX context;
    sha1_init(&context);
    sha1_update(&context, data, len);
    sha1_final(digest, &context);
}

// HMAC-SHA1
void hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *digest) {
    uint8_t k_ipad[SHA1_BLOCK_SIZE];
    uint8_t k_opad[SHA1_BLOCK_SIZE];
    uint8_t tk[SHA1_DIGEST_SIZE];
    uint8_t inner_digest[SHA1_DIGEST_SIZE];
    
    // If key is longer than block size, hash it first
    if (key_len > SHA1_BLOCK_SIZE) {
        sha1(key, key_len, tk);
        key = tk;
        key_len = SHA1_DIGEST_SIZE;
    }
    
    // XOR key with ipad and opad values
    memset(k_ipad, 0x36, SHA1_BLOCK_SIZE);
    memset(k_opad, 0x5c, SHA1_BLOCK_SIZE);
    
    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }
    
    // Inner hash: H(K XOR ipad, data)
    SHA1_CTX context;
    sha1_init(&context);
    sha1_update(&context, k_ipad, SHA1_BLOCK_SIZE);
    sha1_update(&context, data, data_len);
    sha1_final(inner_digest, &context);
    
    // Outer hash: H(K XOR opad, inner_hash)
    sha1_init(&context);
    sha1_update(&context, k_opad, SHA1_BLOCK_SIZE);
    sha1_update(&context, inner_digest, SHA1_DIGEST_SIZE);
    sha1_final(digest, &context);
}

// HMAC-SHA256 for device key derivation
#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} SHA256_CTX;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define EP1(x) (ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SIG0(x) (ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

void sha256_transform(SHA256_CTX *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i;
    
    for (i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
    }
    for (; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    
    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(SHA256_CTX *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if (ctx->count % 64 == 0) {
            sha256_transform(ctx, ctx->buffer);
        }
    }
}

void sha256_final(SHA256_CTX *ctx, uint8_t hash[32]) {
    uint64_t bits = ctx->count * 8;
    size_t pad_len = (ctx->count % 64 < 56) ? (56 - ctx->count % 64) : (120 - ctx->count % 64);
    uint8_t pad[128] = {0x80};
    
    sha256_update(ctx, pad, pad_len);
    
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[i] = (bits >> (56 - i * 8)) & 0xff;
    }
    sha256_update(ctx, len_be, 8);
    
    for (int i = 0; i < 8; i++) {
        hash[i * 4] = (ctx->state[i] >> 24) & 0xff;
        hash[i * 4 + 1] = (ctx->state[i] >> 16) & 0xff;
        hash[i * 4 + 2] = (ctx->state[i] >> 8) & 0xff;
        hash[i * 4 + 3] = ctx->state[i] & 0xff;
    }
}

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *digest) {
    uint8_t k_ipad[SHA256_BLOCK_SIZE];
    uint8_t k_opad[SHA256_BLOCK_SIZE];
    uint8_t tk[SHA256_DIGEST_SIZE];
    uint8_t inner_digest[SHA256_DIGEST_SIZE];
    
    if (key_len > SHA256_BLOCK_SIZE) {
        SHA256_CTX ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_len);
        sha256_final(&ctx, tk);
        key = tk;
        key_len = SHA256_DIGEST_SIZE;
    }
    
    memset(k_ipad, 0x36, SHA256_BLOCK_SIZE);
    memset(k_opad, 0x5c, SHA256_BLOCK_SIZE);
    
    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }
    
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner_digest);
    
    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, inner_digest, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, digest);
}

// --- TOTP Functions ---

// Derive device-specific key using HMAC-SHA256(MasterKey, DeviceID)
void get_device_key(const char *product_id, uint8_t device_key[20]) {
    uint8_t full_hash[SHA256_DIGEST_SIZE];
    
    hmac_sha256(
        (const uint8_t *)MASTER_SECRET, strlen(MASTER_SECRET),
        (const uint8_t *)product_id, strlen(product_id),
        full_hash
    );
    
    // Truncate to 20 bytes (standard TOTP key length)
    memcpy(device_key, full_hash, 20);
}

// Generate TOTP token for a given time counter
uint32_t generate_totp(const char *product_id, uint64_t counter) {
    uint8_t device_key[20];
    uint8_t counter_bytes[8];
    uint8_t digest[SHA1_DIGEST_SIZE];
    
    // Get device-specific key
    get_device_key(product_id, device_key);
    
    // Pack counter as big-endian 8 bytes
    for (int i = 7; i >= 0; i--) {
        counter_bytes[i] = counter & 0xff;
        counter >>= 8;
    }
    
    // HMAC-SHA1(device_key, counter_bytes)
    hmac_sha1(device_key, 20, counter_bytes, 8, digest);
    
    // Dynamic truncation (RFC 6238)
    int offset = digest[SHA1_DIGEST_SIZE - 1] & 0x0f;
    
    uint32_t code_binary = 
        ((digest[offset] & 0x7f) << 24) |
        ((digest[offset + 1] & 0xff) << 16) |
        ((digest[offset + 2] & 0xff) << 8) |
        (digest[offset + 3] & 0xff);
    
    // Modulo 1,000,000 to get 6 digits
    return code_binary % 1000000;
}

// Verify TOTP token (checks current, previous, and next time windows)
int verify_totp(const char *product_id, const char *token_str) {
    time_t current_time = time(NULL);
    uint64_t current_counter = current_time / TIME_STEP;
    
    // Convert input token string to integer
    uint32_t input_token = (uint32_t)atoi(token_str);
    
    printf("Verifying token for device: %s\n", product_id);
    printf("Current time: %ld, Counter: %llu\n", current_time, current_counter);
    
    // Check current window and adjacent windows for clock drift tolerance
    for (int i = -TOKEN_VALIDITY_WINDOW; i <= TOKEN_VALIDITY_WINDOW; i++) {
        uint64_t check_counter = current_counter + i;
        uint32_t expected_token = generate_totp(product_id, check_counter);
        
        printf("Window %d: Expected token = %06u, Input token = %06u\n", 
               i, expected_token, input_token);
        
        if (expected_token == input_token) {
            printf("Token MATCHED in window %d!\n", i);
            return 1;  // Token is valid
        }
    }
    
    printf("Token verification FAILED.\n");
    return 0;  // Token is invalid
}

// --- Terminal Functions ---

void init_terminal() {
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    new_tio.c_cc[VMIN] = 0;
    new_tio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
}

void enable_blocking_input() {
    struct termios blocking_tio = old_tio;
    blocking_tio.c_lflag &= ~ECHO; // Disable echo, but keep canonical usually, or just blocking
    // Make sure VMIN is 1 so read blocks
    blocking_tio.c_cc[VMIN] = 1;
    blocking_tio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &blocking_tio);
}

// --- LCD Functions ---

void lcd_pulse_enable(int data) {
    if (i2c_fd < 0) return;
    unsigned char buf1 = data | LCD_E;
    unsigned char buf2 = data & ~LCD_E;
    write(i2c_fd, &buf1, 1);
    usleep(500);
    write(i2c_fd, &buf2, 1);
    usleep(500);
}

void lcd_write_4bits(int data) {
    if (i2c_fd < 0) return;
    unsigned char buf = data | LCD_BACKLIGHT;
    write(i2c_fd, &buf, 1);
    lcd_pulse_enable(data | LCD_BACKLIGHT);
}

void lcd_send(int value, int mode) {
    int high_nibble = value & 0xF0;
    int low_nibble = (value << 4) & 0xF0;
    lcd_write_4bits(high_nibble | mode);
    lcd_write_4bits(low_nibble | mode);
}

void lcd_command(int cmd) {
    lcd_send(cmd, 0);
}

void lcd_data(int data) {
    lcd_send(data, LCD_RS);
}

void lcd_string(const char *s) {
    while (*s) {
        lcd_data(*s++);
    }
}

void lcd_set_cursor(int col, int row) {
    int row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    lcd_command(LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

void lcd_clear() {
    lcd_command(LCD_CLEARDISPLAY);
    usleep(2000);
}

int lcd_init() {
    if ((i2c_fd = open(I2C_DEVICE, O_RDWR)) < 0) {
        fprintf(stderr, "Failed to open the i2c bus %s: %s\n", I2C_DEVICE, strerror(errno));
        return -1;
    }

    if (ioctl(i2c_fd, I2C_SLAVE, I2C_ADDR) < 0) {
        fprintf(stderr, "Failed to acquire bus access: %s\n", strerror(errno));
        close(i2c_fd);
        i2c_fd = -1;
        return -1;
    }

    usleep(50000);
    lcd_write_4bits(0x30);
    usleep(4500);
    lcd_write_4bits(0x30);
    usleep(4500);
    lcd_write_4bits(0x30);
    usleep(150);
    lcd_write_4bits(0x20);

    lcd_command(LCD_FUNCTIONSET | 0x08);
    lcd_command(LCD_DISPLAYCONTROL | 0x04);
    lcd_command(LCD_ENTRYMODESET | 0x02);
    lcd_clear();

    return 0;
}

void lcd_close() {
    if (i2c_fd >= 0) {
        unsigned char buf = 0x00;
        write(i2c_fd, &buf, 1);
        close(i2c_fd);
        i2c_fd = -1;
    }
}

// --- Config Parsing ---

// Helper to skip whitespace
char* skip_whitespace(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

int check_safe_mode() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open config file: %s\n", CONFIG_FILE);
        return 0;
    }

    char buffer[1024];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, fp);
    fclose(fp);
    buffer[len] = '\0';

    // Improved JSON parsing (still basic, but robust against spaces)
    char *key = "\"safe_mode\"";
    char *found = strstr(buffer, key);
    
    if (found) {
        char *ptr = found + strlen(key);
        ptr = skip_whitespace(ptr);
        if (*ptr == ':') {
            ptr++;
            ptr = skip_whitespace(ptr);
            if (strncmp(ptr, "true", 4) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

int load_device_id() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open config file: %s\n", CONFIG_FILE);
        return -1;
    }

    char buffer[1024];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, fp);
    fclose(fp);
    buffer[len] = '\0';

    // Parse device_id from JSON
    char *start = strstr(buffer, "\"device_id\"");
    if (start == NULL) {
        fprintf(stderr, "device_id not found in config\n");
        return -1;
    }

    // Find the colon after "device_id"
    start += strlen("\"device_id\"");
    start = strchr(start, ':');
    if (start == NULL) return -1;
    start++;

    // Skip whitespace
    start = skip_whitespace(start);

    // Check if it's a number or a string
    if (*start == '"') {
        // String value
        start++;
        char *end = strchr(start, '"');
        if (end == NULL) return -1;
        size_t id_len = end - start;
        if (id_len >= sizeof(device_id)) id_len = sizeof(device_id) - 1;
        strncpy(device_id, start, id_len);
        device_id[id_len] = '\0';
    } else {
        // Numeric value
        char *end = start;
        while (*end >= '0' && *end <= '9') end++;
        size_t id_len = end - start;
        if (id_len >= sizeof(device_id)) id_len = sizeof(device_id) - 1;
        strncpy(device_id, start, id_len);
        device_id[id_len] = '\0';
    }

    printf("Loaded device_id: %s\n", device_id);
    return 0;
}

// --- Service Control ---

int start_measure_weight_service() {
    printf("Starting %s...\n", MW7_SERVICE);
    
    char command[256];
    // Use full path if systemctl isn't in default env PATH
    snprintf(command, sizeof(command), "/bin/systemctl start %s", MW7_SERVICE);
    
    int result = system(command);
    // system() returns waitpid status. WEXITSTATUS can check actual exit code.
    // Standard systemctl returns 0 on success.
    if (result == 0) {
        printf("Successfully started %s\n", MW7_SERVICE);
        return 0;
    } else {
        fprintf(stderr, "Failed to start %s (Exit code: %d)\n", MW7_SERVICE, result);
        return -1;
    }
}

// --- Token Verification ---

int verify_clearance_token() {
    char token[16];
    int token_index = 0;
    char c;
    
    // Display prompt on LCD
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_string("Enter Token:");
    lcd_set_cursor(0, 1);
    lcd_string("______");
    
    printf("\n\nEnter 6-digit clearance token: ");
    fflush(stdout);
    
    enable_blocking_input();
    
    token_index = 0;
    while (token_index < 6) {
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c >= '0' && c <= '9') {
                token[token_index] = c;
                token_index++;
                
                lcd_set_cursor(token_index - 1, 1);
                lcd_data('*');
                
                printf("*");
                fflush(stdout);
            }
            else if ((c == 127 || c == 8) && token_index > 0) {
                token_index--;
                lcd_set_cursor(token_index, 1);
                lcd_data('_');
                printf("\b \b");
                fflush(stdout);
            }
            else if (c == 27 || c == 3) { // ESC or Ctrl+C
                printf("\nToken entry cancelled.\n");
                init_terminal();
                return 0;
            }
        }
    }
    token[6] = '\0';
    
    printf("\n");
    
    init_terminal();
    
    // Display verifying message
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_string("Verifying...");
    
    // Verify token using TOTP
    if (verify_totp(device_id, token)) {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_string("Access Granted!");
        lcd_set_cursor(0, 1);
        lcd_string("Starting Scale..");
        
        printf("Clearance token verified! Access granted.\n");
        usleep(2000000);
        
        return 1;
    } else {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_string("Access Denied!");
        lcd_set_cursor(0, 1);
        lcd_string("Invalid Token");
        
        printf("Invalid clearance token! Access denied.\n");
        usleep(2000000);
        
        return 0;
    }
}

// --- Signal Handler ---

void cleanup_and_exit(int signum) {
    printf("\nReceived signal %d. Cleaning up...\n", signum);
    restore_terminal();
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_string("Shutting Down...");
    usleep(1000000);
    lcd_close();
    exit(0);
}

// --- Main Program ---

int main() {
    printf("Calibris Safe Mode with TOTP Authentication\n");
    printf("============================================\n");

    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    // Load device ID from config
    if (load_device_id() != 0) {
        fprintf(stderr, "Failed to load device ID from config.\n");
        return 1;
    }

    // Check if safe mode is enabled
    if (! check_safe_mode()) {
        printf("Safe mode is DISABLED. Exiting.\n");
        printf("The mw7 service should be started instead.\n");
        // Optional: Trigger service start here if safe mode is OFF?
        // start_measure_weight_service(); 
        return 0;
    }

    printf("Safe mode is ENABLED.\n");
    printf("Device ID: %s\n", device_id);
    printf("Initializing LCD...\n");

    if (lcd_init() != 0) {
        fprintf(stderr, "Failed to initialize LCD.\n");
        return 1;
    }

    init_terminal();

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_string("** SAFE MODE **");
    lcd_set_cursor(0, 1);
    lcd_string("Press ENTER...");

    printf("Safe mode active. Press ENTER to input clearance token.\n");
    printf("Token is time-based (TOTP) - valid for 1 minute.\n\n");

    char c;
    int blink_state = 0;
    int loop_counter = 0;
    
    while (1) {
        // Non-blocking read (VMIN=0)
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == '\n' || c == '\r') {
                printf("Enter key detected. Requesting clearance token...\n");
                
                if (verify_clearance_token()) {
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_string("Exiting Safe");
                    lcd_set_cursor(0, 1);
                    lcd_string("Mode...");
                    
                    if (start_measure_weight_service() == 0) {
                        printf("Successfully exited safe mode.\n");
                        
                        lcd_clear();
                        lcd_set_cursor(0, 0);
                        lcd_string("Service Started");
                        lcd_set_cursor(0, 1);
                        lcd_string("Goodbye!");
                        usleep(2000000);
                        
                        restore_terminal();
                        lcd_close();
                        return 0;
                    } else {
                        lcd_clear();
                        lcd_set_cursor(0, 0);
                        lcd_string("Service Error!");
                        lcd_set_cursor(0, 1);
                        lcd_string("Staying Safe...");
                        usleep(2000000);
                    }
                }
                
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_string("** SAFE MODE **");
                lcd_set_cursor(0, 1);
                lcd_string("Press ENTER...");
            }
        }
        
        loop_counter++;
        if (loop_counter >= 10) {
            loop_counter = 0;
            lcd_set_cursor(15, 0);
            if (blink_state) {
                lcd_data('*');
            } else {
                lcd_data(' ');
            }
            blink_state = !blink_state;
        }
        
        usleep(100000); // 100ms
    }

    restore_terminal();
    lcd_clear();
    lcd_close();

    return 0;
}
