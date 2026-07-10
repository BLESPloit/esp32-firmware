#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
#include "argtable3/argtable3.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <fcntl.h>

#include "ble/ble_bond.h"
#include "api/console.h"
#include "api/wifi.h"
#include "api/usb_net.h"
#include "common/storage.h"
#include "common/utils.h"

#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#if CONFIG_TINYUSB_CDC_ENABLED
#include "tusb_console.h"
#include "tusb_cdc_acm.h"
#endif

static void console_repl_task(void *arg)
{
    esp_console_repl_config_t *cfg = (esp_console_repl_config_t *)arg;
    char prompt_buf[48];

    /* idf.py monitor is not a full VT100 terminal. Smart linenoise emits CSI sequences on
     * stdout; some USB CDC stacks feed them back into RX, which then get executed as commands. */
    linenoiseSetDumbMode(1);
    linenoiseSetMultiLine(0);
    snprintf(prompt_buf, sizeof(prompt_buf), "%s ", cfg->prompt);

    linenoiseSetMaxLineLen(cfg->max_cmdline_length);
    printf("\r\nType 'help' to get the list of commands.\r\n");

    while (true) {
        char *line = linenoise(prompt_buf);
        if (line == NULL || line[0] == '\0') {
            if (line != NULL) {
                linenoiseFree(line);
            }
            continue;
        }
        linenoiseHistoryAdd(line);
        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        linenoiseFree(line);
    }
}

static const char *TAG = "console";

static void start_console_repl_task(esp_console_repl_config_t *repl_config)
{
    if (xTaskCreate(console_repl_task, "console_repl", repl_config->task_stack_size,
                    repl_config, repl_config->task_priority, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start console REPL task");
    }
}

extern device_config_t config;

// command arguments
static struct {
//    struct arg_lit *list_bonded;
    struct arg_lit *remove_bonded;
    struct arg_end *end;
} bond_args;

static struct {
    struct arg_str *ssid;
    struct arg_str *psk;
    struct arg_str *ap_ssid;
    struct arg_str *ap_psk;
    struct arg_str *mode;
    struct arg_lit *on;
    struct arg_lit *off;
    struct arg_end *end;
} wifi_args;


static struct {
    struct arg_str *mode;
    struct arg_end *end;
} usb_console_args;

esp_console_cmd_t free_cmd, reboot_cmd, reset_cmd, bond_cmd, wifi_cmd, version_cmd;
esp_console_cmd_t usb_console_cmd;

// The esp_console_register_help_command() registers global help for all commands.
// We need extra function for a single command help.
void help_command(esp_console_cmd_t *cmd)
{
    // Taken mostly from esp-idf/components/console/commands.c
    // First line: command name and hint, pad all the hints to the same column                                                                                  
    const char *hint = (cmd->hint) ? cmd->hint : "";                                                                            
    printf("%-s %s\n", cmd->command, hint);                                                                                    
    // Second line: print help using argtable helper                                                                          
    printf("  "); // arg_print_formatted does not indent the first line
    arg_print_formatted(stdout, 2, 78, cmd->help);
    // Finally, print the list of arguments
    if (cmd->argtable) {
        arg_print_glossary(stdout, (void **) cmd->argtable, "  %12s  %s\n");
    }
    printf("\n");
}

void ask_reboot(void)
{
    char ans;
    ESP_LOGI(TAG, "Reboot needed to enforce changes");
    printf(" --> Would you like to reboot now? [Y|n]\n");
    scanf("%c", &ans);
    if (ans != 'n' && ans != 'N') {
        esp_restart();
    }
}

static int set_bond(int argc, char **argv)
{
    if (argc==1) { //no parameters
        list_bonded_devices_with_keys();
        help_command(&bond_cmd);
        return ESP_OK;
    }

    int nerrors = arg_parse(argc, argv, (void **) &bond_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, bond_args.end, argv[0]);
        return ESP_FAIL;
    }

    if (bond_args.remove_bonded->count > 0) {
        clear_bond_database();
    }

    return ESP_OK;
}



