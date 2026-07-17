#==============================================================================
# PS3 CD Info Plugin - Makefile
#==============================================================================

#------------------------------------------------------------------------------
# 工具链配置
#------------------------------------------------------------------------------
PREFIX      := /usr/local/ps3dev/ppu
CC          := $(PREFIX)/bin/ppu-gcc
STRIP       := $(PREFIX)/bin/ppu-strip
FSELF       := fself
SPRXLINKER  := sprxlinker

#------------------------------------------------------------------------------
# 编译选项
#------------------------------------------------------------------------------
CFLAGS  := -std=c99 -O2 -Wall -Wextra -fno-builtin
CFLAGS  += -I./include
CFLAGS  += -I$(PREFIX)/include

LDFLAGS := -L$(PREFIX)/lib
LDFLAGS += -nostartfiles
LDFLAGS += -lhttp -lhttputil -lnet -lsysfs -lsysmodule -llv2

#------------------------------------------------------------------------------
# 源文件和目标
#------------------------------------------------------------------------------
SRC_DIR     := src
INC_DIR     := include
BUILD_DIR   := build

SOURCES     := $(SRC_DIR)/main.c \
               $(SRC_DIR)/hook_net.c \
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
$(TARGET_ELF): $(OBJECTS)
	@echo "  LD      $@"
	@$(CC) $(CFLAGS) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "  SIZE    $@"
	@ls -lh $@

# 转换.elf -> .sprx
$(TARGET_SPRX): $(TARGET_ELF)
	@echo "  FSELF   $@"
	@$(FSELF) $(TARGET_ELF) $@ 2>/dev/null || \
	 $(SPRXLINKER) $(TARGET_ELF) $@
	@echo "  DONE    $@"
	@ls -lh $@

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