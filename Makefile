# ACZPropNav build (MinGW-w64 / clang, x86_64-windows-gnu)
CC      ?= x86_64-w64-mingw32-gcc
CXX     ?= x86_64-w64-mingw32-g++
LD      ?= x86_64-w64-mingw32-g++

BUILD   := build
SRC     := src
CXXFLAGS := -std=c++17 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter \
            -Isrc -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -MMD -MP
LDFLAGS  := -static -static-libgcc -static-libstdc++ -s

CORE_SRC := $(shell find $(SRC) -name '*.cpp' 2>/dev/null | sort)
CORE_OBJ := $(patsubst $(SRC)/%.cpp,$(BUILD)/obj/%.o,$(CORE_SRC))
CORE_DEP := $(CORE_OBJ:.o=.d)

TARGETS  := $(BUILD)/ACZPropNav.dll $(BUILD)/XINPUT1_4.dll \
            $(BUILD)/initest.exe $(BUILD)/pnsim.exe $(BUILD)/memtest.exe $(BUILD)/hooktest.exe

.PHONY: all clean test dirs
all: dirs $(TARGETS)

dirs:
	@mkdir -p $(BUILD)/obj $(BUILD)/obj/tools

$(BUILD)/obj/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/tools/%.o: tools/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---------------- the mod DLL ----------------
$(BUILD)/ACZPropNav.dll: $(CORE_OBJ) loader/exports.def
	$(LD) $(LDFLAGS) -shared -o $@ $(CORE_OBJ) loader/exports.def -lkernel32 -luser32 -lpsapi -ladvapi32

# ---------------- the loader/proxy DLL ----------------
$(BUILD)/XINPUT1_4.dll: $(BUILD)/obj/loader/xinput1_4_proxy.o loader/xinput1_4.def
	$(LD) $(LDFLAGS) -shared -o $@ $(BUILD)/obj/loader/xinput1_4_proxy.o loader/xinput1_4.def -lkernel32

# ---------------- offline tools / tests ----------------
$(BUILD)/initest.exe: $(BUILD)/obj/tools/initest/main.o $(BUILD)/obj/core/config.o $(BUILD)/obj/core/log.o
	$(LD) $(LDFLAGS) -o $@ $^ -lkernel32

$(BUILD)/pnsim.exe: $(BUILD)/obj/tools/pnsim/main.o $(BUILD)/obj/pn/pn.o
	$(LD) $(LDFLAGS) -o $@ $^ -lkernel32

$(BUILD)/memtest.exe: $(BUILD)/obj/tools/memtest/main.o $(BUILD)/obj/mem/mem.o $(BUILD)/obj/mem/discovery.o $(BUILD)/obj/core/log.o
	$(LD) $(LDFLAGS) -o $@ $^ -lkernel32 -lpsapi

$(BUILD)/hooktest.exe: $(BUILD)/obj/tools/hooktest/main.o $(BUILD)/obj/hook/hook.o
	$(LD) $(LDFLAGS) -o $@ $^ -lkernel32

test: all
	@echo "=== initest ==="; ./$(BUILD)/initest.exe
	@echo "=== pnsim ===";  ./$(BUILD)/pnsim.exe --selftest
	@echo "=== memtest ==="; ./$(BUILD)/memtest.exe
	@echo "=== hooktest ==="; ./$(BUILD)/hooktest.exe

clean:
	rm -rf $(BUILD)

-include $(CORE_DEP)