static int set_free(int argc, char **argv)
{
    log_memory_usage("On demand");
    return ESP_OK;
}


static int set_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_restart();
    return ESP_OK;
}

#if CONFIG_TINYUSB_NET_MODE_NCM
static int set_usb_console(int argc, char **argv)
{
    if (argc == 1) {
        printf("USB console: %s\n",
               config.usb_jtag_console.value.u8 ? "jtag (USB Serial/JTAG)" : "tinyusb (CDC + NCM)");
        help_command(&usb_console_cmd);
        return ESP_OK;
    }

    int nerrors = arg_parse(argc, argv, (void **)&usb_console_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, usb_console_args.end, argv[0]);
        return ESP_FAIL;
    }

    const char *mode = usb_console_args.mode->sval[0];
    bool want_jtag = false;
    if (strcmp(mode, "jtag") == 0) {
        want_jtag = true;
    } else if (strcmp(mode, "tinyusb") == 0) {
        want_jtag = false;
    } else {
        printf("Unknown mode: %s (use: jtag, tinyusb)\n", mode);
        return ESP_ERR_INVALID_ARG;
    }

    if (config.usb_jtag_console.value.u8 == want_jtag) {
        printf("Already in %s mode\n", mode);
        return ESP_OK;
    }

    config.usb_jtag_console.value.u8 = want_jtag;
    if (write_config_nvs() != ESP_OK) {
        printf("Failed to save NVS\n");
        return ESP_FAIL;
    }

    printf("USB console set to %s — rebooting...\n", mode);
    esp_restart();
    return ESP_OK;
}
#endif

static int set_reset(int argc, char **argv)
{
    esp_err_t err = ESP_OK;
    char ans;

    printf(" --> Reset the device settings to factory default? [y|N]\n");
    scanf("%c", &ans);
    if (ans == 'y' || ans == 'Y') {
        err = nvs_flash_erase();
        if (err) {
            ESP_LOGE(TAG,"NVS Flash erase error: %x", err);
        } else {
            ESP_LOGI(TAG,"NVS Flash erased. Rebooting...");
            esp_restart();
        }
    }
    return err;
}

static int set_version(int argc, char **argv)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    printf("Firmware version: %s\n", desc->version);
    printf("Project name:     %s\n", desc->project_name);
    printf("Compile time:     %s %s\n", desc->date, desc->time);
    printf("IDF version:      %s\n", desc->idf_ver);
    return ESP_OK;
}


