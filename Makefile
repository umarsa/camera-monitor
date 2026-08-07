CXX := /usr/bin/clang++
PKG_CONFIG := /opt/homebrew/bin/pkg-config
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS := $(shell $(PKG_CONFIG) --cflags libavformat libavcodec libavutil libswscale sdl2)
LDLIBS := $(shell $(PKG_CONFIG) --libs libavformat libavcodec libavutil libswscale sdl2)

APP := CameraMonitor.app
EXECUTABLE := $(APP)/Contents/MacOS/camera-monitor
SOURCES := main.cpp macos_window.mm
ICON := AppIcon.icns

.PHONY: all clean check distribution

all: $(EXECUTABLE)

$(EXECUTABLE): $(SOURCES) settings_bridge.h Info.plist $(ICON)
	mkdir -p "$(APP)/Contents/MacOS"
	mkdir -p "$(APP)/Contents/Resources"
	cp Info.plist "$(APP)/Contents/Info.plist"
	cp $(ICON) "$(APP)/Contents/Resources/AppIcon.icns"
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(SOURCES) -o "$@" $(LDLIBS)
	touch "$(APP)"

check: $(EXECUTABLE)
	"$(EXECUTABLE)" --self-test

distribution: $(EXECUTABLE)
	rm -rf "dist/CameraMonitor.app" "dist/CameraMonitor-Apple-Silicon.zip"
	mkdir -p dist
	/usr/bin/ditto "$(APP)" "dist/CameraMonitor.app"
	./bundle_dependencies.py "dist/CameraMonitor.app"
	/usr/bin/ditto -c -k --sequesterRsrc --keepParent \
		"dist/CameraMonitor.app" "dist/CameraMonitor-Apple-Silicon.zip"

clean:
	rm -rf "$(APP)"
