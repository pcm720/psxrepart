#.SILENT:
GIT_VERSION := $(shell git describe --always --dirty --tags --exclude nightly)

# ELF file name
EE_BIN = psxrepart_unc.elf
EE_BIN_PKD = psxrepart.elf

# Base object files
EE_OBJS = main.o init.o

# Base modules
IRX_FILES += iomanX.irx fileXio.irx
IRX_FILES += ps2dev9.irx ps2atad.irx ps2hdd-osd.irx ps2fs.irx
IRX_FILES += dvr.irx dvrmisc.irx dvrdrv.irx dvrfile.irx

# C compiler flags
EE_CFLAGS := -D_EE -O2 -G0 -Wall -DGIT_VERSION="\"${GIT_VERSION}\"" $(EE_CFLAGS)

EE_OBJS_DIR = obj/
EE_ASM_DIR = asm/
EE_SRC_DIR = src/
EE_LIBS = -lcdvd -lpatches -lkernel -ldebug -lfileXio -lpad
EE_INCS = -I../common/include -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I$(PS2SDK)/sbv/include -Iinclude
EE_LDFLAGS += -s

EE_OBJS += $(IRX_FILES:.irx=_irx.o)
EE_OBJS += $(ELF_FILES:.elf=_elf.o)
EE_OBJS += $(RES_FILES:.udm=_udm.o)
EE_OBJS := $(EE_OBJS:%=$(EE_OBJS_DIR)%)

.PHONY: all clean

all: $(EE_BIN_PKD)

clean:
	rm -rf $(EE_OBJS_DIR) $(EE_BIN) $(EE_BIN_PKD) $(EE_BIN_KELF)

$(EE_BIN_PKD): $(EE_BIN)
	ps2-packer $< $@

BIN2C = $(PS2SDK)/bin/bin2c

# IRX files
%_irx.c:
	$(BIN2C) $(PS2SDK)/iop/irx/$(*:$(EE_SRC_DIR)%=%).irx $@ $(*:$(EE_SRC_DIR)%=%)_irx
%ps2hdd-osd_irx.c:
	$(BIN2C) $(PS2SDK)/iop/irx/ps2hdd-osd.irx $@ ps2hdd_osd_irx

$(EE_ASM_DIR):
	@mkdir -p $@

$(EE_OBJS_DIR):
	@mkdir -p $@

$(EE_OBJS_DIR)%.o: $(EE_SRC_DIR)%.c | $(EE_OBJS_DIR)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
