
#############################################################
# Required variables for each makefile
# Discard this section from all parent makefiles
# Expected variables (with automatic defaults):
#   CSRCS (all "C" files in the dir)
#   SUBDIRS (all subdirs with a Makefile)
#   GEN_LIBS - list of libs to be generated ()
#   GEN_IMAGES - list of images to be generated ()
#   COMPONENTS_xxx - a list of libs/objs in the form
#     subdir/lib to be extracted and rolled up into
#     a generated lib/image xxx.a ()
#
ifndef PDIR
GEN_LIBS = libmbedtls.a
endif

STD_CFLAGS=-std=gnu11 -Wimplicit -Wno-implicit-function-declaration

MBEDTLS_PATH=../../sdk/mbedtls_src

# Important: platform must go before library, due to double "net.c" files
VPATH:=$(addprefix $(MBEDTLS_PATH)/mbedtls/, platform library app)

# All .c files as their corresponding .o files, no path.
COMPONENTS_libmbedtls:=$(patsubst %.c,%.o,$(notdir $(foreach dir,$(VPATH),$(wildcard $(dir)/*.c))))

INCLUDES += -I $(MBEDTLS_PATH)/include -I $(MBEDTLS_PATH)/include/mbedtls
DEFINES += -DMBEDTLS_CONFIG_FILE='"config_esp.h"' -Dfcntl=lwip_fcntl

#############################################################
# Configuration i.e. compile options etc.
# Target specific stuff (defines etc.) goes in here!
# Generally values applying to a tree are captured in the
#   makefile at its root level - these are then overridden
#   for a subtree within the makefile rooted therein
#
#DEFINES += 

#############################################################
# Recursion Magic - Don't touch this!!
#
# Each subtree potentially has an include directory
#   corresponding to the common APIs applicable to modules
#   rooted at that subtree. Accordingly, the INCLUDE PATH
#   of a module can only contain the include directories up
#   its parent path, and not its siblings
#
# Required for each makefile to inherit from the parent
#

INCLUDES := $(INCLUDES) -I $(PDIR)include
INCLUDES += -I ./
INCLUDES += -I ./include
INCLUDES += -I ../include
INCLUDES += -I ../../include
PDIR := ../$(PDIR)
sinclude $(PDIR)Makefile