static int set_wifi(int argc, char **argv)
{
    bool write_nvs = false;
    size_t len = 0;
    char ip_str[32];
    char usb_ip_str[32];


    if (argc==1) { //no parameters
        printf("Current wifi settings:\n");
        printf(" Mode: %s\n",
               config.wifi_mode_pref.value.u8 == WIFI_MODE_PREF_AP_ONLY ? "AP-only" : "STA-first");
        char eff_ap[48];
        wifi_get_ap_ssid_effective(eff_ap, sizeof(eff_ap));
        wifi_get_current_ip(ip_str, sizeof(ip_str));
        usb_net_get_ip(usb_ip_str, sizeof(usb_ip_str));
        printf(" WiFi: %s\n", config.net_enabled.value.u8 ? "enabled" : "disabled");
        printf(" WiFi IP:                  %s\n", ip_str);
        printf(" USB IP:                   %s\n", usb_ip_str);
        printf(" Soft-AP SSID (effective): %s\n", eff_ap);
        printf(" Soft-AP SSID (custom NVS): %s\n",
               config.wifi_ap_ssid.value.str ? config.wifi_ap_ssid.value.str : "(none — MAC suffix default)");
        printf(" Soft-AP PSK: %s\n",
               config.wifi_ap_psk.value.str ? "(custom NVS)" : "(firmware default)");
        if (config.wifi_ssid.value.str != NULL) {
            printf(" STA SSID: %s\n", config.wifi_ssid.value.str);
        } else {
            printf(" STA: no credentials\n");
        }
        
        help_command(&wifi_cmd);
        return ESP_OK;
    }
    int nerrors = arg_parse(argc, argv, (void **) &wifi_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_args.end, argv[0]);
        return ESP_FAIL;
    }
    if (wifi_args.off->count > 0) {
        config.net_enabled.value.u8 = 0;
        ESP_LOGI(TAG, "WiFi disabled");
        write_nvs = true;
    } else if (wifi_args.on->count > 0) {
        config.net_enabled.value.u8 = 1;
        ESP_LOGI(TAG, "WiFi enabled (using existing credentials)");
        write_nvs = true;
    }
    else {
        if (wifi_args.mode->count > 0) {
            const char *m = wifi_args.mode->sval[0];
            if (strcasecmp(m, "sta") == 0) {
                config.wifi_mode_pref.value.u8 = WIFI_MODE_PREF_STA_FIRST;
                ESP_LOGI(TAG, "WiFi mode STA-first");
                write_nvs = true;
            } else if (strcasecmp(m, "ap") == 0) {
                config.wifi_mode_pref.value.u8 = WIFI_MODE_PREF_AP_ONLY;
                ESP_LOGI(TAG, "WiFi mode AP-only");
                write_nvs = true;
            } else {
                printf("Invalid --mode (use sta or ap)\n");
                return ESP_FAIL;
            }
        }
        if (wifi_args.ap_ssid->count > 0) {
            const char *v = wifi_args.ap_ssid->sval[0];
            if (wifi_validate_custom_ap_ssid(v) != ESP_OK) {
                printf("Invalid AP SSID length (max %d)\n", MAX_WIFI_AP_SSID_LEN);
                return ESP_FAIL;
            }
            free(config.wifi_ap_ssid.value.str);
            config.wifi_ap_ssid.value.str = NULL;
            if (strlen(v) > 0) {
                config.wifi_ap_ssid.value.str = strdup(v);
                if (!config.wifi_ap_ssid.value.str) {
                    ESP_LOGE(TAG, "OOM AP SSID");
                    return ESP_FAIL;
                }
            }
            ESP_LOGI(TAG, "AP SSID NVS updated");
            write_nvs = true;
        }
        if (wifi_args.ap_psk->count > 0) {
            const char *v = wifi_args.ap_psk->sval[0];
            if (wifi_validate_custom_ap_psk(v) != ESP_OK) {
                printf("Invalid AP PSK (use 8..63 chars WPA2, or empty for default)\n");
                return ESP_FAIL;
            }
            free(config.wifi_ap_psk.value.str);
            config.wifi_ap_psk.value.str = NULL;
            if (strlen(v) > 0) {
                config.wifi_ap_psk.value.str = strdup(v);
                if (!config.wifi_ap_psk.value.str) {
                    ESP_LOGE(TAG, "OOM AP PSK");
                    return ESP_FAIL;
                }
            }
            ESP_LOGI(TAG, "AP PSK NVS updated");
            write_nvs = true;
        }
        if (wifi_args.ssid->count > 0) {
            len = strlen(wifi_args.ssid->sval[0]);
            ESP_LOGI(TAG, "SSID len: %d", len);
            if (len > MAX_WIFI_SSID_LENGTH)
            {
                printf(" Wifi SSID too long: %d!", len);
                return ESP_FAIL;
            }
            if (config.wifi_ssid.value.str == NULL) {
                config.wifi_ssid.value.str = malloc(len+1);
            }
            if (config.wifi_ssid.value.str == NULL) {
                ESP_LOGE(TAG, "Error allocating memory for SSID!");
                return ESP_FAIL;
            }
            strncpy(config.wifi_ssid.value.str, wifi_args.ssid->sval[0], len);
            config.wifi_ssid.value.str[len] = '\0';  // Explicitly null-terminate
            ESP_LOGI(TAG, "Setting wifi SSID to '%s'", config.wifi_ssid.value.str);
            write_nvs = true;
        }
        if (wifi_args.psk->count > 0) {
            len = strlen(wifi_args.psk->sval[0]);
            ESP_LOGI(TAG, "PSK len: %d", len);
            if (len > MAX_WIFI_PSK_LENGTH)
            {
                printf(" Wifi PSK too long: %d!", len);
                return ESP_FAIL;
            }
            if (config.wifi_psk.value.str == NULL) {
                config.wifi_psk.value.str = malloc(len+1);
            }
            if (config.wifi_psk.value.str == NULL) {
                ESP_LOGE(TAG, "Error allocating memory for PSK!");
                return ESP_FAIL;
            }
            strncpy(config.wifi_psk.value.str, wifi_args.psk->sval[0], len);
            config.wifi_psk.value.str[len] = '\0';  // Explicitly null-terminate
            ESP_LOGI(TAG, "Setting wifi PSK to '%s'", config.wifi_psk.value.str);
            write_nvs = true;
        }
        if (write_nvs) {
            config.net_enabled.value.u8 = 1;  // Credentials = WiFi enabled
        }

    }


    if (write_nvs) {
        write_config_nvs();
        ask_reboot();
    }
    return ESP_OK;
}

