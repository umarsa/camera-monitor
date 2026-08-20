#include "settings_bridge.h"

#include <SDL.h>
#include <SDL_syswm.h>

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

std::atomic<bool> gSettingsRequested{false};

NSWindow *nativeWindow(SDL_Window *window) {
    if (!window) return nil;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE) return nil;
    return info.info.cocoa.window;
}

NSTextField *label(NSString *text, NSRect frame) {
    NSTextField *field = [[NSTextField alloc] initWithFrame:frame];
    field.stringValue = text;
    field.editable = NO;
    field.selectable = NO;
    field.bordered = NO;
    field.drawsBackground = NO;
    return [field autorelease];
}

NSString *trimmed(NSString *value) {
    return [value stringByTrimmingCharactersInSet:
                      [NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

void drawLayoutCell(NSRect frame, NSColor *colour) {
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:frame
                                                        xRadius:3
                                                        yRadius:3];
    [colour setFill];
    [path fill];
}

NSAttributedString *developerCredit(NSFont *font) {
    NSString *text = @"Developed by Umar Salim  ·  Website  ·  GitHub";
    NSMutableAttributedString *credit = [[[NSMutableAttributedString alloc]
        initWithString:text
            attributes:@{
                NSFontAttributeName: font,
                NSForegroundColorAttributeName: [NSColor secondaryLabelColor],
            }] autorelease];

    NSDictionary *linkAttributes = @{
        NSForegroundColorAttributeName: [NSColor linkColor],
        NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
    };
    NSRange website = [text rangeOfString:@"Website"];
    [credit addAttributes:linkAttributes range:website];
    [credit addAttribute:NSLinkAttributeName
                   value:[NSURL URLWithString:@"https://umarsalim.com/"]
                   range:website];
    NSRange github = [text rangeOfString:@"GitHub"];
    [credit addAttributes:linkAttributes range:github];
    [credit addAttribute:NSLinkAttributeName
                   value:[NSURL URLWithString:@"https://github.com/umarsa"]
                   range:github];
    return credit;
}

}  // namespace

@interface CameraMonitorMenuTarget : NSObject
- (void)requestSettings:(id)sender;
- (void)showAbout:(id)sender;
@end

@implementation CameraMonitorMenuTarget
- (void)requestSettings:(id)sender {
    (void)sender;
    gSettingsRequested.store(true);
}

- (void)showAbout:(id)sender {
    (void)sender;
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionCredits:
            developerCredit([NSFont systemFontOfSize:12]),
    }];
}
@end

@interface CameraLayoutButton : NSButton
@end

@implementation CameraLayoutButton

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.buttonType = NSButtonTypePushOnPushOff;
    self.bordered = NO;
    self.focusRingType = NSFocusRingTypeExterior;
    return self;
}

