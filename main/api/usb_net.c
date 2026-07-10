#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/esp_netif_net_stack.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tinyusb_net.h"
#include "common/utils.h"
#include "api/usb_net.h"
#include "api/wifi.h"
#include "api/web_server.h"

#if CONFIG_TINYUSB_NET_MODE_NCM

static const char *TAG = "usb_net";

#define USB_RX_POOL_SLOTS 4
#define USB_RX_FRAME_MAX  1514

static esp_netif_t *s_usb_netif;
static uint8_t s_rx_pool[USB_RX_POOL_SLOTS][USB_RX_FRAME_MAX];
static bool s_rx_pool_used[USB_RX_POOL_SLOTS];
static char s_usb_ip_str[20] = "";
static bool s_ncm_link_up = false;

static const esp_netif_ip_info_t s_usb_ip_info = {
    .ip = { .addr = ESP_IP4TOADDR(192, 168, 5, 1) },
    .gw = { .addr = ESP_IP4TOADDR(192, 168, 5, 1) },
    .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
};

static void *usb_rx_pool_alloc(uint16_t len)
{
    if (len > USB_RX_FRAME_MAX) {
        return NULL;
    }
    for (int i = 0; i < USB_RX_POOL_SLOTS; i++) {
        if (!s_rx_pool_used[i]) {
            s_rx_pool_used[i] = true;
            return s_rx_pool[i];
        }
    }
    return NULL;
}

static void usb_rx_pool_free(void *buffer)
{
    if (buffer == NULL) {
        return;
    }
    for (int i = 0; i < USB_RX_POOL_SLOTS; i++) {
        if (buffer == s_rx_pool[i]) {
            s_rx_pool_used[i] = false;
            return;
        }
    }
    ESP_LOGW(TAG, "RX pool free: unknown buffer %p", buffer);
}

static void usb_netif_free_rx(void *h, void *buffer)
{
    (void)h;
    usb_rx_pool_free(buffer);
}

static esp_err_t usb_netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    esp_err_t err = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB NCM transmit failed (%u bytes)", (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t usb_netif_recv(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_usb_netif || len == 0) {
        return ESP_OK;
    }

    void *copy = usb_rx_pool_alloc(len);
    if (!copy) {
        ESP_LOGW(TAG, "RX pool exhausted (%u bytes)", len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    return esp_netif_receive(s_usb_netif, copy, len, NULL);
}

static void usb_set_ncm_link(bool up)
{
    s_ncm_link_up = up;
    if (tud_mounted()) {
        tud_network_link_state(0, up);
        ESP_LOGI(TAG, "NCM link %s", up ? "up" : "down");
    } else {
        ESP_LOGW(TAG, "NCM link %s skipped: not mounted", up ? "up" : "down");
    }
}

static void usb_on_host_attached(void)
{
    usb_set_ncm_link(true);
    start_web_server();
    wifi_broadcast_status();
}

void tud_mount_cb(void)
{
    usb_on_host_attached();
}

void tud_umount_cb(void)
{
    usb_set_ncm_link(false);
    wifi_broadcast_status();
}

static esp_err_t usb_netif_create(void)
{
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &s_usb_ip_info,
        .if_key = "USB_NCM",
        .if_desc = "usb ncm",
        .route_prio = 20,
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,
        .transmit = usb_netif_transmit,
        .driver_free_rx_buffer = usb_netif_free_rx,
    };

    struct esp_netif_netstack_config stack_cfg = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &stack_cfg,
    };

    s_usb_netif = esp_netif_new(&cfg);
    if (s_usb_netif == NULL) {
        return ESP_FAIL;
    }

    uint8_t lwip_mac[6] = { 0x02, 0x02, 0x11, 0x22, 0x33, 0x02 };
    esp_netif_set_mac(s_usb_netif, lwip_mac);

    uint32_t lease_minutes = 60;
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME,
                           &lease_minutes, sizeof(lease_minutes));

    esp_netif_action_start(s_usb_netif, 0, 0, 0);

    snprintf(s_usb_ip_str, sizeof(s_usb_ip_str), IPSTR, IP2STR(&s_usb_ip_info.ip));
    return ESP_OK;
}

esp_err_t usb_net_init(void)
{
    ESP_LOGI(TAG, "Initializing TinyUSB composite (CDC-ACM + NCM)");
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    ESP_LOGE(TAG, "sdkconfig conflict: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG breaks TinyUSB NCM");
#endif

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
        .configuration_descriptor = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install failed");

    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
    ESP_RETURN_ON_ERROR(tusb_cdc_acm_init(&acm_cfg), TAG, "tusb_cdc_acm_init failed");

    const tinyusb_net_config_t net_cfg = {
        .mac_addr = { 0x02, 0x02, 0x11, 0x22, 0x33, 0x01 },
        .on_recv_callback = usb_netif_recv,
        .on_init_callback = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg), TAG, "tinyusb_net_init failed");

    ESP_RETURN_ON_ERROR(usb_netif_create(), TAG, "USB esp-netif create failed");

    usb_set_ncm_link(false);
    log_memory_usage("after usb_net_init");
    return ESP_OK;
}

void usb_net_get_ip(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }
    if (s_usb_ip_str[0]) {
        snprintf(out, out_sz, "%s", s_usb_ip_str);
    } else {
        snprintf(out, out_sz, "(not configured)");
    }
}

bool usb_net_link_up(void)
{
    return s_ncm_link_up && tud_mounted();
}

#else

esp_err_t usb_net_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void usb_net_get_ip(char *out, size_t out_sz)
{
    if (out && out_sz > 0) {
        snprintf(out, out_sz, "(disabled)");
    }
}

bool usb_net_link_up(void)
{
    return false;
}

#endif /* CONFIG_TINYUSB_NET_MODE_NCM */
