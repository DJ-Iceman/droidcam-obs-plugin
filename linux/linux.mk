# Variables with ?= can be overridden
# Example: `ALLOW_STATIC=yes  make`

# Allow static linking some deps
ALLOW_STATIC ?= no

# libimobiledevice
IMOBILEDEV_DIR ?= /opt/libimobiledevice
IMOBILEDEV_LIB ?= $(IMOBILEDEV_DIR)/lib

# libjpeg-turbo
JPEG_DIR ?= /opt/libjpeg-turbo
JPEG_LIB ?= $(JPEG_DIR)/lib$(shell getconf LONG_BIT)

define ADD_Lib =
	INCLUDES += $(shell pkg-config --cflags-only-I  $(1))
	LDD_LIBS += $(shell pkg-config --libs-only-l  $(1))
endef

ifeq "$(ALLOW_STATIC)" "yes"
	INCLUDES += -I$(IMOBILEDEV_DIR)/include
	INCLUDES += -I$(JPEG_DIR)/include

	STATIC += $(JPEG_LIB)/libturbojpeg.a
	STATIC += $(IMOBILEDEV_LIB)/libimobiledevice.a
	STATIC += $(IMOBILEDEV_LIB)/libusbmuxd.a
	STATIC += $(IMOBILEDEV_LIB)/libplist-2.0.a

else
$(eval	$(call ADD_Lib,libturbojpeg))
$(eval	$(call ADD_Lib,libusbmuxd))
endif

ifdef DROIDCAM_OVERRIDE
CXXFLAGS += -DDROIDCAM_OVERRIDE=1
include linux/gui.mk
endif

INCLUDES += -I/usr/include/ffmpeg
INCLUDES += -I/usr/include/obs

LDD_LIBS += -lobs
LDD_FLAG += -shared
