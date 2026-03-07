CC = ppc-amigaos-gcc
CFLAGS = -O2 -Wall -D__AMIGAOS4__ -U__USE_INLINE__ -I./include -fno-tree-loop-distribute-patterns
LDFLAGS = -nostartfiles -nodefaultlibs -lgcc

BUILD_DIR = build
TARGET = $(BUILD_DIR)/Virtio9PFS-handler
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

OBJ = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRC))

# WSL2 shared folder (mapped into QEMU as virtfs + USB FAT drive)
SHARED_DIR = $(HOME)/shared

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(BUILD_DIR) $(TEST_TARGET)

$(TEST_TARGET): test/test_9p.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) test/test_9p.c -o $(TEST_TARGET) -lauto

DIST_DIR = $(BUILD_DIR)/Virtio9PFS
DIST_NAME = Virtio9PFS_$(shell grep HANDLER_VERSION include/version.h | head -1 | awk '{print $$3}').$(shell grep HANDLER_REVISION include/version.h | head -1 | awk '{print $$3}').$(shell grep HANDLER_BUILD include/version.h | head -1 | awk '{print $$3}')-beta
DIST_ARCHIVE = $(BUILD_DIR)/$(DIST_NAME).lha

dist: all
	rm -rf $(DIST_DIR)
	mkdir -p $(DIST_DIR)
	cp $(TARGET) $(DIST_DIR)/Virtio9PFS-handler
	cp DOSDriver/SHARED $(DIST_DIR)/SHARED
	cp install.sh $(DIST_DIR)/install.sh
	cp README.txt $(DIST_DIR)/README
	cd $(BUILD_DIR) && lha -c $(DIST_NAME).lha Virtio9PFS/
	rm -rf $(DIST_DIR)
	@echo "Created $(DIST_ARCHIVE)"

clean:
	rm -rf $(BUILD_DIR)

install: all test
	cp $(TARGET) $(SHARED_DIR)/Virtio9PFS-handler
	cp DOSDriver/SHARED $(SHARED_DIR)/SHARED.DOSDriver
	cp $(TEST_TARGET) $(SHARED_DIR)/test_9p

.PHONY: all clean install test dist
