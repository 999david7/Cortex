# AI IDE
#   Windows:  build.bat (w64devkit in tools/, or any MinGW-w64 g++), or make
#   macOS:    brew install glfw && make        (make app for AI-IDE.app)
#   Linux:    install GLFW 3.3+, libcurl and OpenGL headers, then make
#
#   make DEBUG=1         debug build (with a console window on Windows)

IMGUI    := vendor/imgui
BUILD    := build
INCLUDES := -Isrc -I$(IMGUI) -I$(IMGUI)/backends

ifeq ($(OS),Windows_NT)
  PLATFORM := windows
else ifeq ($(shell uname -s),Darwin)
  PLATFORM := macos
else
  PLATFORM := linux
endif

CXX      ?= g++
WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS := -std=c++17 $(INCLUDES) $(WARNINGS) -MMD -MP

# Every file under src/ builds everywhere; the platform-specific ones compile
# to nothing on the other platforms (#ifdef _WIN32 / #ifndef _WIN32).
APP_SRC   := $(wildcard src/*.cpp)
IMGUI_SRC := $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp \
             $(IMGUI)/imgui_widgets.cpp
OBJC_SRC  :=

ifeq ($(PLATFORM),windows)
  TARGET    := $(BUILD)/AI-IDE.exe
  CXXFLAGS  += -municode -DUNICODE -D_UNICODE
  IMGUI_SRC += $(IMGUI)/backends/imgui_impl_win32.cpp $(IMGUI)/backends/imgui_impl_dx11.cpp
  ifdef DEBUG
    LDFLAGS := -municode
  else
    LDFLAGS := -municode -mwindows -s
  endif
  # Static runtime so the .exe runs on any Windows machine without extra DLLs.
  LDFLAGS += -static -static-libgcc -static-libstdc++
  LIBS    := -ld3d11 -ldxgi -ld3dcompiler -lwinhttp -lws2_32 -ldwmapi -lole32 -loleaut32 \
             -luuid -lshell32 -lshlwapi -lgdi32 -limm32 -luser32 -ladvapi32
else
  TARGET    := $(BUILD)/AI-IDE
  IMGUI_SRC += $(IMGUI)/backends/imgui_impl_glfw.cpp $(IMGUI)/backends/imgui_impl_opengl3.cpp

  # GLFW through pkg-config when it is there, otherwise straight from Homebrew.
  GLFW_CFLAGS := $(shell pkg-config --cflags glfw3 2>/dev/null)
  GLFW_LIBS   := $(shell pkg-config --libs glfw3 2>/dev/null)
  ifeq ($(strip $(GLFW_LIBS)),)
    GLFW_PREFIX := $(shell brew --prefix glfw 2>/dev/null)
    ifneq ($(strip $(GLFW_PREFIX)),)
      GLFW_CFLAGS := -I$(GLFW_PREFIX)/include
      GLFW_LIBS   := -L$(GLFW_PREFIX)/lib -lglfw
    else
      GLFW_LIBS   := -lglfw
    endif
  endif
  CXXFLAGS += $(GLFW_CFLAGS)

  ifeq ($(PLATFORM),macos)
    # libcurl, OpenGL and AppKit all ship with macOS itself.
    OBJC_SRC := src/sys_mac.mm
    CXXFLAGS += -DGL_SILENCE_DEPRECATION
    LDFLAGS  :=
    LIBS     := $(GLFW_LIBS) -lcurl -framework OpenGL -framework AppKit
  else
    LDFLAGS  :=
    CXXFLAGS += $(shell pkg-config --cflags libcurl 2>/dev/null)
    LIBS     := $(GLFW_LIBS) $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl) \
                -lGL -lpthread -ldl
  endif
endif

ifdef DEBUG
  CXXFLAGS += -g -O0
else
  CXXFLAGS += -O2 -DNDEBUG
endif

OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(APP_SRC) $(IMGUI_SRC)) \
       $(patsubst %.mm,$(BUILD)/%.o,$(OBJC_SRC))
# Without these, editing a header leaves stale objects linked against the old
# struct layouts — which shows up as scrambled colours rather than as an error.
DEP := $(OBJ:.o=.d)

.PHONY: all clean run app

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
	@./$(TARGET)

# macOS: wrap the binary in an application bundle, so it can live in
# /Applications, sit in the Dock and be opened from Finder.
APP_BUNDLE := $(BUILD)/AI-IDE.app
app: $(TARGET)
ifeq ($(PLATFORM),macos)
	@rm -rf $(APP_BUNDLE)
	@mkdir -p $(APP_BUNDLE)/Contents/MacOS $(APP_BUNDLE)/Contents/Resources
	@cp $(TARGET) $(APP_BUNDLE)/Contents/MacOS/AI-IDE
	@cp packaging/macos/Info.plist $(APP_BUNDLE)/Contents/Info.plist
	@echo Built $(APP_BUNDLE)
else
	@echo "make app builds a macOS .app bundle; on this platform just use $(TARGET)"
endif

clean:
	@rm -rf $(BUILD)
	@echo Cleaned

-include $(DEP)
