# Cortex (AI IDE) build.
#
#   Windows (w64devkit or any MinGW-w64 g++):
#     build.bat            release build -> build/AI-IDE.exe
#   macOS (Xcode command line tools, GLFW; drawn with Metal):
#     make                 -> build/cortex
#     make app             -> build/Cortex.app
#     make dmg             -> build/Cortex-<version>.dmg
#   Linux (GLFW, libcurl; drawn with OpenGL 3):
#     make                 -> build/cortex
#   Everywhere:
#     make DEBUG=1         debug build
#     make test            build and run the editor tests
#
# The platform is read from the compiler, so cross-compiling for Windows from
# another system works with CXX=x86_64-w64-mingw32-g++.

CXX      ?= g++
BUILD    := build
VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0)

IMGUI    := vendor/imgui
INCLUDES := -Isrc -I$(IMGUI) -I$(IMGUI)/backends
WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

MACHINE := $(shell $(CXX) -dumpmachine)
ifneq (,$(findstring mingw,$(MACHINE)))
  PLATFORM := windows
else ifneq (,$(findstring apple,$(MACHINE)))
  PLATFORM := macos
else
  PLATFORM := linux
endif

IMGUI_CORE := $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp \
              $(IMGUI)/imgui_widgets.cpp
# Every source file builds everywhere: the ones for another platform compile
# to nothing (sys.cpp is Windows only, sys_posix.cpp the rest, and so on).
APP_SRC := $(wildcard src/*.cpp)
MM_SRC  :=

ifeq ($(PLATFORM),windows)
  # ---------------------------------------------------------------- Windows
  TARGET   := $(BUILD)/AI-IDE.exe
  CXXFLAGS := -std=c++17 -municode -DUNICODE -D_UNICODE $(INCLUDES) $(WARN) -MMD -MP
  ifdef DEBUG
    CXXFLAGS += -g -O0
    LDFLAGS  := -municode
  else
    CXXFLAGS += -O2 -DNDEBUG
    LDFLAGS  := -municode -mwindows -s
  endif
  # Static runtime so the .exe runs on any Windows machine without extra DLLs.
  LDFLAGS += -static -static-libgcc -static-libstdc++
  LIBS    := -ld3d11 -ldxgi -ld3dcompiler -lwinhttp -lws2_32 -ldwmapi -lole32 -loleaut32 \
             -luuid -lshell32 -lshlwapi -lgdi32 -limm32 -luser32 -ladvapi32
  IMGUI_SRC := $(IMGUI_CORE) $(IMGUI)/backends/imgui_impl_win32.cpp \
               $(IMGUI)/backends/imgui_impl_dx11.cpp
  TEST_LIBS := -static -static-libgcc -static-libstdc++ -luser32 -limm32
else
  # ---------------------------------------------------------- macOS / Linux
  TARGET   := $(BUILD)/cortex
  CXXFLAGS := -std=c++17 $(INCLUDES) $(WARN) -MMD -MP
  ifdef DEBUG
    CXXFLAGS += -g -O0
  else
    CXXFLAGS += -O2 -DNDEBUG
  endif
  IMGUI_SRC := $(IMGUI_CORE) $(IMGUI)/backends/imgui_impl_glfw.cpp

  # GLFW comes from pkg-config, or from GLFW_PREFIX when it was built by hand
  # (the release build uses a static, universal copy so the app needs nothing
  # installed).
  ifdef GLFW_PREFIX
    CXXFLAGS  += -I$(GLFW_PREFIX)/include
    GLFW_LIBS := $(GLFW_PREFIX)/lib/libglfw3.a
  else
    CXXFLAGS  += $(shell pkg-config --cflags glfw3 2>/dev/null)
    GLFW_LIBS := $(shell pkg-config --libs glfw3 2>/dev/null || echo -lglfw)
  endif

  ifeq ($(PLATFORM),macos)
    # ARCHS="arm64 x86_64" builds one binary for Apple silicon and Intel.
    MACOS_MIN  := 11.0
    ARCH_FLAGS := $(foreach a,$(ARCHS),-arch $(a)) -mmacosx-version-min=$(MACOS_MIN)
    CXXFLAGS   += $(ARCH_FLAGS) -DGL_SILENCE_DEPRECATION
    LDFLAGS    := $(ARCH_FLAGS)
    # Drawn with Metal: main_mac.mm and ImGui's Metal backend.
    MM_SRC     := src/main_mac.mm src/sys_mac.mm $(IMGUI)/backends/imgui_impl_metal.mm
    # Metal, plus what a static GLFW needs from the system.
    MAC_FRAMEWORKS := -framework Cocoa -framework Metal -framework QuartzCore -framework IOKit \
                      -framework CoreFoundation -framework CoreVideo -framework OpenGL
    LIBS       := $(GLFW_LIBS) -lcurl $(MAC_FRAMEWORKS)
    TEST_LIBS  := $(ARCH_FLAGS)
  else
    # Drawn with OpenGL 3: main_glfw.cpp and ImGui's OpenGL backend.
    IMGUI_SRC  += $(IMGUI)/backends/imgui_impl_opengl3.cpp
    LDFLAGS    :=
    LIBS       := $(GLFW_LIBS) -lGL -lcurl -lpthread -ldl
    TEST_LIBS  :=
  endif
endif

OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(APP_SRC) $(IMGUI_SRC)) $(patsubst %.mm,$(BUILD)/%.o,$(MM_SRC))
# Without these, editing a header leaves stale objects linked against the old
# struct layouts — which shows up as scrambled colours rather than as an error.
DEP := $(OBJ:.o=.d)

# The editor tests need only the editor, the language table, the UI palette
# and the ImGui core: no window and no GPU.
TEST_TARGET := $(BUILD)/test_editor.exe
TEST_SRC := tests/test_editor.cpp src/texteditor.cpp src/lang.cpp src/ui.cpp $(IMGUI_CORE)
TEST_OBJ := $(patsubst %.cpp,$(BUILD)/test/%.o,$(TEST_SRC))
TEST_CXXFLAGS := -std=c++17 $(INCLUDES) -O1 -Wall -Wno-unused-parameter -MMD -MP $(ARCH_FLAGS)
DEP += $(TEST_OBJ:.o=.d)

.PHONY: all clean run test app dmg

all: $(TARGET)

$(TARGET): $(OBJ)
	@echo [link] $@
	@$(CXX) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)
	@echo Built $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo [c++ ] $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.mm
	@mkdir -p $(dir $@)
	@echo [objc] $<
	@$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

run: $(TARGET)
	@$(TARGET)

$(BUILD)/test/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo [test] $<
	@$(CXX) $(TEST_CXXFLAGS) -c $< -o $@

$(TEST_TARGET): $(TEST_OBJ)
	@echo [link] $@
	@$(CXX) $(TEST_OBJ) -o $@ $(TEST_LIBS)

test: $(TEST_TARGET)
	@$(TEST_TARGET)

# ------------------------------------------------------------ macOS bundle
APP := $(BUILD)/Cortex.app
DMG := $(BUILD)/Cortex-$(VERSION).dmg

app: $(APP)

# The icon is drawn from macos/icon.png with the system's own tools; the app
# is signed ad hoc, which Apple silicon requires before it runs at all.
$(APP): $(TARGET) macos/Info.plist macos/icon.png VERSION
	@echo [app ] $@
	@rm -rf $@ $(BUILD)/Cortex.iconset
	@mkdir -p $@/Contents/MacOS $@/Contents/Resources $(BUILD)/Cortex.iconset
	@cp $(TARGET) $@/Contents/MacOS/cortex
	@sed "s/@VERSION@/$(VERSION)/g" macos/Info.plist > $@/Contents/Info.plist
	@for s in 16 32 128 256 512; do \
	    sips -z $$s $$s macos/icon.png --out $(BUILD)/Cortex.iconset/icon_$${s}x$${s}.png >/dev/null; \
	    sips -z $$((s*2)) $$((s*2)) macos/icon.png --out $(BUILD)/Cortex.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	@iconutil -c icns $(BUILD)/Cortex.iconset -o $@/Contents/Resources/Cortex.icns
	@codesign --force --sign - $@
	@echo Built $@

dmg: $(DMG)

# A disk image with the app next to a link to /Applications, so installing is
# one drag.
$(DMG): $(APP)
	@echo [dmg ] $@
	@rm -rf $(BUILD)/dmg $@
	@mkdir -p $(BUILD)/dmg
	@cp -R $(APP) $(BUILD)/dmg/
	@ln -s /Applications $(BUILD)/dmg/Applications
	@hdiutil create -volname "Cortex $(VERSION)" -srcfolder $(BUILD)/dmg -ov -format UDZO $@ >/dev/null
	@echo Built $@

clean:
	@rm -rf $(BUILD)
	@echo Cleaned

-include $(DEP)