- (void)setState:(NSControlStateValue)state {
    [super setState:state];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const BOOL selected = self.state == NSControlStateValueOn;
    const BOOL highlighted = self.highlighted;
    NSRect card = NSInsetRect(self.bounds, 1, 1);
    NSBezierPath *background = [NSBezierPath bezierPathWithRoundedRect:card
                                                              xRadius:9
                                                              yRadius:9];

    NSColor *fill = selected
        ? [[NSColor controlAccentColor] colorWithAlphaComponent:highlighted ? 0.42 : 0.30]
        : [[NSColor controlBackgroundColor] colorWithAlphaComponent:highlighted ? 0.72 : 0.48];
    [fill setFill];
    [background fill];

    NSColor *border = selected
        ? [[NSColor controlAccentColor] colorWithAlphaComponent:0.95]
        : [[NSColor separatorColor] colorWithAlphaComponent:0.80];
    [border setStroke];
    background.lineWidth = selected ? 1.5 : 1.0;
    [background stroke];

    NSColor *iconColour = selected
        ? [[NSColor selectedControlTextColor] colorWithAlphaComponent:0.90]
        : [[NSColor secondaryLabelColor] colorWithAlphaComponent:0.92];
    const CGFloat centreX = NSMidX(self.bounds);
    if (self.tag == CAMERA_MONITOR_LAYOUT_VERTICAL) {
        drawLayoutCell(NSMakeRect(centreX - 14, 51, 28, 13), iconColour);
        drawLayoutCell(NSMakeRect(centreX - 14, 35, 28, 13), iconColour);
    } else if (self.tag == CAMERA_MONITOR_LAYOUT_GRID) {
        drawLayoutCell(NSMakeRect(centreX - 16, 50, 14, 14), iconColour);
        drawLayoutCell(NSMakeRect(centreX + 2, 50, 14, 14), iconColour);
        drawLayoutCell(NSMakeRect(centreX - 16, 32, 14, 14), iconColour);
        drawLayoutCell(NSMakeRect(centreX + 2, 32, 14, 14), iconColour);
    } else {
        drawLayoutCell(NSMakeRect(centreX - 29, 36, 27, 28), iconColour);
        drawLayoutCell(NSMakeRect(centreX + 2, 36, 27, 28), iconColour);
    }

    NSMutableParagraphStyle *paragraph = [[[NSMutableParagraphStyle alloc] init]
        autorelease];
    paragraph.alignment = NSTextAlignmentCenter;
    NSDictionary *attributes = @{
        NSFontAttributeName: [NSFont systemFontOfSize:13
                                              weight:selected ? NSFontWeightSemibold
                                                              : NSFontWeightRegular],
        NSForegroundColorAttributeName: selected ? [NSColor labelColor]
                                                  : [NSColor secondaryLabelColor],
        NSParagraphStyleAttributeName: paragraph,
    };
    [self.title drawInRect:NSMakeRect(8, 9, self.bounds.size.width - 16, 18)
            withAttributes:attributes];
}

@end

@interface CameraSettingsController : NSObject <NSWindowDelegate> {
    CameraMonitorSettings *_settings;
    NSWindow *_window;
    NSSegmentedControl *_cameraCount;
    NSMutableArray *_rowLabels;
    NSMutableArray *_nameFields;
    NSMutableArray *_urlFields;
    NSMutableArray *_delayPopups;
    NSMutableArray *_layoutButtons;
    NSInteger _selectedLayout;
    BOOL _accepted;
}
- (instancetype)initWithSettings:(CameraMonitorSettings *)settings;
- (NSInteger)runModal;
@end

@implementation CameraSettingsController

