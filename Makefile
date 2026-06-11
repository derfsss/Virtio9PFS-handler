CC = ppc-amigaos-gcc
STRIP = ppc-amigaos-strip
# -MMD -MP emits .d files alongside each .o so editing a header forces
# the right TUs to recompile on the next make (avoids stale-object bugs
# when struct layouts change in headers).
CFLAGS_COMMON = -Wall -D__AMIGAOS4__ -U__USE_INLINE__ -I./include -fno-tree-loop-distribute-patterns -MMD -MP
CFLAGS = $(CFLAGS_COMMON) -O2 -ffunction-sections -fdata-sections
CFLAGS_DEBUG = $(CFLAGS_COMMON) -O2 -DDEBUG
LDFLAGS = -nostartfiles -nodefaultlibs -lgcc
LDFLAGS_RELEASE = $(LDFLAGS) -Wl,--gc-sections

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
OBJ_DIR_DBG = $(BUILD_DIR)/obj-debug
TARGET = $(BUILD_DIR)/Virtio9PFS-handler
TARGET_DEBUG = $(BUILD_DIR)/Virtio9PFS-handler.debug
TEST_TARGET = $(BUILD_DIR)/test_9p

SRC = src/main.c \
      src/fuse_ops.c \
      src/p9_client.c \
      src/p9_marshal.c \
      src/fid_pool.c \
      src/pci/pci_discovery.c \
      src/pci/pci_modern_detect.c \
      src/virtio/virtio_init.c \
      src/virtio/virtqueue.c \
      src/virtio/virtio_irq.c

OBJ = $(patsubst src/%.c, $(OBJ_DIR)/%.o, $(SRC))
OBJ_DEBUG = $(patsubst src/%.c, $(OBJ_DIR_DBG)/%.o, $(SRC))

# WSL2 shared folder (mapped into QEMU as virtfs + USB FAT drive)
SHARED_DIR = $(HOME)/shared

all: $(TARGET)

debug: $(TARGET_DEBUG)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS_RELEASE)
	$(STRIP) --strip-unneeded $(TARGET)

$(TARGET_DEBUG): $(OBJ_DEBUG)
	$(CC) $(OBJ_DEBUG) -o $(TARGET_DEBUG) $(LDFLAGS)

# Release objects
$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Debug objects
$(OBJ_DIR_DBG)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -c $< -o $@

test: $(TEST_TARGET)

$(TEST_TARGET): test/test_9p.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) test/test_9p.c -o $(TEST_TARGET) -lauto

# Host-native unit test for p9_marshal bounds.  Builds with the host
# compiler (gcc), NOT the PPC cross-compiler -- runs as a normal
# Linux/Windows binary.  Used by tools/qemu-regression/robustness Tier 15.2.
HOST_CC ?= gcc
TEST_NATIVE_TARGET = $(BUILD_DIR)/test_p9_marshal_native

test-native: $(TEST_NATIVE_TARGET)

$(TEST_NATIVE_TARGET): test/test_p9_marshal.c test/exec/types.h src/p9_marshal.c include/p9_protocol.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -Wall -O2 -I test -I include test/test_p9_marshal.c -o $@

# Distribution --------------------------------------------------------------
# `make dist`     — stage the release drawer under build/Virtio9PFS/
#                   (handler + debug build + DOSDriver + the
#                   Installation Utility wizard + docs).
# `make dist-lha` — pack the staged drawer into a versioned LHA.  Uses
#                   host lha when available, otherwise runs lha inside
#                   the toolchain Docker image.
#
# The installer is the OS Installation Utility wizard: installer/
# carries the committed install.py (+ locale module), pre-generated
# from installer/virtio9pfs_installer_fixture.py.  install.py.info's
# default tool is the Installation Utility, so the wizard launches
# from a Workbench double-click with the drawer as current directory.
# drawer.info becomes the archive-root drawer icon.
DOCKER_IMAGE ?= walkero/amigagccondocker:os4-gcc11
DOCKER_RUN    = docker run --rm -v "$(CURDIR):/work" -w /work $(DOCKER_IMAGE)

DIST_DIR = $(BUILD_DIR)/Virtio9PFS
DIST_NAME = Virtio9PFS_$(shell grep HANDLER_VERSION include/version.h | head -1 | awk '{print $$3}').$(shell grep HANDLER_REVISION include/version.h | head -1 | awk '{print $$3}').$(shell grep HANDLER_BUILD include/version.h | head -1 | awk '{print $$3}')-beta
DIST_ARCHIVE = $(BUILD_DIR)/$(DIST_NAME).lha

dist: all debug test
	rm -rf $(DIST_DIR)
	mkdir -p $(DIST_DIR)/content
	cp $(TARGET) $(DIST_DIR)/content/Virtio9PFS-handler
	cp DOSDriver/SHARED $(DIST_DIR)/content/SHARED
	cp $(TARGET_DEBUG) $(DIST_DIR)/Virtio9PFS-handler.debug
	cp $(TEST_TARGET) $(DIST_DIR)/test_9p
	cp installer/install.py $(DIST_DIR)/install.py
	cp installer/install.py.info $(DIST_DIR)/install.py.info
	cp installer/Virtio9PFSInstallerLocale.py $(DIST_DIR)/Virtio9PFSInstallerLocale.py
	cp installer/drawer.info $(BUILD_DIR)/Virtio9PFS.info
	cp README.txt $(DIST_DIR)/README
	cp CHANGELOG.md $(DIST_DIR)/CHANGELOG
	@echo "=== Staged distribution drawer ==="
	@find $(DIST_DIR) $(BUILD_DIR)/Virtio9PFS.info -type f | sort

dist-lha: dist
	rm -f $(DIST_ARCHIVE)
	@if command -v lha >/dev/null 2>&1; then \
	    (cd $(BUILD_DIR) && lha ao5q $(DIST_NAME).lha Virtio9PFS Virtio9PFS.info); \
	else \
	    echo "lha not on PATH — packing inside Docker"; \
	    $(DOCKER_RUN) sh -c 'cd $(BUILD_DIR) && lha ao5q /work/$(DIST_ARCHIVE) Virtio9PFS Virtio9PFS.info'; \
	fi
	@ls -la $(DIST_ARCHIVE)

clean:
	rm -rf $(BUILD_DIR)

# Dev convenience: copy the build products into the QEMU shared folder
# (mapped into the guest as virtfs).  Machine-specific — override
# SHARED_DIR as needed.
install: all debug test
	cp $(TARGET) $(SHARED_DIR)/Virtio9PFS-handler
	cp $(TARGET_DEBUG) $(SHARED_DIR)/Virtio9PFS-handler.debug
	cp DOSDriver/SHARED $(SHARED_DIR)/SHARED.DOSDriver
	cp $(TEST_TARGET) $(SHARED_DIR)/test_9p

help:
	@echo "Virtio9PFS-handler build system"
	@echo ""
	@echo "  make all       - release handler (default)"
	@echo "  make debug     - debug handler (DPRINTF active)"
	@echo "  make test      - AmigaOS test program"
	@echo "  make dist      - stage release drawer in $(DIST_DIR)/"
	@echo "  make dist-lha  - pack the drawer into $(DIST_ARCHIVE)"
	@echo "  make install   - copy builds to \$$(SHARED_DIR) (dev only)"
	@echo "  make clean     - remove $(BUILD_DIR)/"

.PHONY: all debug clean install test dist dist-lha help

# Pull in auto-generated header dependency files (-MMD -MP).  '-' suppresses
# 'no such file' on first build before any .d exists.
-include $(OBJ:.o=.d) $(OBJ_DEBUG:.o=.d)
