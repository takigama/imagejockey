#include "msc_disk.h"

#include <string.h>

#include "class/msc/msc_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

static const char *TAG = "msc_disk";
static bool s_active = false;

/* TinyUSB descriptors -- adapted from ESP-IDF's own
 * examples/peripherals/usb/device/tusb_msc reference (endpoint numbering,
 * TUD_CONFIG_DESCRIPTOR/TUD_MSC_DESCRIPTOR macro usage), since these are
 * easy to get subtly wrong by hand. This board is Full-Speed only, so
 * unlike that example there's no high-speed config/qualifier branch. */

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

enum {
    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN  = 0x81,
};

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, /* Espressif's VID -- fine for a personal/hobby device */
    .idProduct = 0x4004,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static char const *s_string_descriptor[] = {
    (const char[]) { 0x09, 0x04 }, /* 0: English (0x0409) */
    "DIY",                         /* 1: Manufacturer */
    "ImageJockey",                 /* 2: Product */
    "IJ0001",                      /* 3: Serial */
};

esp_err_t msc_disk_init(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = s_config_descriptor;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err == ESP_OK) {
        s_active = true;
    }
    return err;
}

void msc_disk_notify_mount_changed(void)
{
    if (!s_active) {
        return;
    }
    ESP_LOGI(TAG, "soft USB reconnect");
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    tud_connect();
}

/* --- tud_msc_* callbacks: TinyUSB calls these directly, no registration. --- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memset(vendor_id, ' ', 8);
    memset(product_id, ' ', 16);
    memset(product_rev, ' ', 4);
    memcpy(vendor_id, "ImageJck", 8);
    memcpy(product_id, "Jockey", 6);
    memcpy(product_rev, "1.0", 3);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!media_is_mounted()) {
        /* Additional Sense 3A-00 = MEDIUM NOT PRESENT. */
        return tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = media_block_count();
    *block_size = 512;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return media_is_writable();
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    return media_read(lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    return media_write(lba, offset, buffer, bufsize);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    switch (scsi_cmd[0]) {
        case 0x35: /* SYNCHRONIZE CACHE(10) -- some hosts send this before "safely remove" */
            media_sync();
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}
