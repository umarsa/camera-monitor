#ifndef CAMERA_MONITOR_SETTINGS_BRIDGE_H
#define CAMERA_MONITOR_SETTINGS_BRIDGE_H

#define CAMERA_MONITOR_MAX_CAMERAS 4
#define CAMERA_MONITOR_NAME_CAPACITY 64
#define CAMERA_MONITOR_URL_CAPACITY 2048

enum CameraMonitorLayout {
    CAMERA_MONITOR_LAYOUT_VERTICAL = 0,
    CAMERA_MONITOR_LAYOUT_GRID = 1,
    CAMERA_MONITOR_LAYOUT_HORIZONTAL = 2,
};

struct CameraMonitorCameraSetting {
    char name[CAMERA_MONITOR_NAME_CAPACITY];
    char url[CAMERA_MONITOR_URL_CAPACITY];
    int delaySeconds;
};

struct CameraMonitorSettings {
    int cameraCount;
    int layout;
    CameraMonitorCameraSetting cameras[CAMERA_MONITOR_MAX_CAMERAS];
};

struct SDL_Window;

extern "C" {
void configureMacWindowAspect(SDL_Window *window, double width, double height);
void configureMacApplicationMenu(void);
void installMacPinchMonitor(void);
int consumeMacSettingsRequest(void);
int showMacSettingsDialog(SDL_Window *parent, CameraMonitorSettings *settings);
}

#endif
