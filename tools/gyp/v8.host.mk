# This file is generated by gyp; do not edit.

TOOLSET := host
TARGET := v8
### Rules for final target.
$(obj).host/v8/tools/gyp/v8.stamp: TOOLSET := $(TOOLSET)
$(obj).host/v8/tools/gyp/v8.stamp: $(obj).host/v8/tools/gyp/libv8_base.x64.a $(obj).host/v8/tools/gyp/libv8_snapshot.a FORCE_DO_CMD
	$(call do_cmd,touch)

all_deps += $(obj).host/v8/tools/gyp/v8.stamp
# Add target alias
.PHONY: v8
v8: $(obj).host/v8/tools/gyp/v8.stamp

# Add target alias to "all" target.
.PHONY: all
all: v8

