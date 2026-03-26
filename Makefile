CC = ppc-amigaos-gcc
STRIP = ppc-amigaos-strip
CFLAGS_COMMON = -Wall -D__AMIGAOS4__ -U__USE_INLINE__ -I./include -fno-tree-loop-distribute-patterns
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

DIST_DIR = $(BUILD_DIR)/Virtio9PFS
DIST_NAME = Virtio9PFS_$(shell grep HANDLER_VERSION include/version.h | head -1 | awk '{print $$3}').$(shell grep HANDLER_REVISION include/version.h | head -1 | awk '{print $$3}').$(shell grep HANDLER_BUILD include/version.h | head -1 | awk '{print $$3}')-beta
DIST_ARCHIVE = $(BUILD_DIR)/$(DIST_NAME).lha

dist: all debug test
	rm -rf $(DIST_DIR)
	mkdir -p $(DIST_DIR)
	cp $(TARGET) $(DIST_DIR)/Virtio9PFS-handler
	cp $(TARGET_DEBUG) $(DIST_DIR)/Virtio9PFS-handler.debug
	cp $(TEST_TARGET) $(DIST_DIR)/test_9p
	cp DOSDriver/SHARED $(DIST_DIR)/SHARED
	cp install.sh $(DIST_DIR)/install.sh
	cp README.txt $(DIST_DIR)/README
	cp CHANGELOG.md $(DIST_DIR)/CHANGELOG
	cd $(BUILD_DIR) && lha -c $(DIST_NAME).lha Virtio9PFS/
	rm -rf $(DIST_DIR)
	@echo "Created $(DIST_ARCHIVE)"

clean:
	rm -rf $(BUILD_DIR)

install: all debug test
	cp $(TARGET) $(SHARED_DIR)/Virtio9PFS-handler
	cp $(TARGET_DEBUG) $(SHARED_DIR)/Virtio9PFS-handler.debug
	cp DOSDriver/SHARED $(SHARED_DIR)/SHARED.DOSDriver
	cp $(TEST_TARGET) $(SHARED_DIR)/test_9p

.PHONY: all debug clean install test dist