- (instancetype)initWithSettings:(CameraMonitorSettings *)settings {
    self = [super init];
    if (!self) return nil;

    _settings = settings;
    _selectedLayout = settings->layout;
    _accepted = NO;
    _rowLabels = [[NSMutableArray alloc] init];
    _nameFields = [[NSMutableArray alloc] init];
    _urlFields = [[NSMutableArray alloc] init];
    _delayPopups = [[NSMutableArray alloc] init];
    _layoutButtons = [[NSMutableArray alloc] init];

    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 760, 560)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Camera Monitor Settings";
    _window.delegate = self;
    _window.releasedWhenClosed = NO;
    [_window center];

    NSView *content = _window.contentView;
    NSTextField *intro = label(
        @"Choose how many RTSP cameras to display and how they are arranged.",
        NSMakeRect(24, 516, 712, 20));
    intro.textColor = [NSColor secondaryLabelColor];
    [content addSubview:intro];

    NSTextField *cameraSection = label(@"Cameras", NSMakeRect(24, 472, 150, 24));
    cameraSection.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    [content addSubview:cameraSection];
    _cameraCount = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(190, 470, 300, 28)];
    _cameraCount.segmentCount = 4;
    _cameraCount.selectedSegmentBezelColor = [NSColor controlAccentColor];
    for (NSInteger index = 0; index < 4; ++index) {
        [_cameraCount setLabel:[NSString stringWithFormat:@"%ld", index + 1]
                    forSegment:index];
    }
    _cameraCount.selectedSegment = std::max(0, std::min(3, settings->cameraCount - 1));
    _cameraCount.target = self;
    _cameraCount.action = @selector(cameraCountChanged:);
    [content addSubview:_cameraCount];

    NSTextField *layoutSection = label(@"Layout", NSMakeRect(24, 397, 150, 24));
    layoutSection.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    [content addSubview:layoutSection];
    NSArray *layoutNames = @[@"Vertical", @"Grid", @"Horizontal"];
    for (NSInteger layout = 0; layout < 3; ++layout) {
        CameraLayoutButton *button = [[CameraLayoutButton alloc]
            initWithFrame:NSMakeRect(190 + layout * 150, 344, 136, 92)];
        button.title = layoutNames[layout];
        button.tag = layout;
        button.target = self;
        button.action = @selector(layoutSelected:);
        [_layoutButtons addObject:button];
        [content addSubview:button];
        [button release];
    }

    NSTextField *nameHeader = label(@"Camera name", NSMakeRect(55, 322, 130, 20));
    nameHeader.textColor = [NSColor secondaryLabelColor];
    [content addSubview:nameHeader];
    NSTextField *urlHeader = label(@"RTSP URL", NSMakeRect(200, 322, 420, 20));
    urlHeader.textColor = [NSColor secondaryLabelColor];
    [content addSubview:urlHeader];
    NSTextField *delayHeader = label(@"Delay", NSMakeRect(634, 322, 102, 20));
    delayHeader.textColor = [NSColor secondaryLabelColor];
    [content addSubview:delayHeader];

    for (NSInteger index = 0; index < CAMERA_MONITOR_MAX_CAMERAS; ++index) {
        const CGFloat y = 282 - index * 48;
        NSTextField *rowLabel = label([NSString stringWithFormat:@"%ld", index + 1],
                                      NSMakeRect(24, y + 3, 24, 22));
        [_rowLabels addObject:rowLabel];
        [content addSubview:rowLabel];

        NSTextField *nameField = [[NSTextField alloc]
            initWithFrame:NSMakeRect(55, y, 130, 26)];
        NSString *name = [NSString stringWithUTF8String:settings->cameras[index].name];
        nameField.stringValue = name ? name : @"";
        nameField.placeholderString = [NSString stringWithFormat:@"Camera %ld", index + 1];
        [_nameFields addObject:nameField];
        [content addSubview:nameField];
        [nameField release];

        NSTextField *urlField = [[NSTextField alloc]
            initWithFrame:NSMakeRect(200, y, 420, 26)];
        NSString *url = [NSString stringWithUTF8String:settings->cameras[index].url];
        urlField.stringValue = url ? url : @"";
        urlField.placeholderString = @"rtsp://camera-address/stream";
        [_urlFields addObject:urlField];
        [content addSubview:urlField];
        [urlField release];

        NSPopUpButton *delayPopup = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(634, y, 102, 26) pullsDown:NO];
        NSArray *delayNames = @[@"Live", @"2 sec", @"5 sec", @"10 sec",
                                @"15 sec", @"30 sec"];
        const NSInteger delayValues[] = {0, 2, 5, 10, 15, 30};
        [delayPopup addItemsWithTitles:delayNames];
        for (NSInteger delayIndex = 0; delayIndex < 6; ++delayIndex) {
            [delayPopup itemAtIndex:delayIndex].tag = delayValues[delayIndex];
        }
        [delayPopup selectItemWithTag:settings->cameras[index].delaySeconds];
        [_delayPopups addObject:delayPopup];
        [content addSubview:delayPopup];
        [delayPopup release];
    }

    NSBox *separator = [[NSBox alloc] initWithFrame:NSMakeRect(24, 122, 712, 1)];
    separator.boxType = NSBoxSeparator;
    [content addSubview:separator];
    [separator release];

    NSTextField *recovery = label(@"", NSMakeRect(24, 94, 570, 20));
    NSMutableAttributedString *recoveryText = [[[NSMutableAttributedString alloc]
        initWithString:@"●  Each camera reconnects automatically if its stream stalls or drops."]
        autorelease];
    [recoveryText addAttribute:NSFontAttributeName
                         value:[NSFont systemFontOfSize:11]
                         range:NSMakeRange(0, recoveryText.length)];
    [recoveryText addAttribute:NSForegroundColorAttributeName
                         value:[NSColor systemGreenColor]
                         range:NSMakeRange(0, 1)];
    [recoveryText addAttribute:NSForegroundColorAttributeName
                         value:[NSColor secondaryLabelColor]
                         range:NSMakeRange(1, recoveryText.length - 1)];
    recovery.attributedStringValue = recoveryText;
    [content addSubview:recovery];

    NSTextField *privacy = label(
        @"Camera URLs are saved only on this Mac in a private application-support file.",
        NSMakeRect(24, 66, 570, 20));
    privacy.textColor = [NSColor secondaryLabelColor];
    privacy.font = [NSFont systemFontOfSize:11];
    [content addSubview:privacy];

    NSTextField *developer = label(@"", NSMakeRect(24, 45, 430, 18));
    developer.selectable = YES;
    developer.allowsEditingTextAttributes = YES;
    developer.attributedStringValue = developerCredit([NSFont systemFontOfSize:11]);
    [content addSubview:developer];

    NSButton *cancel = [[NSButton alloc] initWithFrame:NSMakeRect(562, 20, 84, 32)];
    cancel.title = @"Cancel";
    cancel.bezelStyle = NSBezelStyleRounded;
    cancel.target = self;
    cancel.action = @selector(cancel:);
    cancel.keyEquivalent = @"\x1b";
    [content addSubview:cancel];
    [cancel release];

    NSButton *save = [[NSButton alloc] initWithFrame:NSMakeRect(652, 20, 84, 32)];
    save.title = @"Save";
    save.bezelStyle = NSBezelStyleRounded;
    save.target = self;
    save.action = @selector(save:);
    save.keyEquivalent = @"\r";
    [content addSubview:save];
    [save release];

    [self updateControls];
    return self;
}

