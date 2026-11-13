/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^WinIOSStateCallback)(void);

/// One contact of a guest touchscreen frame; x and y run 0...32767 across the Windows display.
typedef struct WinIOSTouchContact {
    uint8_t id;
    uint8_t tip;
    uint16_t x;
    uint16_t y;
} WinIOSTouchContact;

@interface WinIOSBridge : NSObject

@property (class, nonatomic, readonly) WinIOSBridge *shared;

@property (nonatomic, readonly) BOOL isJITLive;
@property (nonatomic, readonly) BOOL isDebuggerAttached;
@property (nonatomic, readonly) BOOL isVMRunning;
@property (nonatomic, readonly) BOOL isUnpacking;
@property (nonatomic, readonly) double unpackProgress;
@property (nonatomic, readonly) NSString *jitStatusText;
@property (nonatomic, readonly) NSString *vmStatusText;
@property (nonatomic, readonly) uint32_t framebufferWidth;
@property (nonatomic, readonly) uint32_t framebufferHeight;
@property (nonatomic, readonly) uint64_t presentedFrames;
@property (nonatomic, readonly) uint64_t availableMemoryMB;
/// Guest RAM the VM runs with, or the ledger-exempt pre-allocation before it starts (0 if none).
@property (nonatomic, readonly) uint64_t guestRAMMB;
@property (nonatomic, readonly) BOOL usbKeyboardReady;
@property (nonatomic, readonly) BOOL usbTabletReady;
@property (nonatomic, readonly) uint64_t usbEventsDelivered;
@property (nonatomic, assign) BOOL useSafariForGuestLinks;
@property (nonatomic, copy, nullable) WinIOSStateCallback onStateChanged;
/// What has the focus in Windows after a noted tap or look, from the WindowsiOS Tools agent, on the
/// main queue: editable when it takes text, inside when the tap landed in it.
@property (nonatomic, copy, nullable) void (^onGuestTextFocus)(BOOL editable, BOOL inside);
/// Seconds since that agent last asked for taps, or -1 if this Windows has none that reports focus.
@property (nonatomic, readonly) NSInteger textFocusAgentSeenSeconds;

- (nullable NSString *)takeGuestOpenURL;
- (BOOL)takeGuestOAuthDone;

- (void)installTrapGuard;
- (BOOL)tryAcquireJIT:(BOOL)detachAfter;

- (BOOL)hasProvisionedMachine;
- (BOOL)needsSparseUnpack;
- (void)prepareAndStartWithLayer:(nullable CAMetalLayer *)layer;
- (void)bindMetalLayer:(nullable CAMetalLayer *)layer;
/// Where the Windows picture goes in that layer, as fractions of its width and height. The rest of
/// the layer continues the picture's edge pixels. Ignored for a layer that is not the bound one.
- (void)setPictureRect:(CGRect)rect forLayer:(CAMetalLayer *)layer;

- (void)updateScreenSize:(CGSize)size scale:(CGFloat)scale;
- (void)updateHingeStatus:(NSInteger)status angleDegrees:(double)angleDegrees;
/// The window's resolved geometry as "key: value" lines, appended to Documents/boot_status.txt.
- (void)updateGeometryReport:(NSString *)report;

- (void)sendTabletNormalizedX:(double)x y:(double)y buttons:(uint8_t)buttons wheel:(int8_t)wheel;
/// The full set of contacts for one frame (at most 5), lifting contacts once with tip 0.
- (void)sendTouchContacts:(const WinIOSTouchContact *_Nullable)contacts count:(NSInteger)count;
/// Windows has configured the HID touchscreen, so raw touches reach it.
@property (nonatomic, readonly) BOOL touchscreenReady;
/// The size, in guest pixels, Windows should render at (virtio-gpu); ignored on ramfb.
- (void)requestDisplayWidth:(uint32_t)width height:(uint32_t)height;
/// Laptop deck showing (YES) or full-screen tablet (NO); Windows' slate indicator follows it.
- (void)setLaptopPosture:(BOOL)laptop;
/// The display scale Windows should run at, in percent. The WindowsiOS Tools agent writes it down and
/// Windows takes it at its next sign-in.
- (void)setWindowsScalePercent:(NSInteger)percent;
/// The scale Windows is running at, in percent, as the agent reports it; 0 until one has (Windows is
/// still starting, or its Tools are from before the report). onStateChanged follows a change.
@property (nonatomic, readonly) NSInteger windowsRunningScalePercent;
/// A tap reached Windows at this guest pixel; the agent answers through onGuestTextFocus.
- (void)noteTapAtGuestX:(int32_t)x y:(int32_t)y;
/// Nothing was tapped, but the focus may have moved (Return was typed): the agent looks again.
- (void)noteFocusLook;
- (void)sendKeyHid:(uint8_t)usage down:(BOOL)down;
- (void)tapKeyHid:(uint8_t)usage;
- (void)sendKeyComboWithModifiers:(NSArray<NSNumber *> *)modifiers key:(uint8_t)usage;
- (void)sendWindowsKey;
- (void)sendCtrlAltDel;
- (void)sendAltTab;
- (void)sendEscape;
- (void)pressPowerButton;
/// Ask Windows to hibernate (through the WindowsiOS Tools agent); the VM stops once it has saved itself.
- (void)requestHibernate;
- (void)flushDisks;
- (void)stopVM;

@end

NS_ASSUME_NONNULL_END
