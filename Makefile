PROGRAM = main

EXTRA_COMPONENTS = \
	extras/dht \
	extras/http-parser \
	extras/dhcpserver \
	extras/rboot-ota \
	extras/ssd1306 \
	extras/fonts \
	extras/i2c \
	$(abspath ../../components/wifi_config) \
	$(abspath ../../components/wolfssl) \
	$(abspath ../../components/cJSON) \
	$(abspath ../../components/homekit)

FONTS_TERMINUS_BOLD_8X14_ISO8859_1 = 1
FONTS_TERMINUS_BOLD_14X28_ISO8859_1 = 1
FONTS_TERMINUS_BOLD_11X22_ISO8859_1 = 1
FONT_FACE_TERMINUS_BOLD_6X12_ISO8859_1 = 1

FLASH_SIZE = 8
FLASH_MODE = dout
FLASH_SPEED = 40

HOMEKIT_SPI_FLASH_BASE_ADDR = 0x8c000
HOMEKIT_MAX_CLIENTS = 16
HOMEKIT_SMALL = 0
HOMEKIT_OVERCLOCK = 0
HOMEKIT_OVERCLOCK_PAIR_SETUP = 0
HOMEKIT_OVERCLOCK_PAIR_VERIFY = 0

EXTRA_CFLAGS += -I../.. -DHOMEKIT_SHORT_APPLE_UUIDS -DHOMEKIT_DEBUG -DFONTS_TERMINUS_BOLD_8X14_ISO8859_1 -DFONTS_TERMINUS_BOLD_14X28_ISO8859_1 -DFONTS_TERMINUS_BOLD_11X22_ISO8859_1 -DFONT_FACE_TERMINUS_BOLD_6X12_ISO8859_1 -DWIFI_CONFIG_CONNECT_TIMEOUT=180000

include $(abspath ../../sdk/esp-open-rtos/common.mk)

signature: 
	$(shell /usr/local/opt/openssl/bin/openssl   sha384 -binary -out firmware/main.bin.sig firmware/main.bin)
	$(shell printf "%08x" `cat firmware/main.bin | wc -c`| xxd -r -p >>firmware/main.bin.sig)
 
monitor:
	$(FILTEROUTPUT) --port $(ESPPORT) --baud 115200 --elf $(PROGRAM_OUT)
