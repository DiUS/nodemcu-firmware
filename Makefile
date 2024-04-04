SHELL:=/bin/bash

export IDF_TOOLS_PATH?=$(HOME)/.espressif-r6

ifeq ($(IDF_PATH),)

THIS_MK_FILE:=$(notdir $(lastword $(MAKEFILE_LIST)))
THIS_DIR:=$(abspath $(dir $(lastword $(MAKEFILE_LIST))))
IDF_PATH=$(THIS_DIR)/sdk/esp32-esp-idf

all: | .idf_tools_installed
	. $(IDF_PATH)/export.sh && $(MAKE) "$@"

%: | .idf_tools_installed
	. $(IDF_PATH)/export.sh && $(MAKE) "$@"

IDF_PATCHES:=$(wildcard idf-patches/*)

$(IDF_PATH)/.patched: $(IDF_PATCHES)
	@echo "Discarding local IDF changes..."
	(cd "$(IDF_PATH)" && git checkout . )
	@echo "Patching IDF..."
	for f in $(IDF_PATCHES); do (cd "$(IDF_PATH)" && patch -p1 < "$(THIS_DIR)/$$f" ); done
	touch "$@"

.idf_tools_installed: $(IDF_PATH)/.patched
	"$(THIS_DIR)/install.sh"
	touch "$@"

else

all:
	$(IDF_PATH)/tools/idf.py $(IDFPY_ARGS) "$@"

%:
	$(IDF_PATH)/tools/idf.py $(IDFPY_ARGS) "$@"

endif

# FIXME - needs updating to work in IDF4
#
#extmod-update:
#	@tools/extmod/extmod.sh update
#
#extmod-clean:
##	@tools/extmod/extmod.sh clean