- (void)dealloc {
    [_window release];
    [_cameraCount release];
    [_rowLabels release];
    [_nameFields release];
    [_urlFields release];
    [_delayPopups release];
    [_layoutButtons release];
    [super dealloc];
}

- (void)updateControls {
    const NSInteger count = _cameraCount.selectedSegment + 1;
    for (NSInteger index = 0; index < CAMERA_MONITOR_MAX_CAMERAS; ++index) {
        BOOL visible = index < count;
        [(NSTextField *)_rowLabels[index] setHidden:!visible];
        [(NSTextField *)_nameFields[index] setHidden:!visible];
        [(NSTextField *)_urlFields[index] setHidden:!visible];
        [(NSPopUpButton *)_delayPopups[index] setHidden:!visible];
    }
    for (NSButton *button in _layoutButtons) {
        button.state = button.tag == _selectedLayout ? NSControlStateValueOn
                                                      : NSControlStateValueOff;
    }
}

- (void)cameraCountChanged:(id)sender {
    (void)sender;
    [self updateControls];
}

- (void)layoutSelected:(NSButton *)sender {
    _selectedLayout = sender.tag;
    [self updateControls];
}

- (void)save:(id)sender {
    (void)sender;
    const NSInteger count = _cameraCount.selectedSegment + 1;
    for (NSInteger index = 0; index < count; ++index) {
        NSString *url = trimmed([(NSTextField *)_urlFields[index] stringValue]);
        NSString *lower = url.lowercaseString;
        if (![lower hasPrefix:@"rtsp://"] && ![lower hasPrefix:@"rtsps://"]) {
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = [NSString stringWithFormat:
                @"Camera %ld needs an RTSP URL", index + 1];
            alert.informativeText =
                @"Enter a URL beginning with rtsp:// or rtsps://.";
            [alert runModal];
            [alert release];
            [_window makeFirstResponder:_urlFields[index]];
            return;
        }
    }

    std::memset(_settings, 0, sizeof(*_settings));
    _settings->cameraCount = static_cast<int>(count);
    _settings->layout = static_cast<int>(_selectedLayout);
    for (NSInteger index = 0; index < CAMERA_MONITOR_MAX_CAMERAS; ++index) {
        NSString *name = trimmed([(NSTextField *)_nameFields[index] stringValue]);
        if (name.length == 0) {
            name = [NSString stringWithFormat:@"Camera %ld", index + 1];
        }
        NSString *url = trimmed([(NSTextField *)_urlFields[index] stringValue]);
        std::snprintf(_settings->cameras[index].name,
                      CAMERA_MONITOR_NAME_CAPACITY, "%s", name.UTF8String);
        std::snprintf(_settings->cameras[index].url,
                      CAMERA_MONITOR_URL_CAPACITY, "%s", url.UTF8String);
        _settings->cameras[index].delaySeconds = static_cast<int>(
            [(NSPopUpButton *)_delayPopups[index] selectedItem].tag);
    }

    _accepted = YES;
    [NSApp stopModalWithCode:NSModalResponseOK];
    [_window orderOut:nil];
}

