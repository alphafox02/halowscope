# Elecrow ESP32 WiFi HaLow Module: what the stock firmware shows

Copyright 2026 CEMAXECUTER LLC

Findings from the stock "Web Camera Serve Demo" firmware: its boot log at
115200 baud and a full flash dump (read twice, identical). The dump itself is
kept out of git.

## Chips

- ESP32-S3 rev v0.2, 16 MB flash (the stock image header says 8 MB).
- 8 MB octal PSRAM (AP Memory, 80 MHz). Octal PSRAM takes GPIO33-37.
- Morse Micro chip ID 0x306 (MM6108) inside the Quectel FGH100M, on SPI at
  40 MHz.
- OV2640 camera, SCCB on SDA 45 / SCL 42.

## Stock firmware

- ESP-IDF v5.1.1, project `web_camera_serve`, built with Morse Micro's
  mm-iot-esp32 framework: morselib 2.6.4-esp32, MM6108 firmware 1.13.1
  (rel_1_13_1_2024_Sep_30).
- The ESP32-S3 runs the HaLow host driver and lwIP and loads the MM6108
  firmware over SPI at every boot. HaLow has no configuration of its own on
  the module: the SSID and passphrase from `AT+CWJAP` are stored in the
  ESP's NVS (`sta_cfg`). Replacing the ESP firmware replaces the HaLow
  control as well.
- The board config file (BCF) is compiled into the app image (the `MMBC`
  blob, 344 bytes). It is Elecrow's own: board name `custom`, build string
  ending `_Modified`, regulatory domains AU EU IN JP KR NZ SG US. It does not
  match any BCF shipped with morsemicro/halow 2.11.2.
- Flash layout: bootloader at 0x0, partition table at 0x8000, nvs 0x9000
  (24 KB), phy_init 0xf000, factory app 0x10000 (4 MB). Everything above
  about 0x1b0000 is erased.
- Also starts a BLE GATT demo service.

## HaLow pins (morsemicro/halow shim names)

| Signal | GPIO | Evidence |
|---|---|---|
| MM_RESET_N | 8 | first output configured, before the SPI setup |
| MM_SPI_CS | 2 | output, configured with WAKE (mask 0x204), driven low after WAKE |
| MM_WAKE | 9 | output, mask 0x204 |
| MM_BUSY | 7 | input with pull-down (mask 0x80) |
| MM_SPI_IRQ | 6 | input (mask 0x40) |
| MM_SPI_MOSI | 3 | spi_bus_config_t in the stock binary |
| MM_SPI_MISO | 5 | spi_bus_config_t |
| MM_SPI_SCK | 4 | spi_bus_config_t |

SPI2_HOST, mode 0, 40 MHz, CS driven as a GPIO (spics_io_num -1). The order of
the gpio_config calls and the struct fields match mmhal_wlan.c in
morsemicro/halow.

## TF card

The stock firmware mounts the card with ESP-IDF's SDMMC host (not SPI mode)
at `/sdcard` and saves camera images under `/sdcard/image`. From the slot
and host settings it passes to `esp_vfs_fat_sdmmc_mount`:

| Signal | GPIO |
|---|---|
| CLK | 15 |
| CMD | 16 |
| D0 | 11 |

1-bit bus with the internal pull-ups enabled, 10 MHz clock, SDMMC slot 0,
no formatting on a failed mount. The manual gives 32 GB as the largest
card.

## Still open

- Camera DVP pins, the blue data LED.
- Whether the S3's native USB (GPIO19/20) is wired anywhere.
