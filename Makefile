# AI IDE - build with w64devkit in tools/ (or any MinGW-w64 g++).
#   build.bat            release build
#   make DEBUG=1         debug build with a console window

CXX      ?= g++
BUILD    := build
TARGET   := $(BUILD)/AI-IDE.exe

IMGUI    := vendor/imgui
INCLUDES := -Isrc -I$(IMGUI) -I$(IMGUI)/backends

CXXFLAGS := -std=c++17 -municode -DUNICODE -D_UNICODE $(INCLUDES) \
            -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -MMD -MP

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

APP_SRC := $(wildcard src/*.cpp)
IMGUI_SRC := $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp \
             $(IMGUI)/imgui_widgets.cpp \
             $(IMGUI)/backends/imgui_impl_win32.cpp $(IMGUI)/backends/imgui_impl_dx11.cpp

OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(APP_SRC) $(IMGUI_SRC))
# Without these, editing a header leaves stale objects linked against the old
# struct layouts — which shows up as scrambled colours rather than as an error.
DEP := $(OBJ:.o=.d)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	@echo [link] $@
	@$(CXX) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)
	@echo Built $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo [c++ ] $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	@./$(TARGET)

clean:
	@rm -rf $(BUILD)
	@echo Cleaned

-include $(DEP)