void register_bond(void)
{
//    bond_args.list_bonded = arg_lit0("b", "bonded", "list bonded devices"),
    bond_args.remove_bonded = arg_lit0("r", "remove-bonded", "remove all bonded devices"),
    bond_args.end = arg_end(20);

    bond_cmd.command = "bond";
    bond_cmd.help = "List / remove bonds";
    bond_cmd.hint = "[-r ]";
    bond_cmd.func = &set_bond;
    bond_cmd.argtable = &bond_args;

    ESP_ERROR_CHECK( esp_console_cmd_register(&bond_cmd) );

    if (arg_nullcheck((void *)&bond_args) != 0)
        {
        // NULL entries were detected, some allocations must have failed 
        ESP_LOGE(TAG, "Register cmd sec: insufficient memory");
        }
}


void register_free(void)
{
    free_cmd.command = "free";
    free_cmd.help = "Show memory usage";
    free_cmd.hint = NULL;
    free_cmd.func = &set_free;
    free_cmd.argtable = NULL;

    ESP_ERROR_CHECK( esp_console_cmd_register(&free_cmd) );
}


void register_reboot(void)
{
    reboot_cmd.command = "reboot";
    reboot_cmd.help = "Reboot the device";
    reboot_cmd.hint = NULL;
    reboot_cmd.func = &set_reboot;
    reboot_cmd.argtable = NULL;

    ESP_ERROR_CHECK( esp_console_cmd_register(&reboot_cmd) );
}

#if CONFIG_TINYUSB_NET_MODE_NCM
void register_usb_console(void)
{
    usb_console_args.mode = arg_str1(NULL, NULL, "<jtag|tinyusb>", "USB console mode");
    usb_console_args.end = arg_end(2);

    usb_console_cmd.command = "usb-console";
    usb_console_cmd.help = "Switch USB console: jtag = Serial/JTAG (idf.py flash, full logs in monitor), tinyusb = CDC + NCM (USB ethernet)";
    usb_console_cmd.hint = "<jtag|tinyusb>";
    usb_console_cmd.func = &set_usb_console;
    usb_console_cmd.argtable = &usb_console_args;

    ESP_ERROR_CHECK(esp_console_cmd_register(&usb_console_cmd));
}
#endif

void register_reset(void)
{
    reset_cmd.command = "reset";
    reset_cmd.help = "Reset the device settings to factory default (erase NVS storage).";
    reset_cmd.hint = NULL;
    reset_cmd.func = &set_reset;
    reset_cmd.argtable = NULL;

    ESP_ERROR_CHECK( esp_console_cmd_register(&reset_cmd) );
}

