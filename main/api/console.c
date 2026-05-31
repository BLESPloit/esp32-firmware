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
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include <fcntl.h>

#include "ble/ble_bond.h"
#include "api/console.h"
#include "api/wifi.h"
#include "common/storage.h"
#include "common/utils.h"

static const char *TAG = "console";

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


esp_console_cmd_t free_cmd, reboot_cmd, reset_cmd, bond_cmd, wifi_cmd, version_cmd;

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
    esp_restart();
    return ESP_OK;
}

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


    if (argc==1) { //no parameters
        printf("Current wifi settings:\n");
        printf(" Mode: %s\n",
               config.wifi_mode_pref.value.u8 == WIFI_MODE_PREF_AP_ONLY ? "AP-only" : "STA-first");
        char eff_ap[48];
        wifi_get_ap_ssid_effective(eff_ap, sizeof(eff_ap));
        wifi_get_current_ip(ip_str, sizeof(ip_str));
        printf(" Networking: %s\n", config.net_enabled.value.u8 ? "enabled" : "disabled");
        printf(" Current IP:               %s\n", ip_str);
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
        ESP_LOGI(TAG, "Networking disabled");
        write_nvs = true;
    } else if (wifi_args.on->count > 0) {
        config.net_enabled.value.u8 = 1;
        ESP_LOGI(TAG, "Networking enabled (using existing credentials)");
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
            config.net_enabled.value.u8 = 1;  // Credentials = enabled
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

void register_reset(void)
{
    reset_cmd.command = "reset";
    reset_cmd.help = "Reset the device settings to factory default (erase NVS storage). Note: for badge log files use 'log' command.";
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
    wifi_args.off  = arg_lit0("f", "off",   "Disable networking");
    wifi_args.on   = arg_lit0("e", "on",    "Enable networking (uses existing credentials)");
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
    esp_console_repl_t *repl = NULL;

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    // Prompt to be printed before each line
    repl_config.prompt = "BLESPlo.it >";
    repl_config.max_cmdline_length = 1024;

    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback*) &esp_console_get_hint);

    // help() command that prints syntax for all commands
    esp_console_register_help_command();

    register_bond();
    register_free();
    register_reboot();
    register_reset();
    register_version();
    register_wifi();
 
    // web proxy
    register_file_commands();
    register_ws_proxy();

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));

#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));    

#else
#error Unsupported console type
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));

}