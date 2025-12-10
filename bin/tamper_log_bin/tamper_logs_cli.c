/**
 * Tamper Log CLI Tool for Calibris
 *
 * Command-line interface for logging tamper events.
 * Can be called from shell scripts, cron jobs, or other programs.
 *
 * Usage:
 * tamper_log --type <tamper_type> [--details <text>] [--config <path>] [--db <path>]
 *
 * Compile: gcc -o tamper_log tamper_log_cli.c -L../lib -ltamper_log -lsqlite3 -lssl -lcrypto
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>     // Required for getopt
#include <sys/wait.h>   // Required for WIFEXITED, WEXITSTATUS
#include "../../lib/tamper_logs.h"

#define VERSION "1.0.1"
#define ANNA_SCRIPT_PATH "/home/pico/calibris/auto_update/anna.sh"

// --- Print usage ---
static void print_usage(const char *prog_name) {
    printf("Tamper Log CLI Tool v%s\n", VERSION);
    printf("Usage: %s --type <tamper_type> [options]\n\n", prog_name);
    printf("Required:\n");
    printf("  -t, --type <type>        Tamper type (e.g., magnetic, firmware, weight_drift)\n\n");
    printf("Optional:\n");
    printf("  -d, --details <text>     Details or description of the tamper event\n");
    printf("  -c, --config <path>      Path to config.json (default: %s)\n", DEFAULT_CONFIG_FILE);
    printf("  -D, --db <path>          Path to SQLite database (default: %s)\n", DEFAULT_DB_PATH);
    printf("  -h, --help               Show this help message\n");
    printf("  -v, --version            Show version\n\n");
    printf("Examples:\n");
    printf("  %s --type magnetic\n", prog_name);
    printf("  %s --type firmware --details \"Hash mismatch detected\"\n", prog_name);
    printf("  %s -t weight_drift -d \"Drift:  5.2g exceeded 3.0g threshold\"\n", prog_name);
}

// --- Run anna.sh script to sync tamper logs ---
static int run_anna_sync(void) {
    printf("\n[INFO] Running anna.sh to sync tamper logs to remote server...\n");

    // Check if script exists and is executable before calling system()
    if (access(ANNA_SCRIPT_PATH, X_OK) != 0) {
        fprintf(stderr, "[WARNING] Script not found or not executable: %s\n", ANNA_SCRIPT_PATH);
        return -1;
    }

    int ret = system(ANNA_SCRIPT_PATH);

    if (ret == -1) {
        fprintf(stderr, "[WARNING] Failed to execute anna.sh script\n");
        return -1;
    } else if (WIFEXITED(ret)) {
        int exit_status = WEXITSTATUS(ret);
        if (exit_status == 0) {
            printf("[INFO] anna.sh completed successfully\n");
            return 0;
        } else {
            fprintf(stderr, "[WARNING] anna.sh exited with status %d\n", exit_status);
            return exit_status;
        }
    } else {
        fprintf(stderr, "[WARNING] anna.sh terminated abnormally\n");
        return -1;
    }
}

// --- Main ---
int main(int argc, char *argv[]) {
    char *tamper_type = NULL;
    char *details = NULL;
    char *config_path = NULL;
    char *db_path = NULL;

    // Define long options
    static struct option long_options[] = {
        {"type",    required_argument, 0, 't'},
        {"details", required_argument, 0, 'd'},
        {"config",  required_argument, 0, 'c'},
        {"db",      required_argument, 0, 'D'},
        {"help",    no_argument,       0, 'h'},
        {"version", no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    // Parse arguments
    int opt;
    int option_index = 0;
    
    // FIXED: Removed space in optstring "t:d:c:D:hv"
    while ((opt = getopt_long(argc, argv, "t:d:c:D:hv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 't':
                tamper_type = optarg;
                break;
            case 'd':
                details = optarg;
                break;
            case 'c':
                config_path = optarg;
                break;
            case 'D': 
                db_path = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                printf("tamper_log v%s\n", VERSION);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Validate required arguments
    if (!tamper_type) {
        fprintf(stderr, "Error: --type is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // Use defaults if paths not specified
    if (!config_path) config_path = DEFAULT_CONFIG_FILE;
    if (!db_path) db_path = DEFAULT_DB_PATH;

    // Log the tamper event
    printf("==========================================\n");
    printf("  Tamper Log CLI Tool v%s\n", VERSION);
    printf("==========================================\n\n");
    printf("[INFO] Tamper type: %s\n", tamper_type);
    if (details) {
        printf("[INFO] Details:  %s\n", details);
    }
    printf("[INFO] Config: %s\n", config_path);
    printf("[INFO] Database: %s\n\n", db_path);

    TamperLogResult result = log_tamper_ex(tamper_type, details, config_path, db_path);

    if (result != TAMPER_LOG_SUCCESS) {
        fprintf(stderr, "\n[ERROR] %s\n", tamper_log_strerror(result));
        return (int)result;
    }

    printf("\n[SUCCESS] Tamper event logged successfully!\n");

    // Run anna.sh to sync tamper logs to remote server
    run_anna_sync();

    return 0;
}