- (void)cancel:(id)sender {
    (void)sender;
    [NSApp stopModalWithCode:NSModalResponseCancel];
    [_window orderOut:nil];
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    [self cancel:nil];
    return NO;
}

- (NSInteger)runModal {
    [NSApp activateIgnoringOtherApps:YES];
    [_window makeKeyAndOrderFront:nil];
    [NSApp runModalForWindow:_window];
    return _accepted ? NSModalResponseOK : NSModalResponseCancel;
}

@end

extern "C" void configureMacWindowAspect(SDL_Window *window, double width,
                                           double height) {
    NSWindow *macWindow = nativeWindow(window);
    if (!macWindow) return;
    macWindow.contentAspectRatio = NSMakeSize(width, height);
}

extern "C" void configureMacApplicationMenu(void) {
    static CameraMonitorMenuTarget *target = nil;
    if (target) return;
    target = [[CameraMonitorMenuTarget alloc] init];

    NSMenu *mainMenu = NSApp.mainMenu;
    if (!mainMenu) return;
    NSMenuItem *applicationItem = mainMenu.itemArray.firstObject;
    NSMenu *applicationMenu = applicationItem.submenu;
    if (!applicationMenu) return;

    NSMenuItem *about = nil;
    NSMenuItem *settings = nil;
    for (NSMenuItem *item in applicationMenu.itemArray) {
        if ([item.title hasPrefix:@"About"]) about = item;
        if ([item.keyEquivalent isEqualToString:@","] ||
            [item.title hasPrefix:@"Settings"] ||
            [item.title hasPrefix:@"Preferences"]) {
            settings = item;
        }
    }
    if (about) {
        about.action = @selector(showAbout:);
        about.target = target;
        about.enabled = YES;
    }
    if (!settings) {
        settings = [[[NSMenuItem alloc] initWithTitle:@"Settings…"
                                                action:nil
                                         keyEquivalent:@","] autorelease];
        const NSInteger insertion =
            std::min<NSInteger>(1, applicationMenu.numberOfItems);
        [applicationMenu insertItem:settings atIndex:insertion];
    }
    settings.title = @"Settings…";
    settings.action = @selector(requestSettings:);
    settings.target = target;
    settings.enabled = YES;
    settings.keyEquivalentModifierMask = NSEventModifierFlagCommand;
}

// SDL does not turn trackpad pinches into SDL_MULTIGESTURE on macOS, so
// forward the native magnify gesture as one. dDist carries Apple's
// magnification delta (scale change, roughly -1..1 per gesture).
extern "C" void installMacPinchMonitor(void) {
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
                                          handler:^NSEvent *(NSEvent *event) {
        SDL_Event gesture;
        std::memset(&gesture, 0, sizeof gesture);
        gesture.type = SDL_MULTIGESTURE;
        gesture.mgesture.timestamp = SDL_GetTicks();
        gesture.mgesture.numFingers = 2;
        gesture.mgesture.dDist = static_cast<float>(event.magnification);
        SDL_PushEvent(&gesture);
        return event;
    }];
}

extern "C" int consumeMacSettingsRequest(void) {
    return gSettingsRequested.exchange(false) ? 1 : 0;
}

extern "C" int showMacSettingsDialog(SDL_Window *parent,
                                      CameraMonitorSettings *settings) {
    (void)parent;
    if (!settings) return 0;
    CameraSettingsController *controller =
        [[CameraSettingsController alloc] initWithSettings:settings];
    const NSInteger result = [controller runModal];
    [controller release];
    return result == NSModalResponseOK ? 1 : 0;
}