void register_version(void)
{
    version_cmd.command  = "version";
    version_cmd.help     = "Show firmware version and build info";
    version_cmd.hint     = NULL;
    version_cmd.func     = &set_version;
    version_cmd.argtable = NULL;

    ESP_ERROR_CHECK(esp_console_cmd_register(&version_cmd));
}

void register_wifi(void)
{
    wifi_args.ssid = arg_str0("s", "ssid", "<ssid>", "STA SSID");
    wifi_args.psk = arg_str0("p", "PSK", "<psk>", "STA password");
    wifi_args.ap_ssid = arg_str0("a", "ap-ssid", "<ssid>", "Soft-AP SSID (omit value or \"\" clears → MAC default)");
    wifi_args.ap_psk = arg_str0("q", "ap-psk", "<psk>", "Soft-AP WPA2 password (empty clears → firmware default)");
    wifi_args.mode = arg_str0("M", "mode", "<sta|ap>", "sta = STA-first + AP fallback, ap = AP-only");
    wifi_args.off  = arg_lit0("f", "off",   "Disable WiFi (USB Ethernet stays up)");
    wifi_args.on   = arg_lit0("e", "on",    "Enable WiFi (uses existing credentials)");
    wifi_args.end = arg_end(20);

    wifi_cmd.command = "wifi";
    wifi_cmd.help = "STA/AP WiFi config, soft-AP overrides, mode preference";
    wifi_cmd.hint = "-s -p | -M sta|ap | -a AP-SSID | -q AP-PSK | -e | -f";
    wifi_cmd.func = &set_wifi;
    wifi_cmd.argtable = &wifi_args;

    ESP_ERROR_CHECK( esp_console_cmd_register(&wifi_cmd) );

    if (arg_nullcheck((void *)&wifi_args) != 0)
    {
        // NULL entries were detected, some allocations must have failed 
        ESP_LOGE(TAG, "Register cmd 'wifi': insufficient memory");
    }
}
void initialize_console(void)
{
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "BLESPlo.it >";
    repl_config.max_cmdline_length = 1024;

    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback*) &esp_console_get_hint);

    esp_console_register_help_command();

    register_bond();
    register_free();
    register_reboot();
#if CONFIG_TINYUSB_NET_MODE_NCM
    register_usb_console();
#endif
    register_reset();
    register_version();
    register_wifi();
    register_file_commands();
    register_ws_proxy();

#if CONFIG_TINYUSB_NET_MODE_NCM
    if (config.usb_jtag_console.value.u8) {
#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

        usb_serial_jtag_driver_config_t usj_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_config));

        esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
        console_config.max_cmdline_length = repl_config.max_cmdline_length;
        ESP_ERROR_CHECK(esp_console_init(&console_config));
        usb_serial_jtag_vfs_use_driver();

        fcntl(fileno(stdout), F_SETFL, 0);
        fcntl(fileno(stdin), F_SETFL, 0);
        setvbuf(stdin, NULL, _IONBF, 0);

        start_console_repl_task(&repl_config);
        ESP_LOGI(TAG, "Console on USB Serial/JTAG (NVS usbjtag=1)");
#else
        ESP_LOGE(TAG, "usbjtag=1 but CONFIG_USJ_ENABLE_USB_SERIAL_JTAG is disabled");
#endif
        return;
    }
#endif

#if CONFIG_TINYUSB_CDC_ENABLED
    {
        esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
        console_config.max_cmdline_length = repl_config.max_cmdline_length;
        ESP_ERROR_CHECK(esp_console_init(&console_config));
        ESP_ERROR_CHECK(esp_tusb_init_console(TINYUSB_CDC_ACM_0));

        fcntl(fileno(stdout), F_SETFL, 0);
        fcntl(fileno(stdin), F_SETFL, 0);
        setvbuf(stdin, NULL, _IONBF, 0);

        start_console_repl_task(&repl_config);
        ESP_LOGI(TAG, "Console on TinyUSB CDC-ACM");
    }
#else
#error Unsupported console type
#endif
}