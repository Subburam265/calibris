/**
 * Safe Mode Activator for Calibris
 * 
 * A standalone program that activates safe mode by:
 * 1. Setting safe_mode = true in config.json
 * 2. Stopping and disabling measure_weight.service
 * 3. Enabling and starting safe_mode.service
 * 
 * Usage:
 *   ./safe_mode_activator                    # Use default config path
 *   ./safe_mode_activator /path/to/config.json
 * 
 * Exit codes:
 *   0 = Success
 *   1 = Failed to update config
 *   2 = Failed to stop measure_weight.service
 *   3 = Failed to start safe_mode.service
 * 
 * Compile:
 *   gcc -o safe_mode_activator safe_mode_activator.c
 * 
 * Install (optional):
 *   sudo cp safe_mode_activator /usr/local/bin/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>

// --- Default Configuration ---
#define DEFAULT_CONFIG_PATH "/home/pico/calibris/data/config.json"
#define MEASURE_WEIGHT_SERVICE "measure_weight.service"
#define SAFE_MODE_SERVICE "safe_mode.service"

// --- Exit Codes ---
#define EXIT_SUCCESS_CODE 0
#define EXIT_CONFIG_FAILED 1
#define EXIT_STOP_SERVICE_FAILED 2
#define EXIT_START_SERVICE_FAILED 3

// --- Function: Update safe_mode in config.json ---
int update_config_safe_mode(const char *filepath, bool enable_safe_mode) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        perror("[safe_mode] Failed to open config file for reading");
        return -1;
    }

    // Read entire file into memory
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *content = malloc(fsize + 128); // Extra space for potential expansion
    if (!content) {
        perror("[safe_mode] Memory allocation failed");
        fclose(fp);
        return -1;
    }

    size_t bytes_read = fread(content, 1, fsize, fp);
    content[bytes_read] = '\0';
    fclose(fp);

    // Find and replace safe_mode value
    char *pos = strstr(content, "\"safe_mode\"");
    if (! pos) {
        fprintf(stderr, "[safe_mode] Error: 'safe_mode' key not found in config\n");
        free(content);
        return -1;
    }

    // Find the colon after "safe_mode"
    char *colon = strchr(pos, ':');
    if (!colon) {
        fprintf(stderr, "[safe_mode] Error: Invalid JSON format\n");
        free(content);
        return -1;
    }

    // Find the value (true or false)
    char *value_start = colon + 1;
    while (*value_start == ' ' || *value_start == '\t') value_start++;

    // Determine current value and its length
    bool current_value;
    int old_len;
    if (strncmp(value_start, "true", 4) == 0) {
        current_value = true;
        old_len = 4;
    } else if (strncmp(value_start, "false", 5) == 0) {
        current_value = false;
        old_len = 5;
    } else {
        fprintf(stderr, "[safe_mode] Error: Invalid safe_mode value in config\n");
        free(content);
        return -1;
    }

    // Check if already in desired state
    if (current_value == enable_safe_mode) {
        printf("[safe_mode] Config already has safe_mode = %s\n", 
               enable_safe_mode ?  "true" : "false");
        free(content);
        return 0;
    }

    // Build new content with replaced value
    const char *new_value = enable_safe_mode ? "true" : "false";
    int new_len = enable_safe_mode ? 4 : 5;

    size_t prefix_len = value_start - content;
    size_t suffix_len = strlen(value_start + old_len);

    char *new_content = malloc(prefix_len + new_len + suffix_len + 1);
    if (!new_content) {
        perror("[safe_mode] Memory allocation failed");
        free(content);
        return -1;
    }

    memcpy(new_content, content, prefix_len);
    memcpy(new_content + prefix_len, new_value, new_len);
    memcpy(new_content + prefix_len + new_len, value_start + old_len, suffix_len + 1);

    // Write back to file
    fp = fopen(filepath, "w");
    if (!fp) {
        perror("[safe_mode] Failed to open config file for writing");
        free(content);
        free(new_content);
        return -1;
    }

    fputs(new_content, fp);
    fclose(fp);

    printf("[safe_mode] Config updated: safe_mode = %s\n", new_value);

    free(content);
    free(new_content);
    return 0;
}

// --- Function: Execute systemctl command ---
int run_systemctl(const char *action, const char *service) {
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("[safe_mode] fork failed");
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        execl("/usr/bin/systemctl", "systemctl", action, service, (char *)NULL);
        // If execl returns, it failed
        perror("[safe_mode] execl failed");
        _exit(127);
    }
    
    // Parent process - wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[safe_mode] waitpid failed");
        return -1;
    }
    
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    
    return -1;
}

// --- Function: Stop and disable a service ---
int stop_and_disable_service(const char *service) {
    printf("[safe_mode] Stopping %s.. .\n", service);
    
    int rc = run_systemctl("stop", service);
    if (rc != 0) {
        fprintf(stderr, "[safe_mode] Warning: Failed to stop %s (exit code: %d)\n", service, rc);
        // Continue anyway - service might not be running
    } else {
        printf("[safe_mode] Stopped %s\n", service);
    }
    
    printf("[safe_mode] Disabling %s...\n", service);
    rc = run_systemctl("disable", service);
    if (rc != 0) {
        fprintf(stderr, "[safe_mode] Warning: Failed to disable %s (exit code: %d)\n", service, rc);
    } else {
        printf("[safe_mode] Disabled %s\n", service);
    }
    
    return 0; // Continue even if service control fails
}

// --- Function: Enable and start a service ---
int enable_and_start_service(const char *service) {
    printf("[safe_mode] Enabling %s...\n", service);
    
    int rc = run_systemctl("enable", service);
    if (rc != 0) {
        fprintf(stderr, "[safe_mode] Error: Failed to enable %s (exit code: %d)\n", service, rc);
        return -1;
    }
    printf("[safe_mode] Enabled %s\n", service);
    
    printf("[safe_mode] Starting %s...\n", service);
    rc = run_systemctl("start", service);
    if (rc != 0) {
        fprintf(stderr, "[safe_mode] Error: Failed to start %s (exit code: %d)\n", service, rc);
        return -1;
    }
    printf("[safe_mode] Started %s\n", service);
    
    return 0;
}

// --- Function: Activate safe mode (main logic) ---
int activate_safe_mode(const char *config_path) {
    printf("==========================================\n");
    printf("  Safe Mode Activator for Calibris\n");
    printf("==========================================\n\n");
    
    // Step 1: Update config.json
    printf("[Step 1/3] Updating configuration...\n");
    if (update_config_safe_mode(config_path, true) != 0) {
        fprintf(stderr, "[FAILED] Could not update config file\n");
        return EXIT_CONFIG_FAILED;
    }
    
    // Step 2: Stop and disable measure_weight.service
    printf("\n[Step 2/3] Stopping measurement service...\n");
    if (stop_and_disable_service(MEASURE_WEIGHT_SERVICE) != 0) {
        fprintf(stderr, "[FAILED] Could not stop %s\n", MEASURE_WEIGHT_SERVICE);
        return EXIT_STOP_SERVICE_FAILED;
    }
    
    // Step 3: Enable and start safe_mode.service
    printf("\n[Step 3/3] Starting safe mode service...\n");
    if (enable_and_start_service(SAFE_MODE_SERVICE) != 0) {
        fprintf(stderr, "[FAILED] Could not start %s\n", SAFE_MODE_SERVICE);
        return EXIT_START_SERVICE_FAILED;
    }
    
    printf("\n==========================================\n");
    printf("  SAFE MODE ACTIVATED SUCCESSFULLY\n");
    printf("==========================================\n");
    
    return EXIT_SUCCESS_CODE;
}

// --- Function: Print usage ---
void print_usage(const char *prog_name) {
    printf("Safe Mode Activator for Calibris\n\n");
    printf("Usage: %s [config_path]\n\n", prog_name);
    printf("Arguments:\n");
    printf("  config_path    Path to config.json (default: %s)\n\n", DEFAULT_CONFIG_PATH);
    printf("Exit codes:\n");
    printf("  0 = Success\n");
    printf("  1 = Failed to update config\n");
    printf("  2 = Failed to stop measure_weight.service\n");
    printf("  3 = Failed to start safe_mode.service\n\n");
    printf("Examples:\n");
    printf("  %s\n", prog_name);
    printf("  %s /home/pico/calibris/data/config.json\n", prog_name);
}

// --- Main ---
int main(int argc, char *argv[]) {
    const char *config_path = DEFAULT_CONFIG_PATH;
    
    // Parse arguments
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        config_path = argv[1];
    }
    
    // Check if config file exists
    if (access(config_path, F_OK) != 0) {
        fprintf(stderr, "[safe_mode] Error: Config file not found: %s\n", config_path);
        return EXIT_CONFIG_FAILED;
    }
    
    // Check if we have write permission
    if (access(config_path, W_OK) != 0) {
        fprintf(stderr, "[safe_mode] Error: No write permission for: %s\n", config_path);
        return EXIT_CONFIG_FAILED;
    }
    
    return activate_safe_mode(config_path);
}
