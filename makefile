#==============================================================================
# PS3 CD Info Plugin - Makefile
#==============================================================================

#------------------------------------------------------------------------------
# 工具链配置
#------------------------------------------------------------------------------
PREFIX      := /usr/local/ps3dev/ppu
CC          := $(PREFIX)/bin/ppu-gcc
STRIP       := $(PREFIX)/bin/ppu-objcopy
FSELF       := fself
SPRXLINKER  := sprxlinker
PRX_CRT     := $(PREFIX)/powerpc64-ps3-elf/lib/lv2-sprx.o

#------------------------------------------------------------------------------
# 编译选项
#------------------------------------------------------------------------------
CFLAGS  := -std=c99 -O2 -Wall -Wextra -fno-builtin
CFLAGS  += -I./include
CFLAGS  += -I$(PREFIX)/include

LDFLAGS := -L$(PREFIX)/lib
LDFLAGS += -nostartfiles
LDFLAGS += -Wl,-e,_ps3_cd_info_start
LDFLAGS += -lhttp -lhttputil -lssl -lnet -lsysfs -lsysmodule -llv2

#------------------------------------------------------------------------------
# 源文件和目标
#------------------------------------------------------------------------------
SRC_DIR     := src
INC_DIR     := include
BUILD_DIR   := build

SOURCES     := $(SRC_DIR)/main.c \
               $(SRC_DIR)/hook_net.c \
               $(SRC_DIR)/cddb_query.c \
               $(SRC_DIR)/toc_reader.c \
               $(SRC_DIR)/musicbrainz.c \
               $(SRC_DIR)/formatter.c

OBJECTS     := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDS     := $(OBJECTS:.o=.d)

TARGET_ELF  := ps3_cd_info_plugin.elf
TARGET_SPRX := ps3_cd_info_plugin.sprx

#------------------------------------------------------------------------------
# 默认目标
#------------------------------------------------------------------------------
.PHONY: all
all: $(TARGET_SPRX)

#------------------------------------------------------------------------------
# 编译规则
#------------------------------------------------------------------------------

# 创建build目录
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# 编译.c -> .o (生成依赖文件)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# 链接.o -> .elf
# 去调试信息用 ppu-objcopy 写临时文件再替换：部分 macOS 环境下
# ppu-strip 的原地 rename 会被拒绝（EPERM），objcopy 直写不受影响。
$(TARGET_ELF): $(OBJECTS)
	@echo "  LD      $@"
	@$(CC) $(CFLAGS) $(PRX_CRT) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "  STRIP   $@"
	@$(STRIP) --strip-debug $@ $@.tmp && mv $@.tmp $@
	@echo "  PRX     $@"
	@$(SPRXLINKER) $@
	@echo "  SIZE    $@"
	@ls -lh $@

# 转换.elf -> .sprx
$(TARGET_SPRX): $(TARGET_ELF)
	@echo "  FSELF   $@"
	@$(FSELF) $(TARGET_ELF) $@
	@echo "  DONE    $@"
	@ls -lh $@

#------------------------------------------------------------------------------
# 主机侧单元测试（无需 PS3 工具链，cc/clang 即可）
#------------------------------------------------------------------------------
HOST_CC     ?= cc
HOST_CFLAGS := -std=c99 -O1 -Wall -Wextra -I./include

.PHONY: test
test:
	@mkdir -p $(BUILD_DIR)
	@echo "  HOST-CC tests"
	$(HOST_CC) $(HOST_CFLAGS) tests/test_formatter.c   $(SRC_DIR)/formatter.c  -o $(BUILD_DIR)/test_formatter
	$(HOST_CC) $(HOST_CFLAGS) tests/test_json.c                                -o $(BUILD_DIR)/test_json
	$(HOST_CC) $(HOST_CFLAGS) tests/test_hook_logic.c                          -o $(BUILD_DIR)/test_hook_logic
	$(HOST_CC) $(HOST_CFLAGS) tests/test_cddb_query.c  $(SRC_DIR)/cddb_query.c -o $(BUILD_DIR)/test_cddb_query
	$(HOST_CC) $(HOST_CFLAGS) tests/test_struct_size.c                         -o $(BUILD_DIR)/test_struct_size
	@echo "  HOST-RUN tests"
	@$(BUILD_DIR)/test_formatter
	@$(BUILD_DIR)/test_json
	@$(BUILD_DIR)/test_hook_logic
	@$(BUILD_DIR)/test_cddb_query
	@$(BUILD_DIR)/test_struct_size
	@echo "  ALL TESTS PASSED"

#------------------------------------------------------------------------------
# 清理
#------------------------------------------------------------------------------
.PHONY: clean
clean:
	@echo "  CLEAN"
	@rm -rf $(BUILD_DIR) $(TARGET_ELF) $(TARGET_SPRX)

.PHONY: distclean
distclean: clean
	@rm -f *~ $(SRC_DIR)/*~ $(INC_DIR)/*~

#------------------------------------------------------------------------------
# 调试信息
#------------------------------------------------------------------------------
.PHONY: info
info:
	@echo "Project: PS3 CD Info Plugin"
	@echo "Sources: $(SOURCES)"
	@echo "Objects: $(OBJECTS)"
	@echo "Target:  $(TARGET_SPRX)"

#------------------------------------------------------------------------------
# 包含依赖文件
#------------------------------------------------------------------------------
-include $(DEPENDS)

#------------------------------------------------------------------------------
# 帮助信息
#------------------------------------------------------------------------------
.PHONY: help
help:
	@echo "PS3 CD Info Plugin Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build .sprx (default)"
	@echo "  clean      - Remove build files"
	@echo "  distclean  - Remove all generated files"
	@echo "  info       - Show build configuration"
	@echo "  help       - Show this help"
