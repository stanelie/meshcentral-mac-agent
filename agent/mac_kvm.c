/*
Copyright 2010 - 2018 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "mac_kvm.h"
#include "mac_events.h"
#include "../../meshdefines.h"
#include "../../meshinfo.h"
#include "../../../microstack/ILibParsers.h"
#include "../../../microstack/ILibAsyncSocket.h"
#include "../../../microstack/ILibAsyncServerSocket.h"
#include "../../../microstack/ILibProcessPipe.h"
#include <IOKit/IOKitLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreServices/CoreServices.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>

#include <string.h>
#include <pwd.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <SystemConfiguration/SystemConfiguration.h>

// Temporary diagnostic log for loginwindow KVM debugging
// Log path is PER-UID, and that is not cosmetic.
//
// Two kvmagents run from the same LaunchAgent: one in the console user's Aqua
// session and one as ROOT in the LoginWindow session. They used to share
// /tmp/kvm_debug.log, and whichever created it first owned it. On a Mac that
// boots to the login window -- i.e. the normal case -- root gets there first and
// creates it 0644 root:wheel, after which the user-session agent cannot append
// at all and logs NOTHING.
//
// That silently broke the installer's permission walkthrough, which reads this
// log to ask the agent whether a grant took. Measured on a deployed Mac
// 2026-09-15: Accessibility and Screen Recording were both granted (auth_value=2
// in TCC.db) and the walkthrough still reported them ungranted forever, because
// the only line in the log was "AXIsProcessTrusted=0 at startup (uid=0)" from
// the ROOT agent -- which correctly has no Accessibility in the LoginWindow
// session, and whose answer is not the one being asked about.
//
// Splitting by uid fixes both halves: no cross-uid permission collision, and a
// reader gets the agent that shares its own session rather than the last writer.
static void kvm_log_path(char *out, size_t outsz)
{
    snprintf(out, outsz, "/tmp/kvm_debug-%u.log", (unsigned)getuid());
}

static void kvm_flog(const char *fmt, ...) {
    char _p[64]; kvm_log_path(_p, sizeof(_p));
    FILE *f = fopen(_p, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fflush(f); fclose(f);
}
#include <stdarg.h>

int KVM_Listener_FD = -1;
// Socket path used when kvm_server_mainloop runs in socket-listener
// mode. Default is the system-owned path used by the legacy -kvm1
// mode (root daemon spawn case). The new user-LaunchAgent flow
// (-kvmagent) overrides this via kvm_set_listener_path() before
// entering the loop, since the user agent can't bind under
// /usr/local/mesh_services/.
static char kvm_listener_path[256] = "/usr/local/mesh_services/meshagent/kvm";

void kvm_set_listener_path(const char *path)
{
	if (path == NULL || *path == '\0') return;
	size_t len = strlen(path);
	if (len >= sizeof(kvm_listener_path)) return;
	memcpy(kvm_listener_path, path, len);
	kvm_listener_path[len] = '\0';
}
#if defined(_TLSLOG)
#define TLSLOG1 printf
#else
#define TLSLOG1(...) ;
#endif


int KVM_AGENT_FD = -1;
int KVM_SEND(char *buffer, int bufferLen)
{
	int retVal = -1;
	retVal = write(KVM_AGENT_FD == -1 ? STDOUT_FILENO : KVM_AGENT_FD, buffer, bufferLen);
	if (KVM_AGENT_FD == -1) { fsync(STDOUT_FILENO); }
	else
	{
		if (retVal < 0)
		{
			char tmp[255];
			int tmpLen = sprintf_s(tmp, sizeof(tmp), "Write Error: %d on %d\n", errno, KVM_AGENT_FD);
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}
	}
	return(retVal);
}



CGDirectDisplayID SCREEN_NUM = 0;

// ---- Multi-display ---------------------------------------------------------
// MeshCentral's protocol has had display enumeration and selection all along
// (MNG_KVM_GET_DISPLAYS / MNG_KVM_SET_DISPLAY) and the Windows and Linux agents
// both implement it. macOS never did: it hardcoded CGMainDisplayID(), so a Mac
// with two monitors only ever showed one and the viewer's monitor picker never
// appeared. The capture layer was already able to do this -- kvm_capture_sck()
// takes a displayID and enumerates every SCDisplay -- it was simply always
// handed the main one.
#define KVM_MAX_DISPLAYS 16
CGDirectDisplayID SCREEN_LIST[KVM_MAX_DISPLAYS];
int SCREEN_COUNT = 0;
int SCREEN_SEL   = 1;   // 1-based index into SCREEN_LIST; 0 = ALL displays combined

// Origin of the selected display in the global coordinate space.
//
// Mouse coordinates arrive relative to the top-left of the CAPTURED display,
// but CGEventCreateMouseEvent() takes GLOBAL coordinates. For the main display
// the origin is (0,0), which is why single-display worked without ever
// consulting this. A second monitor does not sit at the origin -- measured on
// the test VM it is at (1132,0) -- so without this every click on that monitor
// would land on the first one.
int SCREEN_ORIGIN_X = 0;
int SCREEN_ORIGIN_Y = 0;

int SH_HANDLE = 0;
int SCREEN_WIDTH = 0;
int SCREEN_HEIGHT = 0;
int SCREEN_SCALE = 1;
int SCREEN_SCALE_SET = 0;
int SCREEN_DEPTH = 0;
int TILE_WIDTH = 0;
int TILE_HEIGHT = 0;
int TILE_WIDTH_COUNT = 0;
int TILE_HEIGHT_COUNT = 0;
int COMPRESSION_RATIO = 0;
int FRAME_RATE_TIMER = 0;
struct tileInfo_t **g_tileInfo = NULL;
int g_remotepause = 0;
int g_pause = 0;
int g_shutdown = 0;
int g_resetipc = 0;
int kvm_clientProcessId = 0;
int g_restartcount = 0;
int g_totalRestartCount = 0;
int restartKvm = 0;
extern void* tilebuffer;
pid_t g_slavekvm = 0;
pthread_t kvmthread = (pthread_t)NULL;
ILibProcessPipe_Process gChildProcess;
ILibQueue g_messageQ;

//int logenabled = 1;
//FILE *logfile = NULL;
//#define MASTERLOGFILE "/dev/null"
//#define SLAVELOGFILE "/dev/null"
//#define LOGFILE "/dev/null"


#define KvmDebugLog(...)
//#define KvmDebugLog(...) printf(__VA_ARGS__); if (logfile != NULL) fprintf(logfile, __VA_ARGS__);
//#define KvmDebugLog(x) if (logenabled) printf(x);
//#define KvmDebugLog(x) if (logenabled) fprintf(logfile, "Writing from slave in kvm_send_resolution\n");

void senddebug(int val)
{
	char *buffer = (char*)ILibMemory_SmartAllocate(8);

	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_DEBUG);	// Write the type
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)8);			// Write the size
	((int*)buffer)[1] = val;

	ILibQueue_Lock(g_messageQ);
	ILibQueue_EnQueue(g_messageQ, buffer);
	ILibQueue_UnLock(g_messageQ);
}



void kvm_send_resolution() 
{
	char *buffer = ILibMemory_SmartAllocate(8);
	
	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_SCREEN);	// Write the type
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)8);				// Write the size
	((unsigned short*)buffer)[2] = (unsigned short)htons((unsigned short)SCREEN_WIDTH);		// X position
	((unsigned short*)buffer)[3] = (unsigned short)htons((unsigned short)SCREEN_HEIGHT);	// Y position


	// Write the reply to the pipe.
	ILibQueue_Lock(g_messageQ);
	ILibQueue_EnQueue(g_messageQ, buffer);
	ILibQueue_UnLock(g_messageQ);
}

// Rebuild SCREEN_LIST from the OS.
//
// Mirrored displays are skipped: they show the same pixels as the display they
// mirror, so listing them would offer the operator duplicate entries that all
// look identical.
void kvm_refresh_display_list(void)
{
	CGDirectDisplayID ids[KVM_MAX_DISPLAYS];
	uint32_t n = 0;
	if (CGGetActiveDisplayList(KVM_MAX_DISPLAYS, ids, &n) != kCGErrorSuccess) n = 0;

	SCREEN_COUNT = 0;
	for (uint32_t i = 0; i < n && SCREEN_COUNT < KVM_MAX_DISPLAYS; i++)
	{
		if (CGDisplayMirrorsDisplay(ids[i]) != kCGNullDirectDisplay) continue;
		SCREEN_LIST[SCREEN_COUNT++] = ids[i];
	}

	// Order deterministically: main display first, then left-to-right, top-to-bottom.
	//
	// CGGetActiveDisplayList does NOT promise a stable order, and this list is
	// rebuilt on every SET_DISPLAY and every advertisement. An unstable order
	// silently remaps what "Display 1" means between the moment the viewer is told
	// the list and the moment it asks to switch -- so picking a display could hand
	// back a different one, which is exactly the shape of "switching back to the
	// first monitor keeps showing the second". Sorting makes index N always mean
	// the same physical screen for as long as the layout is unchanged.
	for (int i = 1; i < SCREEN_COUNT; i++)
	{
		CGDirectDisplayID key = SCREEN_LIST[i];
		CGRect kb = CGDisplayBounds(key);
		int kmain = CGDisplayIsMain(key) ? 1 : 0;
		int j = i - 1;
		while (j >= 0)
		{
			CGDirectDisplayID cur = SCREEN_LIST[j];
			CGRect cb = CGDisplayBounds(cur);
			int cmain = CGDisplayIsMain(cur) ? 1 : 0;
			int after = 0;                        // does cur sort AFTER key?
			if (cmain != kmain)            after = (kmain > cmain);
			else if (cb.origin.x != kb.origin.x) after = (cb.origin.x > kb.origin.x);
			else                            after = (cb.origin.y > kb.origin.y);
			if (!after) break;
			SCREEN_LIST[j + 1] = cur;
			j--;
		}
		SCREEN_LIST[j + 1] = key;
	}
	if (SCREEN_COUNT == 0) { SCREEN_LIST[0] = CGMainDisplayID(); SCREEN_COUNT = 1; }
	// A display can be unplugged mid-session; fall back to the main one rather
	// than capturing an ID that no longer exists.
	if (SCREEN_SEL > SCREEN_COUNT) SCREEN_SEL = 1;
}

// Backing scale of a display, from the display itself.
//
// This used to be derived as pixelWidth / SCREEN_WIDTH, i.e. from the width of
// whatever was captured LAST. That is only valid while the display never
// changes, and it broke the moment display switching existed: going from a
// larger screen to a smaller one made it integer-divide to zero.
//
// Measured on the test VM going from display 2 back to display 1:
//     2264 / 3136 = 0   ->  SCREEN_SCALE=0
//     SCREEN_WIDTH  = CGDisplayPixelsWide * 0 = 0
//     SCREEN_HEIGHT = 0, TILE_WIDTH_COUNT = 0, TILE_HEIGHT_COUNT = 0
// so the agent captured and sent nothing at all. The viewer kept displaying the
// last good frame -- the previous monitor -- and its display buttons stopped
// responding because no further frames or resolution updates ever arrived. The
// reverse direction (small to large) happened to give 2 and looked fine, which
// is why switching one way worked.
//
// The ratio of the mode's pixel width to its point width is the actual scale
// factor and depends on nothing but the display being asked about.
static int kvm_display_scale(CGDirectDisplayID d)
{
	static CGDirectDisplayID cached_id = 0;
	static int cached_scale = 1;
	if (d == cached_id && cached_scale > 0) return cached_scale;

	int scale = 1;
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(d);
	if (mode != NULL)
	{
		size_t pw  = CGDisplayModeGetPixelWidth(mode);
		size_t ptw = CGDisplayModeGetWidth(mode);
		if (ptw > 0 && pw >= ptw) scale = (int)(pw / ptw);
		CGDisplayModeRelease(mode);
	}
	if (scale < 1) scale = 1;
	cached_id = d; cached_scale = scale;
	return scale;
}

CGDirectDisplayID kvm_selected_display(void)
{
	if (SCREEN_SEL >= 1 && SCREEN_SEL <= SCREEN_COUNT) return SCREEN_LIST[SCREEN_SEL - 1];
	return CGMainDisplayID();
}

// Is the combined "all displays" view actually in effect?
//
// Honoured only in a USER session. At the login window the composite path would
// bypass the single-display capture chain -- the SkyLight / CGWindowList / SCK
// ordering that took the longest here to get working -- and use only
// CGDisplayCreateImage plus SCK, which are exactly the two that fail there on
// some machines. The result is a black login-window screen.
//
// That is not hypothetical exposure: the viewer REMEMBERS the chosen display
// (deskPreferedStickyDisplay in default.handlebars) and re-sends it on connect,
// so once an operator picks All Displays in a user session, every later
// connection asks for it too -- including the login window.
//
// There is normally a single display at the login window in any case, so
// falling back to it there costs nothing.
static int kvm_combined_mode(void)
{
	return (SCREEN_SEL == 0 && getuid() != 0);
}

// Bounds of what is being captured, in points, in the global coordinate space.
// For a single display that is its own bounds; for ALL it is the union, whose
// origin is NOT (0,0) whenever a display sits left of or above the main one.
CGRect kvm_selected_bounds(void)
{
	if (!kvm_combined_mode()) return CGDisplayBounds(kvm_selected_display());
	CGRect u = CGRectNull;
	for (int i = 0; i < SCREEN_COUNT; i++) u = CGRectUnion(u, CGDisplayBounds(SCREEN_LIST[i]));
	if (CGRectIsNull(u)) u = CGDisplayBounds(CGMainDisplayID());
	return u;
}

// Scale to render at. Displays can differ -- measured on the test VM, one at 1x
// and one at 2x -- so a combined image takes the largest, and the smaller
// display is simply drawn scaled up into its rectangle rather than losing the
// detail of the sharper one.
int kvm_selected_scale(void)
{
	if (!kvm_combined_mode()) return kvm_display_scale(kvm_selected_display());
	int best = 1;
	for (int i = 0; i < SCREEN_COUNT; i++)
	{
		int sc = kvm_display_scale(SCREEN_LIST[i]);
		if (sc > best) best = sc;
	}
	return best;
}

// Capture every display into one image laid out as the desktop actually is.
//
// Deliberately self-contained rather than reusing the single-display fallback
// chain: that chain carries the login-window capture work (SkyLight private
// API, SCK, TCC-dependent ordering) which took the longest to get right, and a
// combined view is not worth destabilising it. The last resort here is
// CGWindowListCreateImage over CGRectInfinite, which is itself a whole-desktop
// composite -- the correct answer for this mode, just slower.
static CGImageRef kvm_capture_all_displays(void)
{
	CGRect u  = kvm_selected_bounds();
	int    sc = kvm_selected_scale();
	size_t w  = (size_t)(u.size.width  * sc);
	size_t h  = (size_t)(u.size.height * sc);
	if (w == 0 || h == 0) return NULL;

	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	if (cs == NULL) return NULL;
	CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, 0, cs,
		kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(cs);
	if (ctx == NULL) return NULL;

	int drew = 0;
	for (int i = 0; i < SCREEN_COUNT; i++)
	{
		CGDirectDisplayID d = SCREEN_LIST[i];
		CGImageRef img = CGDisplayCreateImage(d);
		if (img == NULL) { extern CGImageRef kvm_capture_sck(uint32_t); img = kvm_capture_sck((uint32_t)d); }
		if (img == NULL) continue;
		CGRect b = CGDisplayBounds(d);
		// Display bounds are top-left origin with y increasing downward; a bitmap
		// context is bottom-left with y increasing upward, so the row has to be
		// flipped or the lower monitor lands above the upper one.
		CGRect dst = CGRectMake(
			(b.origin.x - u.origin.x) * sc,
			(u.origin.y + u.size.height - (b.origin.y + b.size.height)) * sc,
			b.size.width  * sc,
			b.size.height * sc);
		CGContextDrawImage(ctx, dst, img);
		CGImageRelease(img);
		drew++;
	}

	CGImageRef out = NULL;
	if (drew > 0) out = CGBitmapContextCreateImage(ctx);
	CGContextRelease(ctx);
	if (out == NULL)
	{
		static int logged = 0;
		if (!logged) { kvm_flog("capture_all: per-display capture failed, compositing window list\n"); logged = 1; }
		out = CGWindowListCreateImage(CGRectInfinite, kCGWindowListOptionOnScreenOnly,
			kCGNullWindowID, kCGWindowImageDefault);
	}
	return out;
}

// Tell the viewer which displays exist. Wire format matches the Windows agent:
// [type][size][count][id...][selected], 2 bytes each, count = number of ids.
//
// 65535 is the "all displays as one image" entry; the viewer labels exactly that
// value "All Displays" and shows no such button unless the agent sends it.
void kvm_send_display_list(void)
{
	kvm_refresh_display_list();

	if (SCREEN_COUNT <= 1)
	{
		// One display: send the empty form, as Windows does, so the viewer hides
		// its monitor picker rather than showing a picker with one entry.
		char *buffer = ILibMemory_SmartAllocate(8);
		((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_GET_DISPLAYS);
		((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)8);
		((unsigned short*)buffer)[2] = (unsigned short)htons((unsigned short)0);
		((unsigned short*)buffer)[3] = (unsigned short)htons((unsigned short)0);
		ILibQueue_Lock(g_messageQ);
		ILibQueue_EnQueue(g_messageQ, buffer);
		ILibQueue_UnLock(g_messageQ);
		return;
	}

	// Entries: "All Displays" (65535) first, as the Windows agent sends it, then
	// one per physical display.
	int n   = SCREEN_COUNT + 1;
	int sel = (SCREEN_SEL == 0) ? 65535
	        : ((SCREEN_SEL >= 1 && SCREEN_SEL <= SCREEN_COUNT) ? SCREEN_SEL : 1);
	int sz  = 8 + (2 * n);
	char *buffer = ILibMemory_SmartAllocate(sz);
	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_GET_DISPLAYS);
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)sz);
	((unsigned short*)buffer)[2] = (unsigned short)htons((unsigned short)n);
	((unsigned short*)buffer)[3] = (unsigned short)htons((unsigned short)65535);
	for (int i = 0; i < SCREEN_COUNT; i++)
		((unsigned short*)buffer)[4 + i] = (unsigned short)htons((unsigned short)(i + 1));
	((unsigned short*)buffer)[3 + n] = (unsigned short)htons((unsigned short)sel);

	kvm_flog("kvm_send_display_list: count=%d (incl. All) selected=%d\n", n, sel);
	ILibQueue_Lock(g_messageQ);
	ILibQueue_EnQueue(g_messageQ, buffer);
	ILibQueue_UnLock(g_messageQ);
}

#define BUFSIZE 65535

int set_kbd_state(int input_state)
{
	int ret = 0;
	kern_return_t kr;
	io_service_t ios;
	io_connect_t ioc;
	CFMutableDictionaryRef mdict;

	while (1)
	{
		mdict = IOServiceMatching(kIOHIDSystemClass);
		ios = IOServiceGetMatchingService(kIOMasterPortDefault, (CFDictionaryRef)mdict);
		if (!ios)
		{
			if (mdict)
			{
				CFRelease(mdict);
			}
			ILIBLOGMESSAGEX("IOServiceGetMatchingService() failed\n");
			break;
		}

		kr = IOServiceOpen(ios, mach_task_self(), kIOHIDParamConnectType, &ioc);
		IOObjectRelease(ios);
		if (kr != KERN_SUCCESS)
		{
			ILIBLOGMESSAGEX("IOServiceOpen() failed: %x\n", kr);
			break;
		}

		// Set CAPSLOCK
		kr = IOHIDSetModifierLockState(ioc, kIOHIDCapsLockState, (input_state & 4) == 4);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}

		// Set NUMLOCK
		kr = IOHIDSetModifierLockState(ioc, kIOHIDNumLockState, (input_state & 1) == 1);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}

		// CAPSLOCK_QUERY
		bool state;
		kr = IOHIDGetModifierLockState(ioc, kIOHIDCapsLockState, &state);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}
		ret |= (state << 2);

		// NUMLOCK_QUERY
		kr = IOHIDGetModifierLockState(ioc, kIOHIDNumLockState, &state);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}
		ret |= state;

		IOServiceClose(ioc);
		break;
	}
	return(ret);
}
int get_kbd_state()
{
	int ret = 0;
	kern_return_t kr;
	io_service_t ios;
	io_connect_t ioc;
	CFMutableDictionaryRef mdict;

	while (1)
	{
		mdict = IOServiceMatching(kIOHIDSystemClass);
		ios = IOServiceGetMatchingService(kIOMasterPortDefault, (CFDictionaryRef)mdict);
		if (!ios)
		{
			if (mdict)
			{
				CFRelease(mdict);
			}
			ILIBLOGMESSAGEX("IOServiceGetMatchingService() failed\n");
			break;
		}

		kr = IOServiceOpen(ios, mach_task_self(), kIOHIDParamConnectType, &ioc);
		IOObjectRelease(ios);
		if (kr != KERN_SUCCESS)
		{
			ILIBLOGMESSAGEX("IOServiceOpen() failed: %x\n", kr);
			break;
		}

		// CAPSLOCK_QUERY
		bool state;
		kr = IOHIDGetModifierLockState(ioc, kIOHIDCapsLockState, &state);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}
		ret |= (state << 2);

		// NUMLOCK_QUERY
		kr = IOHIDGetModifierLockState(ioc, kIOHIDNumLockState, &state);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}
		ret |= state;

		IOServiceClose(ioc);
		break;
	}
	return(ret);
}


int kvm_init()
{
	ILibCriticalLogFilename = "KVMSlave.log";
	int old_height_count = TILE_HEIGHT_COUNT;

	kvm_refresh_display_list();
	SCREEN_NUM = kvm_selected_display();
	{
		// In ALL mode the captured area is the union of every display, so the
		// origin that mouse coordinates must be offset by is the union's origin,
		// which is not (0,0) whenever a display sits left of or above the main one.
		CGRect b = kvm_selected_bounds();
		SCREEN_ORIGIN_X = (int)b.origin.x;
		SCREEN_ORIGIN_Y = (int)b.origin.y;
		if (SCREEN_SEL == 0)
			kvm_flog("kvm_init: ALL %d displays, union %.0fx%.0f origin=(%d,%d)\n",
				SCREEN_COUNT, b.size.width, b.size.height, SCREEN_ORIGIN_X, SCREEN_ORIGIN_Y);
		else
			kvm_flog("kvm_init: display %d/%d id=%u origin=(%d,%d)\n",
				SCREEN_SEL, SCREEN_COUNT, SCREEN_NUM, SCREEN_ORIGIN_X, SCREEN_ORIGIN_Y);
	}
	if (__builtin_available(macOS 10.15, *))
	{
		// Check (but do NOT request) TCC screen capture access.
		// CGRequestScreenCaptureAccess() triggers a recurring permission banner
		// even when auth_value=2 is already in TCC.db; CGDisplayCreateImage works
		// via the path-based TCC entry without needing the request call.
		bool pre = CGPreflightScreenCaptureAccess();
		kvm_flog("kvm_init: display id=%u preflight=%d uid=%d\n",
			SCREEN_NUM, (int)pre, (int)getuid());
	}
	else
	{
		kvm_flog("kvm_init: display id=%u uid=%d\n", SCREEN_NUM, (int)getuid());
	}
	
	SCREEN_SCALE = kvm_selected_scale();

	kvm_flog("kvm_init: CGDisplayIsActive=%d CGDisplayIsOnline=%d\n",
		(int)CGDisplayIsActive(SCREEN_NUM), (int)CGDisplayIsOnline(SCREEN_NUM));
	// One-time diagnostic: test if screencapture can capture from this context.
	{
		static int sc_tested = 0;
		if (!sc_tested) {
			sc_tested = 1;
			unlink("/tmp/kvm_sc_test.png");
			int ret = system("/usr/sbin/screencapture -x /tmp/kvm_sc_test.png 2>/dev/null");
			struct stat st; int exists = (stat("/tmp/kvm_sc_test.png", &st) == 0);
			kvm_flog("screencapture test: ret=%d file_exists=%d size=%lld\n",
				ret, exists, (long long)(exists ? st.st_size : 0));
		}
	}
	{
		CGRect sb = kvm_selected_bounds();
		SCREEN_HEIGHT = (int)(sb.size.height * SCREEN_SCALE);
		SCREEN_WIDTH  = (int)(sb.size.width  * SCREEN_SCALE);
	}
	kvm_flog("kvm_init: SCREEN_WIDTH=%d SCREEN_HEIGHT=%d SCREEN_SCALE=%d\n", SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_SCALE);
	// Some magic numbers.
	TILE_WIDTH = 32;
	TILE_HEIGHT = 32;
	COMPRESSION_RATIO = 50;
	FRAME_RATE_TIMER = 100;
	
	TILE_HEIGHT_COUNT = SCREEN_HEIGHT / TILE_HEIGHT;
	TILE_WIDTH_COUNT = SCREEN_WIDTH / TILE_WIDTH;
	if (SCREEN_WIDTH % TILE_WIDTH) { TILE_WIDTH_COUNT++; }
	if (SCREEN_HEIGHT % TILE_HEIGHT) { TILE_HEIGHT_COUNT++; }
	
	kvm_flog("kvm_init: calling kvm_send_resolution\n");
	kvm_send_resolution();
	// Advertise the display list unprompted. The viewer does ask for it, but
	// sending it alongside the resolution means the monitor picker is populated
	// from the first frame instead of only after a round trip.
	kvm_send_display_list();
	kvm_flog("kvm_init: calling reset_tile_info\n");
	reset_tile_info(old_height_count);
	kvm_flog("kvm_init: building keystate\n");

	unsigned char *buffer = ILibMemory_SmartAllocate(5);
	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_KEYSTATE);		// Write the type
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)5);					// Write the size
	// Skip get_kbd_state() IOKit call here — IOServiceOpen(kIOHIDParamConnectType) can
	// deadlock when the kvm_mainloopinput thread is concurrently posting CGEvents via
	// kCGHIDEventTap (both contend on the IOHIDSystem kernel mutex). The client will
	// receive the correct state on the next lock-key press anyway.
	buffer[4] = 0;

	// Write the reply to the pipe.
	ILibQueue_Lock(g_messageQ);
	ILibQueue_EnQueue(g_messageQ, buffer);
	ILibQueue_UnLock(g_messageQ);
	return 0;
}

// void CheckDesktopSwitch(int checkres) { return; }

int kvm_server_inputdata(char* block, int blocklen)
{
	unsigned short type, size;
	//CheckDesktopSwitch(0);
	
	//senddebug(100+blocklen);

	// Decode the block header
	if (blocklen < 4) return 0;
	type = ntohs(((unsigned short*)(block))[0]);
	size = ntohs(((unsigned short*)(block))[1]);

	{
		char _dbuf[64];
		int _dlen = snprintf(_dbuf, sizeof(_dbuf), "inputdata: type=%d size=%d blocklen=%d\n", type, size, blocklen);
		write(STDOUT_FILENO, _dbuf, _dlen);
	}

	if (size > blocklen) return 0;

	switch (type)
	{
		case MNG_KVM_KEY_UNICODE: // Unicode Key
			if (size != 7) break;
			KeyActionUnicode(((((unsigned char)block[5]) << 8) + ((unsigned char)block[6])), block[4]);
			break;
		case MNG_KVM_KEY: // Key
		{
			// Removed `KVM_AGENT_FD != -1` skip — in our user-LaunchAgent
			// architecture (-kvmagent) the agent IS the process that
			// should handle keystrokes locally via KeyAction, since it's
			// the one in the user's GUI session with TCC permission to
			// post CGEvents.
			if (size != 6) { break; }
			KeyAction(block[5], block[4]);
			break;
		}
		case MNG_KVM_MOUSE: // Mouse
		{
			int x, y;
			short w = 0;
			// Same as above — process mouse locally in -kvmagent mode.
			if (size == 10 || size == 12)
			{
				x = ((int)ntohs(((unsigned short*)(block))[3])) / SCREEN_SCALE;
				y = ((int)ntohs(((unsigned short*)(block))[4])) / SCREEN_SCALE;
				
				if (size == 12) w = ((short)ntohs(((short*)(block))[5]));
				
				//printf("x:%d, y:%d, b:%d, w:%d\n", x, y, block[5], w);
				MouseAction(x, y, (int)(unsigned char)(block[5]), w);
			}
			break;
		}
		case MNG_KVM_COMPRESSION: // Compression
		{
			if (size != 6) break;
			set_tile_compression((int)block[4], (int)block[5]);
			COMPRESSION_RATIO = 100;
			break;
		}
		case MNG_KVM_REFRESH: // Refresh
		{
			kvm_send_resolution();

			int row, col;
			if (size != 4) break;
			if (g_tileInfo == NULL) {
				if ((g_tileInfo = (struct tileInfo_t **) malloc(TILE_HEIGHT_COUNT * sizeof(struct tileInfo_t *))) == NULL) ILIBCRITICALEXIT(254);
				for (row = 0; row < TILE_HEIGHT_COUNT; row++) {
					if ((g_tileInfo[row] = (struct tileInfo_t *) malloc(TILE_WIDTH_COUNT * sizeof(struct tileInfo_t))) == NULL) ILIBCRITICALEXIT(254);
				}
			}
			for (row = 0; row < TILE_HEIGHT_COUNT; row++) {
				for (col = 0; col < TILE_WIDTH_COUNT; col++) {
					g_tileInfo[row][col].crc = 0xFF;
					g_tileInfo[row][col].flag = 0;
				}
			}
			break;
		}
		case MNG_KVM_PAUSE: // Pause
		{
			if (size != 5) break;
			g_remotepause = block[4];
			break;
		}
		case MNG_KVM_FRAME_RATE_TIMER:
		{
			//int fr = ((int)ntohs(((unsigned short*)(block))[2]));
			//if (fr > 20 && fr < 2000) FRAME_RATE_TIMER = fr;
			break;
		}
		case MNG_KVM_GET_DISPLAYS:
		{
			kvm_send_display_list();
			break;
		}
		case MNG_KVM_SET_DISPLAY:
		{
			if (size < 6) break;
			unsigned short v = ntohs(((unsigned short*)(block))[2]);
			kvm_refresh_display_list();
			// 65535 means "all displays" in this protocol. We do not advertise
			// that option (no composite capture yet), but a viewer can still ask
			// for it -- fall back to the first display rather than blanking.
			int newsel = (v == 65535) ? 0 : (int)v;   // 0 = all displays combined
			// Log the RAW request unconditionally, before any early return. Every
			// rejection path below used to be silent, which meant a switch that did
			// nothing left no trace at all and could not be told apart from a switch
			// that was never sent.
			kvm_flog("MNG_KVM_SET_DISPLAY: raw=%u -> want=%d (have %d, current %d)\n",
				(unsigned)v, newsel, SCREEN_COUNT, SCREEN_SEL);
			if (newsel < 0 || newsel > SCREEN_COUNT) { kvm_flog("  ignored: out of range\n"); break; }
			if (newsel == SCREEN_SEL) { kvm_flog("  ignored: already selected\n"); break; }
			SCREEN_SEL = newsel;
			if (SCREEN_SEL == 0 && getuid() == 0)
			{
				kvm_flog("MNG_KVM_SET_DISPLAY: -> ALL requested at the login window; "
					"using a single display there (composite bypasses the login-window capture path)\n");
			}
			else if (SCREEN_SEL == 0)
			{
				CGRect u = kvm_selected_bounds();
				kvm_flog("MNG_KVM_SET_DISPLAY: -> ALL (%d displays, union %.0fx%.0f at %.0f,%.0f)\n",
					SCREEN_COUNT, u.size.width, u.size.height, u.origin.x, u.origin.y);
			}
			else
			{
				kvm_flog("MNG_KVM_SET_DISPLAY: -> %d/%d (id=%u)\n",
					SCREEN_SEL, SCREEN_COUNT, kvm_selected_display());
			}
			// No further work needed here: the main loop compares SCREEN_NUM with
			// the selected display every frame and already re-runs kvm_init() on a
			// change, which re-sends the resolution and rebuilds the tile cache.
			break;
		}
	}

	return size;
}


// Socket-mode (user-LaunchAgent) state. When the daemon successfully
// connects to /tmp/meshagent-kvm-<uid>.sock at session-init time,
// kvm_relay_setup() leaves these populated and the write/read paths
// fall through to the socket pipe instead of the legacy fork-exec
// helper's stdio.
static ILibProcessPipe_Pipe g_kvmSocketPipe = NULL;
static int                   g_kvmSocketFD   = -1;
static ILibKVM_WriteHandler  g_kvmSocketWriteHandler = NULL;
static void                 *g_kvmSocketReserved     = NULL;

// Not currently exported in ILibProcessPipe.h, but available from
// ILibProcessPipe.c. We use it here so socket-mode sessions are fully
// detached from the manager on KVM disconnect.
extern void ILibProcessPipe_FreePipe(void *pipeObject);

static void kvm_relay_socket_ResetState(int closePipe)
{
	if (closePipe != 0 && g_kvmSocketPipe != NULL)
	{
		ILibProcessPipe_Pipe_SetBrokenPipeHandler(g_kvmSocketPipe, NULL);
		ILibProcessPipe_FreePipe(g_kvmSocketPipe);
	}
	else if (closePipe != 0 && g_kvmSocketFD != -1)
	{
		close(g_kvmSocketFD);
	}

	g_kvmSocketPipe = NULL;
	g_kvmSocketFD = -1;
	g_kvmSocketWriteHandler = NULL;
	g_kvmSocketReserved = NULL;
}

int kvm_relay_feeddata(char* buf, int len)
{
	if (g_kvmSocketPipe != NULL)
	{
		// Socket-mode: forward bytes to the user-context LaunchAgent
		// over the connected Unix domain socket. Same TLV protocol
		// that flowed over the helper's stdin in fork-exec mode.
		ILibProcessPipe_Pipe_Write(g_kvmSocketPipe, buf, len, ILibTransport_MemoryOwnership_USER);
		return(len);
	}
	if (gChildProcess != NULL)
	{
		ILibProcessPipe_Process_WriteStdIn(gChildProcess, buf, len, ILibTransport_MemoryOwnership_USER);
	}
	return(len);
}

// Set the KVM pause state
void kvm_pause(int pause)
{
	g_pause = pause;
}


void* kvm_mainloopinput(void* param)
{
	int ptr = 0;
	int ptr2 = 0;
	int len = 0;
	char pchRequest2[30000];
	int cbBytesRead = 0;

	char tmp[255];
	int tmpLen;

	if (KVM_AGENT_FD == -1)
	{
		int flags;
		flags = fcntl(STDIN_FILENO, F_GETFL, 0);
		if (fcntl(STDIN_FILENO, F_SETFL, (O_NONBLOCK | flags) ^ O_NONBLOCK) == -1) { senddebug(-999); }
	}

	while (!g_shutdown)
	{
		if (KVM_AGENT_FD != -1)
		{
			tmpLen = sprintf_s(tmp, sizeof(tmp), "About to read from IPC Socket\n");
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}

		KvmDebugLog("Reading from master in kvm_mainloopinput\n");
		if (KVM_AGENT_FD != -1 && KVM_Listener_FD != -1)
		{
			// A stale connection (daemon end left open — e.g. a browser tab closed
			// without a clean disconnect) can sit on KVM_AGENT_FD forever without
			// ever hitting EOF, which starves the listener: the backlog holds any
			// new session's connect() attempt indefinitely, since accept() below
			// only runs after THIS read detects EOF. Poll both fds instead of just
			// reading, so a pending new connection can evict the stale one rather
			// than queue behind it — last connect wins.
			struct pollfd pfds[2];
			pfds[0].fd = KVM_AGENT_FD;    pfds[0].events = POLLIN;
			pfds[1].fd = KVM_Listener_FD; pfds[1].events = POLLIN;
			int pr = poll(pfds, 2, -1);
			if (pr > 0 && (pfds[1].revents & POLLIN))
			{
				int newfd = accept(KVM_Listener_FD, NULL, NULL);
				if (newfd >= 0)
				{
					kvm_flog("EVICT stale kvm connection fd=%d for new fd=%d\n", KVM_AGENT_FD, newfd);
					{ extern void kvm_reset_modifiers(void); kvm_reset_modifiers(); }
					close(KVM_AGENT_FD);
					KVM_AGENT_FD = newfd;
					SCREEN_HEIGHT = SCREEN_WIDTH = 0; // force a clean reinit for the new viewer
					len = 0; ptr = 0;                 // discard any partial buffer from the old connection
					continue;
				}
			}
			if (!(pr > 0 && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)))) { continue; }
		}
		cbBytesRead = read(KVM_AGENT_FD == -1 ? STDIN_FILENO: KVM_AGENT_FD, pchRequest2 + len, 30000 - len);
		KvmDebugLog("Read %d bytes from master in kvm_mainloopinput\n", cbBytesRead);

		if (KVM_AGENT_FD != -1)
		{
			tmpLen = sprintf_s(tmp, sizeof(tmp), "Read %d bytes from IPC-xx-Socket\n", cbBytesRead);
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}

		if (cbBytesRead == -1 || cbBytesRead == 0) 
		{ 
			/*ILIBMESSAGE("KVMBREAK-K1\r\n"); g_shutdown = 1; printf("shutdown\n");*/ 
			if (KVM_AGENT_FD == -1)
			{
				g_shutdown = 1;
			}
			else
			{
				g_resetipc = 1;
			}
			break; 
		}
		len += cbBytesRead;
		ptr2 = 0;
		
		if (KVM_AGENT_FD != -1)
		{
			tmpLen = sprintf_s(tmp, sizeof(tmp), "enter while\n");
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}
		while ((ptr2 = kvm_server_inputdata(pchRequest2 + ptr, len - ptr)) != 0) { ptr += ptr2; }

		if (KVM_AGENT_FD != -1)
		{
			tmpLen = sprintf_s(tmp, sizeof(tmp), "exited while\n");
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}

		if (ptr == len) { len = 0; ptr = 0; }
		else { memmove(pchRequest2, pchRequest2 + ptr, len - ptr); len -= ptr; ptr = 0; }
	}

	return 0;
}
void ExitSink(int s)
{
	UNREFERENCED_PARAMETER(s);
	// _exit(1): exit immediately with non-zero so launchd (KeepAlive:true /
	// SuccessfulExit:false) restarts us. Closing KVM_Listener_FD and waiting
	// for the kvm_mainloopinput thread (which may block in read(KVM_AGENT_FD))
	// can hang indefinitely; _exit bypasses that.
	_exit(1);
}
void* kvm_server_mainloop(void* param)
{
	int x, y, height, width, r, c = 0;
	long long desktopsize = 0;
	long long tilesize = 0;
	void *desktop = NULL;
	void *buf = NULL;
	int screen_height, screen_width, screen_num;
	int written = 0;
	struct sockaddr_un serveraddr;

	if (param == NULL)
	{
		// This is doing I/O via StdIn/StdOut

		int flags;
		flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
		if (fcntl(STDOUT_FILENO, F_SETFL, (O_NONBLOCK | flags) ^ O_NONBLOCK) == -1) {}
	}
	else
	{
		// this is doing I/O via a Unix Domain Socket
		if ((KVM_Listener_FD = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		{
			char tmp[255];
			int tmplen = sprintf_s(tmp, sizeof(tmp), "ERROR CREATING DOMAIN SOCKET: %d\n", errno);
			// Error creating domain socket
			written = write(STDOUT_FILENO, tmp, tmplen);
			fsync(STDOUT_FILENO);
			return(NULL);
		}

		int flags;
		flags = fcntl(KVM_Listener_FD, F_GETFL, 0);
		if (fcntl(KVM_Listener_FD, F_SETFL, (O_NONBLOCK | flags) ^ O_NONBLOCK) == -1) { }

		written = write(STDOUT_FILENO, "Set FCNTL2\n", 11);
		fsync(STDOUT_FILENO);

		memset(&serveraddr, 0, sizeof(serveraddr));
		serveraddr.sun_family = AF_UNIX;
		strcpy(serveraddr.sun_path, kvm_listener_path);
		remove(kvm_listener_path);
		if (bind(KVM_Listener_FD, (struct sockaddr *)&serveraddr, SUN_LEN(&serveraddr)) < 0)
		{
			char tmp[255];
			int tmplen = sprintf_s(tmp, sizeof(tmp), "BIND ERROR on DOMAIN SOCKET: %d\n", errno);
			// Error creating domain socket
			written = write(STDOUT_FILENO, tmp, tmplen);
			fsync(STDOUT_FILENO);
			return(NULL);
		}

		if (listen(KVM_Listener_FD, 1) < 0)
		{
			written = write(STDOUT_FILENO, "LISTEN ERROR ON DOMAIN SOCKET", 29);
			fsync(STDOUT_FILENO);
			return(NULL);
		}

		written = write(STDOUT_FILENO, "LISTENING ON DOMAIN SOCKET\n", 27);
		fsync(STDOUT_FILENO);

		signal(SIGTERM, ExitSink);

		if ((KVM_AGENT_FD = accept(KVM_Listener_FD, NULL, NULL)) < 0)
		{
			written = write(STDOUT_FILENO, "ACCEPT ERROR ON DOMAIN SOCKET", 29);
			fsync(STDOUT_FILENO);
			kvm_flog("ACCEPT ERROR: errno=%d\n", errno);
			return(NULL);
		}
		else
		{
			char tmp[255];
			int tmpLen = sprintf_s(tmp, sizeof(tmp), "ACCEPTed new connection %d on Domain Socket\n", KVM_AGENT_FD);
			written = write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
			kvm_flog("ACCEPTED connection fd=%d uid=%d\n", KVM_AGENT_FD, (int)getuid());
		}
	}
	// Init the kvm
	g_messageQ = ILibQueue_Create();
	kvm_flog("kvm_init start\n");
	if (kvm_init() != 0) { kvm_flog("kvm_init FAILED\n"); return (void*)-1; }
	kvm_flog("kvm_init OK\n");
	hid_inject_init();


	g_shutdown = 0;
	pthread_create(&kvmthread, NULL, kvm_mainloopinput, param);


	if (KVM_AGENT_FD != -1)
	{
		written = write(STDOUT_FILENO, "Starting Loop []\n", 14);
		fsync(STDOUT_FILENO);

		char stmp[255];
		int stmpLen = sprintf_s(stmp, sizeof(stmp), "TILE_HEIGHT_COUNT=%d, TILE_WIDTH_COUNT=%d\n", TILE_HEIGHT_COUNT, TILE_WIDTH_COUNT);
		written = write(STDOUT_FILENO, stmp, stmpLen);
		fsync(STDOUT_FILENO);
	}

	while (!g_shutdown) 
	{
		if (g_resetipc != 0)
		{
			g_resetipc = 0;
			close(KVM_AGENT_FD);

			SCREEN_HEIGHT = SCREEN_WIDTH = 0;

			char stmp[255];
			int stmpLen = sprintf_s(stmp, sizeof(stmp), "Waiting for NEXT DomainSocket, TILE_HEIGHT_COUNT=%d, TILE_WIDTH_COUNT=%d\n", TILE_HEIGHT_COUNT, TILE_WIDTH_COUNT);
			written = write(STDOUT_FILENO, stmp, stmpLen);
			fsync(STDOUT_FILENO);

			if ((KVM_AGENT_FD = accept(KVM_Listener_FD, NULL, NULL)) < 0)
			{
				g_shutdown = 1;
				written = write(STDOUT_FILENO, "ACCEPT ERROR ON DOMAIN SOCKET", 29);
				fsync(STDOUT_FILENO);
				break;
			}
			else
			{
				char tmp[255];
				int tmpLen = sprintf_s(tmp, sizeof(tmp), "ACCEPTed new connection %d on Domain Socket\n", KVM_AGENT_FD);
				written = write(STDOUT_FILENO, tmp, tmpLen);
				fsync(STDOUT_FILENO);
				pthread_create(&kvmthread, NULL, kvm_mainloopinput, param);
			}
		}
		
		// Check if there are pending messages to be sent
		ILibQueue_Lock(g_messageQ);
		while (ILibQueue_IsEmpty(g_messageQ) == 0)
		{
			if ((buf = (char*)ILibQueue_DeQueue(g_messageQ)) != NULL)
			{
				KVM_SEND(buf, (int)ILibMemory_Size(buf));
				ILibMemory_Free(buf);
			}
		}
		ILibQueue_UnLock(g_messageQ);


		for (r = 0; r < TILE_HEIGHT_COUNT; r++) 
		{
			for (c = 0; c < TILE_WIDTH_COUNT; c++) 
			{
				g_tileInfo[r][c].flag = TILE_TODO;
#ifdef KVM_ALL_TILES
				g_tileInfo[r][c].crc = 0xFF;
#endif
			}
		}

		screen_num = kvm_selected_display();
		static int logged_once = 0;
		if (!logged_once) { kvm_flog("MainLoop start: display id=%u\n", screen_num); logged_once = 1; }

		if (screen_num == 0) { kvm_flog("CGMainDisplayID=0, shutdown\n"); g_shutdown = 1; senddebug(-2); break; }

		// Size the frame from the display we are actually on. The old code only
		// ever revised SCREEN_SCALE upward and then latched it (SCREEN_SCALE_SET),
		// which cannot survive switching to a display with a different scale.
		{
			CGRect sb = kvm_selected_bounds();
			int    sc = kvm_selected_scale();
			screen_height = (int)(sb.size.height * sc);
			screen_width  = (int)(sb.size.width  * sc);
		}
		
		if ((SCREEN_HEIGHT != screen_height || (SCREEN_WIDTH != screen_width) || SCREEN_NUM != screen_num))
		{
			kvm_flog("reinit: H=%d→%d W=%d→%d SCALE=%d NUM=%u→%u\n",
				SCREEN_HEIGHT, screen_height, SCREEN_WIDTH, screen_width,
				SCREEN_SCALE, SCREEN_NUM, screen_num);
			kvm_init();
			continue;
		}

		//senddebug(screen_num);
		extern CGImageRef kvm_capture_sck(uint32_t displayID);
		CGImageRef image = NULL;
		if (kvm_combined_mode())
		{
			image = kvm_capture_all_displays();
			static int logged_all = 0;
			if (!logged_all) { kvm_flog("capture: ALL displays, image=%p\n", image); logged_all = 1; }
		}
		else if (getuid() == 0)
		{
			// loginwindow (uid=0). ScreenCaptureKit is preferred but only exists on
			// macOS 14+, so on older systems kvm_capture_sck() returns NULL. Fall
			// back to the standard CoreGraphics capture APIs: on a PHYSICAL Mac (not
			// a headless VM) these DO capture the login window on macOS 11-13 when
			// ScreenCapture TCC is granted (preflight=1 here). CGWindowListCreateImage
			// composites the on-screen login UI; CGDisplayCreateImage is a last resort.
			image = kvm_capture_sck((uint32_t)screen_num);
			if (image == NULL)
			{
				image = CGWindowListCreateImage(
					CGRectInfinite, kCGWindowListOptionOnScreenOnly,
					kCGNullWindowID, kCGWindowImageDefault);
				if (image == NULL) image = CGDisplayCreateImage(screen_num);
			}
			static int logged_sck_lw = 0;
			if (!logged_sck_lw) {
				kvm_flog("loginwindow capture: image=%p preflight=%d\n",
					image, CGPreflightScreenCaptureAccess());
				logged_sck_lw = 1;
			}
		}
		else
		{
			// User session (uid != 0). Prefer CGDisplayCreateImage: it captures the
			// display directly instead of compositing the window list, and measured
			// ~150-300x faster than CGWindowListCreateImage on macOS 15+ (that API
			// was obsoleted in macOS 15.0 and its post-deprecation fallback path is
			// catastrophically slow — ~1s/call — on at least one tested machine,
			// vs 3-7ms/call for CGDisplayCreateImage on the same hardware).
			// Fall back to CGWindowListCreateImage only if CGDisplayCreateImage
			// fails, which happens on HEADLESS VMs (no display scanout): there,
			// CGDisplayCreateImage / ScreenCaptureKit / screencapture all fail
			// ("could not create image from display"), but CGWindowListCreateImage
			// still works by compositing on-screen window content into an offscreen
			// buffer without needing a real display.
			image = CGDisplayCreateImage(screen_num);
			if (image == NULL)
			{
				image = CGWindowListCreateImage(
					CGRectInfinite,
					kCGWindowListOptionOnScreenOnly,
					kCGNullWindowID,
					kCGWindowImageDefault);
			}
			static int logged_wl = 0;
			if (!logged_wl) {
				kvm_flog("user capture: image=%p preflight=%d\n",
					image, CGPreflightScreenCaptureAccess());
				logged_wl = 1;
			}
		}
		if (image == NULL)
		{
			// Both public APIs failed (TCC blocked). Try private SkyLight/CGS
			// capture functions that go through our established WindowServer
			// connection directly, bypassing the client-side TCC check.
			static int logged_cgs = 0;
			static void *sl_handle = NULL;
			static int (*sl_capture)(int, uint32_t, CGRect, float, CGImageRef *) = NULL;
			if (sl_handle == NULL)
			{
				sl_handle = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_NOW | RTLD_NOLOAD);
				if (!sl_handle) sl_handle = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_NOW);
				if (sl_handle)
				{
					sl_capture = dlsym(sl_handle, "SLSCaptureSizedDisplayRect");
					if (!sl_capture) sl_capture = dlsym(sl_handle, "CGSCaptureSizedDisplayRect");
					if (!sl_capture) sl_capture = dlsym(RTLD_DEFAULT, "SLSCaptureSizedDisplayRect");
					if (!sl_capture) sl_capture = dlsym(RTLD_DEFAULT, "CGSCaptureSizedDisplayRect");
				}
				else
				{
					// Try RTLD_DEFAULT directly
					sl_capture = dlsym(RTLD_DEFAULT, "SLSCaptureSizedDisplayRect");
					if (!sl_capture) sl_capture = dlsym(RTLD_DEFAULT, "CGSCaptureSizedDisplayRect");
				}
				if (!logged_cgs)
				{
					typedef int CGSConnectionID;
					extern CGSConnectionID CGSMainConnectionID(void);
					kvm_flog("SkyLight handle=%p sl_capture=%p cid=%d\n",
						sl_handle, sl_capture, CGSMainConnectionID());
					logged_cgs = 1;
				}
			}
			if (sl_capture)
			{
				typedef int CGSConnectionID;
				extern CGSConnectionID CGSMainConnectionID(void);
				CGSConnectionID cid = CGSMainConnectionID();
				CGRect fullRect = CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
				int rc = sl_capture(cid, screen_num, fullRect, 1.0f, &image);
				if (rc != 0 || image == NULL)
				{
					static int logged_sls = 0;
					if (!logged_sls) { kvm_flog("SLS capture rc=%d image=%p\n", rc, image); logged_sls = 1; }
					image = NULL;
				}
				else
				{
					static int logged_ok = 0;
					if (!logged_ok) { kvm_flog("SLS capture succeeded!\n"); logged_ok = 1; }
				}
			}
		}
		if (image == NULL)
		{
			// Last resort: ScreenCaptureKit (macOS 14+). Uses a separate
			// system daemon, so its TCC check may differ from the CoreGraphics
			// path and might succeed at the loginwindow.
			extern CGImageRef kvm_capture_sck(uint32_t displayID);
			static int sck_logged = 0;
			if (!sck_logged) { kvm_flog("trying SCK fallback\n"); sck_logged = 1; }
			image = kvm_capture_sck((uint32_t)screen_num);
		}
		if (image == NULL)
		{
			// All capture APIs failed. Sleep 1s and retry rather than exiting —
			// launchd won't restart on exit(0), and SCK needs time to settle.
			static int all_logged = 0;
			if (!all_logged) { kvm_flog("All capture APIs failed, retrying\n"); all_logged = 1; }
			if (!g_resetipc && !g_shutdown) usleep(1000000);
			continue;
		}
		else {
			//senddebug(100);
			getScreenBuffer((unsigned char **)&desktop, &desktopsize, image);

			if (KVM_AGENT_FD != -1)
			{
				char tmp[255];
				int tmpLen = sprintf_s(tmp, sizeof(tmp), "...Enter for loop\n");
				written = write(STDOUT_FILENO, tmp, tmpLen);
				fsync(STDOUT_FILENO);
			}

			for (y = 0; y < TILE_HEIGHT_COUNT; y++) 
			{
				for (x = 0; x < TILE_WIDTH_COUNT; x++) {
					height = TILE_HEIGHT * y;
					width = TILE_WIDTH * x;
					if (!g_shutdown && (g_pause)) { usleep(100000); g_pause = 0; } //HACK: Change this
					
					if (g_shutdown) { x = TILE_WIDTH_COUNT; y = TILE_HEIGHT_COUNT; break; }
					
					if (g_tileInfo[y][x].flag == TILE_SENT || g_tileInfo[y][x].flag == TILE_DONT_SEND) {
						continue;
					}
					
					getTileAt(width, height, &buf, &tilesize, desktop, desktopsize, y, x);
					
					if (buf && !g_shutdown)
					{	
						// Write the reply to the pipe.
						//KvmDebugLog("Writing to master in kvm_server_mainloop\n");

						written = KVM_SEND(buf, tilesize);

						//KvmDebugLog("Wrote %d bytes to master in kvm_server_mainloop\n", written);
						if (written == -1) 
						{ 
							/*ILIBMESSAGE("KVMBREAK-K2\r\n");*/ 
							if(KVM_AGENT_FD == -1)
							{
								// This is a User Session, so if the connection fails, we exit out... We can be spawned again later
								g_shutdown = 1; height = SCREEN_HEIGHT; width = SCREEN_WIDTH; break;
							}
						}
						//else
						//{
						//	char tmp[255];
						//	int tmpLen = sprintf_s(tmp, sizeof(tmp), "KVM_SEND => tilesize: %d\n", tilesize);
						//	written = write(STDOUT_FILENO, tmp, tmpLen);
						//	fsync(STDOUT_FILENO);
						//}
						free(buf);

					}
				}
			}

			if (KVM_AGENT_FD != -1)
			{
				char tmp[255];
				int tmpLen = sprintf_s(tmp, sizeof(tmp), "...exit for loop\n");
				written = write(STDOUT_FILENO, tmp, tmpLen);
				fsync(STDOUT_FILENO);
			}

		}
		CGImageRelease(image);
	}
	
	pthread_join(kvmthread, NULL);
	kvmthread = (pthread_t)NULL;

	if (g_tileInfo != NULL) { for (r = 0; r < TILE_HEIGHT_COUNT; r++) { free(g_tileInfo[r]); } }
	g_tileInfo = NULL;
	if(tilebuffer != NULL) {
		free(tilebuffer);
		tilebuffer = NULL;
	}

	if (KVM_AGENT_FD != -1)
	{
		written = write(STDOUT_FILENO, "Exiting...\n", 11);
		fsync(STDOUT_FILENO);
	}
	ILibQueue_Destroy(g_messageQ);
	return (void*)0;
}

void kvm_relay_ExitHandler(ILibProcessPipe_Process sender, int exitCode, void* user)
{
	//ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void**)user)[0];
	//void *reserved = ((void**)user)[1];
	//void *pipeMgr = ((void**)user)[2];
	//char *exePath = (char*)((void**)user)[3];
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(exitCode);
	UNREFERENCED_PARAMETER(user);
}
void kvm_relay_StdOutHandler(ILibProcessPipe_Process sender, char *buffer, size_t bufferLen, size_t* bytesConsumed, void* user)
{
	unsigned short size = 0;
	UNREFERENCED_PARAMETER(sender);
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void**)user)[0];
	void *reserved = ((void**)user)[1];

	if (bufferLen > 4)
	{
		if (ntohs(((unsigned short*)(buffer))[0]) == (unsigned short)MNG_JUMBO)
		{
			if (bufferLen > 8)
			{
				if (bufferLen >= (8 + (int)ntohl(((unsigned int*)(buffer))[1])))
				{
					*bytesConsumed = 8 + (int)ntohl(((unsigned int*)(buffer))[1]);
					TLSLOG1("<< KVM/WRITE: %d bytes\n", *bytesConsumed);
					writeHandler(buffer, *bytesConsumed, reserved);

					//printf("JUMBO PACKET: %d\n", *bytesConsumed);
					return;
				}
			}
		}
		else
		{
			size = ntohs(((unsigned short*)(buffer))[1]);
			if (size <= bufferLen)
			{
				*bytesConsumed = size;
				writeHandler(buffer, size, reserved);
				//printf("Normal PACKET: %d\n", *bytesConsumed);
				return;
			}
		}
	}
	*bytesConsumed = 0;
}
void kvm_relay_StdErrHandler(ILibProcessPipe_Process sender, char *buffer, size_t bufferLen, size_t* bytesConsumed, void* user)
{
	//KVMDebugLog *log = (KVMDebugLog*)buffer;

	//UNREFERENCED_PARAMETER(sender);
	//UNREFERENCED_PARAMETER(user);

	//if (bufferLen < sizeof(KVMDebugLog) || bufferLen < log->length) { *bytesConsumed = 0;  return; }
	//*bytesConsumed = log->length;
	////ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), (ILibRemoteLogging_Modules)log->logType, (ILibRemoteLogging_Flags)log->logFlags, "%s", log->logData);
	//ILibRemoteLogging_printf(ILibChainGetLogger(gILibChain), ILibRemoteLogging_Modules_Microstack_Generic, (ILibRemoteLogging_Flags)log->logFlags, "%s", log->logData);
	*bytesConsumed = bufferLen;
}


// macOS Tahoe: socket-mode read handler. Same TLV-frame parser as
// kvm_relay_StdOutHandler but with the (sender, buf, len, consumed)
// signature ILibProcessPipe_Pipe_AddPipeReadHandler expects (no user
// arg). User context is fetched from g_kvmSocket* statics, set in
// kvm_relay_setup.
static void kvm_relay_socket_ReadHandler(ILibProcessPipe_Pipe sender, char *buffer, size_t bufferLen, size_t* bytesConsumed)
{
	unsigned short size = 0;
	UNREFERENCED_PARAMETER(sender);
	ILibKVM_WriteHandler writeHandler = g_kvmSocketWriteHandler;
	void *reserved = g_kvmSocketReserved;
	if (writeHandler == NULL) { *bytesConsumed = bufferLen; return; }

	if (bufferLen > 4)
	{
		if (ntohs(((unsigned short*)(buffer))[0]) == (unsigned short)MNG_JUMBO)
		{
			if (bufferLen > 8)
			{
				if (bufferLen >= (8 + (int)ntohl(((unsigned int*)(buffer))[1])))
				{
					*bytesConsumed = 8 + (int)ntohl(((unsigned int*)(buffer))[1]);
					writeHandler(buffer, *bytesConsumed, reserved);
					return;
				}
			}
		}
		else
		{
			size = ntohs(((unsigned short*)(buffer))[1]);
			if (size <= bufferLen)
			{
				*bytesConsumed = size;
				writeHandler(buffer, size, reserved);
				return;
			}
		}
	}
	*bytesConsumed = 0;
}

// macOS Tahoe: socket-mode broken-pipe handler. Clears state so
// kvm_relay_feeddata() falls through cleanly and a future Desktop
// session re-runs kvm_relay_setup() (which reconnects).
static void kvm_relay_socket_BrokenHandler(ILibProcessPipe_Pipe sender)
{
	UNREFERENCED_PARAMETER(sender);
	kvm_relay_socket_ResetState(0);
}

// Setup the KVM session. Return 1 if ok, 0 if it could not be setup.
void* kvm_relay_setup(char *exePath, void *processPipeMgr, ILibKVM_WriteHandler writeHandler, void *reserved, int uid)
{
	char * parms0[] = { "meshagent_osx64", "-kvm0", NULL };
	void **user = (void**)ILibMemory_Allocate(4 * sizeof(void*), 0, NULL, NULL);
	user[0] = writeHandler;
	user[1] = reserved;
	user[2] = processPipeMgr;
	user[3] = exePath;

	// Ensure stale socket-mode state from a prior Desktop session is gone
	// before attempting a new connection.
	kvm_relay_socket_ResetState(1);

	if (uid != 0)
	{
		// macOS Tahoe path: prefer the user-LaunchAgent socket. Agent
		// registers in gui/<uid> natively → com.apple.replayd reachable
		// → ScreenCaptureKit works without audit_session_join. Daemon
		// just proxies bytes here.
		//
		// Fallback policy: only fall through to the legacy fork-exec
		// helper if the LaunchAgent is genuinely NOT installed (no
		// socket file on disk). If the socket exists but connect()
		// transiently fails — e.g. agent is between sessions and
		// hasn't re-accept()ed yet — we retry briefly with backoff
		// rather than firing the broken fork-exec path that lands
		// the helper in the wrong audit session and produces black
		// frames.
		char socketPath[128];
		snprintf(socketPath, sizeof(socketPath), "/tmp/meshagent-kvm-%d.sock", uid);
		struct stat sockStat;

		// Right after a login/logout handoff the console UID has just changed and
		// the NEW session's kvmagent LaunchAgent may still be starting — so its
		// socket file doesn't exist yet. A single stat() here would wrongly
		// conclude "agent not installed" and fall through to the broken legacy
		// fork-exec path, which freezes the viewer until the operator manually
		// reconnects. Poll briefly for the socket to appear so the viewer rides
		// through the session switch. Normal (already-running) sessions match on
		// the first check with no delay.
		int agentInstalled = 0;
		{
			int waitedMs;
			for (waitedMs = 0; waitedMs <= 5000; waitedMs += 150)
			{
				if (stat(socketPath, &sockStat) == 0 && S_ISSOCK(sockStat.st_mode)) { agentInstalled = 1; break; }
				usleep(150000);
			}
		}

		if (agentInstalled)
		{
			int fd = -1;
			int attempt;
			for (attempt = 0; attempt < 5; attempt++)
			{
				fd = socket(AF_UNIX, SOCK_STREAM, 0);
				if (fd < 0) break;
				struct sockaddr_un addr;
				memset(&addr, 0, sizeof(addr));
				addr.sun_family = AF_UNIX;
				strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);
				if (connect(fd, (struct sockaddr*)&addr, SUN_LEN(&addr)) == 0)
				{
					// Agent is up. Wrap the connected socket in a Pipe so
					// the daemon's existing Pipe_Write / read-handler paths
					// work without changes elsewhere in the codebase.
					g_kvmSocketFD            = fd;
					g_kvmSocketWriteHandler  = writeHandler;
					g_kvmSocketReserved      = reserved;
					g_kvmSocketPipe          = ILibProcessPipe_Pipe_CreateFromExisting((ILibProcessPipe_Manager)processPipeMgr, fd);
					ILibProcessPipe_Pipe_AddPipeReadHandler(g_kvmSocketPipe, 65535, &kvm_relay_socket_ReadHandler);
					ILibProcessPipe_Pipe_SetBrokenPipeHandler(g_kvmSocketPipe, &kvm_relay_socket_BrokenHandler);
					ILibProcessPipe_Pipe_ResetMetadata(g_kvmSocketPipe, "KVM user-LaunchAgent socket");
					g_shutdown = 0;
					ILibMemory_Free(user); // not used in socket mode
					return(g_kvmSocketPipe);
				}
				close(fd);
				fd = -1;
				// Transient failure (ECONNREFUSED on agent re-accept,
				// or similar). Back off briefly: 50ms, 100ms, 200ms,
				// 400ms, 800ms — total ~1.55s before giving up.
				usleep(50000 << attempt);
			}
			// Socket exists but unreachable after retries. Don't fall
			// back to fork-exec — that path is broken on Tahoe and
			// would produce black-screen / dead-input. Return NULL
			// to let the caller surface the error to MeshCentral.
			ILibMemory_Free(user);
			return(NULL);
		}

		// Agent not installed (no socket file) — legacy fork-exec
		// helper. Still subject to the audit-session-isolation
		// problem on Tahoe; install the LaunchAgent (-fullinstall
		// in agent-installer.js) to upgrade to the working path.
		gChildProcess = ILibProcessPipe_Manager_SpawnProcessEx3(processPipeMgr, exePath, parms0, ILibProcessPipe_SpawnTypes_DEFAULT, (void*)(uint64_t)uid, 0);
		g_slavekvm = ILibProcessPipe_Process_GetPID(gChildProcess);

		char tmp[255];
		sprintf_s(tmp, sizeof(tmp), "Child KVM (pid: %d)", g_slavekvm);
		ILibProcessPipe_Process_ResetMetadata(gChildProcess, tmp);

		ILibProcessPipe_Process_AddHandlers(gChildProcess, 65535, &kvm_relay_ExitHandler, &kvm_relay_StdOutHandler, &kvm_relay_StdErrHandler, NULL, user);

		// Run the relay
		g_shutdown = 0;
		return(ILibProcessPipe_Process_GetStdOut(gChildProcess));
	}
	else
	{
		// No user logged in — loginwindow session.
		// The kvmagent LaunchAgent loads into the loginwindow bootstrap domain
		// (LimitLoadToSessionType = LoginWindow) as root (uid 0) and creates
		// /tmp/meshagent-kvm-0.sock. Connect to it exactly like the Aqua path.
		char lwSocketPath[128];
		snprintf(lwSocketPath, sizeof(lwSocketPath), "/tmp/meshagent-kvm-0.sock");
		struct stat sockStat;
		int agentInstalled = (stat(lwSocketPath, &sockStat) == 0 && S_ISSOCK(sockStat.st_mode));

		if (agentInstalled)
		{
			int fd = -1;
			int attempt;
			for (attempt = 0; attempt < 5; attempt++)
			{
				fd = socket(AF_UNIX, SOCK_STREAM, 0);
				if (fd < 0) break;
				struct sockaddr_un addr;
				memset(&addr, 0, sizeof(addr));
				addr.sun_family = AF_UNIX;
				strncpy(addr.sun_path, lwSocketPath, sizeof(addr.sun_path) - 1);
				if (connect(fd, (struct sockaddr*)&addr, SUN_LEN(&addr)) == 0)
				{
					g_kvmSocketFD            = fd;
					g_kvmSocketWriteHandler  = writeHandler;
					g_kvmSocketReserved      = reserved;
					g_kvmSocketPipe          = ILibProcessPipe_Pipe_CreateFromExisting((ILibProcessPipe_Manager)processPipeMgr, fd);
					ILibProcessPipe_Pipe_AddPipeReadHandler(g_kvmSocketPipe, 65535, &kvm_relay_socket_ReadHandler);
					ILibProcessPipe_Pipe_SetBrokenPipeHandler(g_kvmSocketPipe, &kvm_relay_socket_BrokenHandler);
					ILibProcessPipe_Pipe_ResetMetadata(g_kvmSocketPipe, "KVM loginwindow-session socket");
					g_shutdown = 0;
					ILibMemory_Free(user);
					return(g_kvmSocketPipe);
				}
				close(fd);
				fd = -1;
				usleep(50000 << attempt);
			}
		}
		ILibMemory_Free(user);
		return(NULL);
	}
}

// Force a KVM reset & refresh
void kvm_relay_reset()
{
	char buffer[4];
	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_REFRESH);	// Write the type
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)4);				// Write the size
	kvm_relay_feeddata(buffer, 4);
}

// Clean up the KVM session.
void kvm_cleanup()
{
	KvmDebugLog("kvm_cleanup\n");
	g_shutdown = 1;
	kvm_relay_socket_ResetState(1);
	if (gChildProcess != NULL)
	{
		ILibProcessPipe_Process_SoftKill(gChildProcess);
		gChildProcess = NULL;
	}
}


typedef enum {
    MPAuthorizationStatusNotDetermined,
    MPAuthorizationStatusAuthorized,
    MPAuthorizationStatusDenied
} MPAuthorizationStatus;




MPAuthorizationStatus _checkFDAUsingFile(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd != -1)
    {
        close(fd);
        return MPAuthorizationStatusAuthorized;
    }

    if (errno == EPERM || errno == EACCES)
    {
        return MPAuthorizationStatusDenied;
    }

    return MPAuthorizationStatusNotDetermined;
}

MPAuthorizationStatus _fullDiskAuthorizationStatus() {
    char *userHomeFolderPath = getenv("HOME");
    if (userHomeFolderPath == NULL) {
        struct passwd *pw = getpwuid(getuid());
        if (pw == NULL) {
            return MPAuthorizationStatusNotDetermined;
        }
        userHomeFolderPath = pw->pw_dir;
    }

    const char *testFiles[] = {
        strcat(strcpy(malloc(strlen(userHomeFolderPath) + 30), userHomeFolderPath), "/Library/Safari/CloudTabs.db"),
        strcat(strcpy(malloc(strlen(userHomeFolderPath) + 30), userHomeFolderPath), "/Library/Safari/Bookmarks.plist"),
        "/Library/Application Support/com.apple.TCC/TCC.db",
        "/Library/Preferences/com.apple.TimeMachine.plist",
    };

    MPAuthorizationStatus resultStatus = MPAuthorizationStatusNotDetermined;
    for (int i = 0; i < 4; i++) {
        MPAuthorizationStatus status = _checkFDAUsingFile(testFiles[i]);
        if (status == MPAuthorizationStatusAuthorized) {
            resultStatus = MPAuthorizationStatusAuthorized;
            break;
        }
        if (status == MPAuthorizationStatusDenied) {
            resultStatus = MPAuthorizationStatusDenied;
        }
    }

    return resultStatus;
}


// ---- Interactive permission walkthrough (-requestperms) --------------------
//
// Run by the installer, in the console user's Aqua session, so every grant the
// agent needs is asked for WHILE INSTALLING instead of the first time someone
// opens a remote session.
//
// The mechanic that matters: a program only appears in a Privacy pane once tccd
// has a record for it. Opening the Screen Recording pane before that just shows
// an empty list (observed 2026-09-09). And CGRequestScreenCaptureAccess() does
// NOT create that record here -- tccd answers "Service kTCCServiceScreenCapture
// does not allow prompting; returning denied" -- besides re-triggering a
// recurring permission banner even when the grant already exists (see kvm_init).
// What does register the program, and raises Apple's own "... would like to
// record this screen" dialog, is ATTEMPTING A REAL CAPTURE. So each step
// performs the actual operation and lets macOS raise its own dialog; the
// System Settings pane is only ever a fallback.
//
// Steps are also sequential and user-paced: opening a pane while another
// dialog is pending steals focus and hides it (that is what masked the
// Accessibility prompt).

static void perm_open_pane(const char *anchor)
{
    char url[256];
    snprintf(url, sizeof(url), "x-apple.systempreferences:com.apple.preference.security?%s", anchor);
    CFStringRef u = CFStringCreateWithCString(NULL, url, kCFStringEncodingASCII);
    if (!u) return;
    CFURLRef p = CFURLCreateWithString(NULL, u, NULL);
    if (p) { LSOpenCFURLRef(p, NULL); CFRelease(p); }
    CFRelease(u);
}

// Read a line from the operator.
//
// Prefer stdin: the installer runs this under `launchctl asuser`, which detaches
// the process from its controlling terminal, so fopen("/dev/tty") is unreliable
// there (it returned NULL mid-run and made the walkthrough "skip" a step it
// should have waited on). The installer redirects stdin from /dev/tty for us, so
// an inherited fd still works even with no controlling terminal. Fall back to
// /dev/tty only when stdin is not a terminal.
// Returns 0 when neither is usable, so callers can skip instead of hanging an
// unattended install.
static int perm_ask(const char *prompt, char *out, size_t outsz)
{
    if (isatty(STDIN_FILENO))
    {
        printf("%s", prompt); fflush(stdout);
        // fgets() also returns NULL when the read is merely interrupted (a
        // dialog going up mid-read did exactly that, and step 1 "skipped"
        // itself while later steps read the same fd fine). Only a real EOF
        // means there is nobody there.
        for (int tries = 0; tries < 5; tries++)
        {
            if (fgets(out, (int)outsz, stdin) != NULL) return 1;
            if (feof(stdin)) return 0;
            clearerr(stdin);
        }
        return 0;
    }
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) return 0;
    fputs(prompt, tty); fflush(tty);
    if (fgets(out, (int)outsz, tty) == NULL) { fclose(tty); return 0; }
    fclose(tty);
    return 1;
}

// Drive a brief, local KVM session so macOS raises the Screen Recording dialog
// against the AGENT.
//
// Everything else was tried and measured to fail (2026-09-09, clean Tahoe 26.6.2):
//   - CGRequestScreenCaptureAccess() creates no tccd record at all, and
//     re-triggers a recurring banner even once granted (see kvm_init).
//   - Capturing from this walkthrough attributes to the RESPONSIBLE process,
//     which is the operator's shell -- tccd recorded
//     kTCCServiceScreenCapture|com.apple.Terminal and no meshagent row, so the
//     pane listed nothing to toggle.
//   - Probing at agent startup did not help either: the LaunchAgent had been
//     bootstrapped by the installer, so the job inherited that same responsible
//     process. Clearing Terminal's row and restarting merely recreated it.
//     A reboot did not clear it.
// What DOES work, observed by the operator and then confirmed here, is a real
// Desktop session: the prompt appears correctly populated with meshagent.
//
// A Desktop session is just the daemon connecting to the agent's unix socket --
// on accept the agent runs kvm_init() and its capture loop. So connect to that
// socket ourselves, hold it briefly, and drop it. Who connects is irrelevant to
// TCC: the capture is performed by the agent process, which is what tccd judges.

// One connect() attempt against the agent's session socket. Returns the fd, or -1.
static int perm_connect_agent(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/meshagent-kvm-%u.sock", (unsigned)getuid());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

// Bring the agent back NOW.
//
// Closing a session makes the agent exit, so how fast it comes back is
// launchd's ThrottleInterval. That used to be 30 in the installed LaunchAgent:
// measured 2026-09-10, the socket was still dead 40s after a session closed,
// which produced both "agent socket not reachable" and the "still not granted"
// loop (the walkthrough was reading a stale log while the agent was down).
// The installer now writes 5, but do not rely on it -- this program also runs
// against agents installed before that change. `launchctl kickstart -k`
// bypasses the throttle whatever it is; measured 13s to a listening agent.
static void perm_kickstart_agent(void)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd),
             "/bin/launchctl kickstart -k gui/%u/meshagent-launchagent >/dev/null 2>&1",
             (unsigned)getuid());
    (void)system(cmd);
}

// Is the agent process alive? Asked of launchd, never by connecting.
//
// A connection to that socket IS a session, and dropping one makes the agent
// exit -- so "probe by connect, then close" destroys the very thing it just
// found. Measured 2026-09-10: a wait loop built that way reported the agent up
// and the next connect() immediately failed, because the probe had killed it.
// Do not test it with `nc -z -U` either: that reports failure against a live
// listener, and the socket FILE outlives the process, so its presence proves
// nothing.
static int perm_agent_running(void)
{
    FILE *f = popen("/bin/launchctl list meshagent-launchagent 2>/dev/null", "r");
    if (!f) return 0;
    char line[512];
    int running = 0;
    while (fgets(line, sizeof(line), f) != NULL)
        if (strstr(line, "\"PID\"") != NULL) { running = 1; break; }
    pclose(f);
    return running;
}

// Wait until launchd reports the agent up again, up to `secs`. Returns 1 if so.
static int perm_wait_for_agent(int secs)
{
    for (int i = 0; i < secs * 2; i++)
    {
        if (perm_agent_running()) return 1;
        usleep(500000);
    }
    return 0;
}

static void perm_fake_kvm_session(void)
{
    // The connection that succeeds IS the session -- never open a throwaway one.
    int fd = perm_connect_agent();
    if (fd < 0)
    {
        // Almost always mid-respawn rather than absent: the previous session
        // made the agent exit, and its LaunchAgent throttles restarts by 30s.
        perm_kickstart_agent();
        for (int i = 0; i < 90 && fd < 0; i++) { usleep(500000); fd = perm_connect_agent(); }
        if (fd < 0)
        {
            printf("      (the agent is not reachable -- grant these in System Settings instead)\n");
            return;
        }
    }
    // Hold it open long enough for kvm_init() + the first capture, which is what
    // raises the dialog.
    sleep(6);
    close(fd);
}

// IMPORTANT: this walkthrough must NOT test the permissions itself.
//
// TCC answers a status query for the RESPONSIBLE process, and this program runs
// as a child of the installer, i.e. of the operator's shell. Measured on clean
// Tahoe 26.6.2 with both grants present in TCC.db (auth_value=2):
//     agent (launchd-spawned) : preflight=1            <- correct
//     this process (shell child): "NOT granted"        <- wrong
// Checking locally therefore loops forever telling the operator to grant
// something that is already granted -- which is what happened.
//
// So ask the AGENT. It already records both values every time it starts and
// every time a session initialises:
//     "AXIsProcessTrusted=%d at startup"
//     "kvm_init: ... preflight=%d"
// perm_fake_kvm_session() makes it re-run both, then we read the freshest line.

// The walkthrough runs as the console user, so this resolves to the log of the
// agent in that same session -- the one whose grants it is actually asking about.
#define PERM_AGENT_LOG_FMT "/tmp/kvm_debug-%u.log"

// Last integer following `key` in the agent's log, or `deflt` if never seen.
static int perm_log_last_int(const char *key, int deflt)
{
    char _p[64]; snprintf(_p, sizeof(_p), PERM_AGENT_LOG_FMT, (unsigned)getuid());
    FILE *f = fopen(_p, "r");
    if (!f) return deflt;
    char line[1024];
    int val = deflt;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *p = strstr(line, key);
        if (p != NULL)
        {
            int v;
            if (sscanf(p + strlen(key), "%d", &v) == 1) val = v;
        }
    }
    fclose(f);
    return val;
}

static void perm_fake_kvm_session(void);
static void perm_kickstart_agent(void);
static int  perm_wait_for_agent(int secs);

// Make the agent re-report: a session drives kvm_init (logs preflight), and a
// restart re-runs its startup probe (logs AXIsProcessTrusted, and prompts for
// Accessibility if it is still missing).
//
// The restart must be forced. Waiting for launchd costs ThrottleInterval --
// up to 30s on agents installed before that was lowered to 5 -- and the
// previous fixed sleep(4) simply read a stale log while the agent was dead.
static void perm_refresh_agent_status(void)
{
    perm_fake_kvm_session();
    perm_kickstart_agent();
    perm_wait_for_agent(45);
    sleep(2);   // let the startup lines land in the log
}

static int perm_has_accessibility(void) { return perm_log_last_int("AXIsProcessTrusted=", 0) != 0; }

static int perm_has_screencapture(void) { return perm_log_last_int("preflight=", 0) != 0; }

// Full Disk Access is deliberately NOT probed from this program.
//
// _fullDiskAuthorizationStatus() opens protected files, and TCC answers for the
// RESPONSIBLE process. Run from the installer that is the operator's shell, so
// the probe reports Terminal's access, never the agent's -- and, worse, the
// attempt makes tccd create a row for it. Measured 2026-09-10 on the operator's
// VM, after a walkthrough that had "failed" to verify FDA:
//     kTCCServiceSystemPolicyAllFiles|com.apple.Terminal|0                 <- us
//     kTCCServiceSystemPolicyAllFiles|/usr/local/.../meshagent/meshagent|2 <- real
// The grant was there the whole time; only our check was wrong, and it looped
// forever demanding something already done. Same reason the operator saw a
// stray Terminal entry appear in the Privacy lists.
//
// It is also the wrong binary to ask about: file transfer runs in the root
// daemon, not in this KVM helper, so the grant belongs on the daemon and this
// process could not observe it even with correct attribution. Step 3 therefore
// names the exact path to add and states plainly that it cannot verify it.

// Path of the root daemon, which is the binary that needs Full Disk Access.
// This program is <dir>/kvm/meshagent; the daemon is <dir>/meshagent.
static void perm_daemon_path(char *out, size_t outsz)
{
    char exe[PATH_MAX];
    uint32_t sz = sizeof(exe);
    if (_NSGetExecutablePath(exe, &sz) != 0) { snprintf(out, outsz, "the MeshAgent daemon"); return; }
    char real[PATH_MAX];
    if (realpath(exe, real) == NULL) snprintf(real, sizeof(real), "%s", exe);

    char *base = strrchr(real, '/');
    if (base == NULL) { snprintf(out, outsz, "%s", real); return; }
    *base = 0;                          // real = .../kvm
    char *parent = strrchr(real, '/');
    if (parent == NULL || strcmp(parent + 1, "kvm") != 0)
    {
        *base = '/';
        snprintf(out, outsz, "%s", real);
        return;
    }
    *parent = 0;                        // real = the install dir
    snprintf(out, outsz, "%s/%s", real, base + 1);
}

// Poll until granted or the user gives up. Returns 1 if granted.
// `check` reads the agent's own report, so refresh it before believing a "no".
static int perm_wait(const char *label, int (*check)(void), const char *pane)
{
    char buf[32];
    for (;;)
    {
        if (check()) { printf("      -> %s granted\n", label); return 1; }
        printf("      Grant it, then press Return here.\n");
        if (!perm_ask("      [Return]=recheck  o=open System Settings  s=skip : ", buf, sizeof(buf)))
        {
            printf("      (no terminal available -- skipping %s)\n", label);
            return 0;
        }
        if (buf[0] == 's' || buf[0] == 'S') { printf("      -> %s SKIPPED\n", label); return 0; }
        if (buf[0] == 'o' || buf[0] == 'O') { perm_open_pane(pane); continue; }
        printf("      checking with the agent...\n");
        perm_refresh_agent_status();
        if (check()) { printf("      -> %s granted\n", label); return 1; }
        printf("      still not granted.\n");
    }
}

// wantFDA: ask about Full Disk Access too (it is optional -- only needed for
// pushing/downloading files into protected locations on the managed Mac).
void kvm_request_permissions_interactive(int wantFDA)
{
    printf("\n=== MeshAgent: granting macOS privacy permissions ===\n");
    printf("Two permissions are required, one is optional. Each step raises a dialog\n");
    printf("(or opens System Settings); grant it there, then come back here.\n\n");

    // [1] Accessibility -- remote keyboard/mouse in the logged-in session.
    // AXIsProcessTrustedWithOptions raises Apple's own dialog, whose "Open
    // System Settings" button lands on the right pane WITH this program listed.
    printf("[1/3] Accessibility  (required: remote keyboard & mouse in-session)\n");
    if (perm_has_accessibility()) { printf("      -> already granted\n"); }
    else
    {
        // Ask the AGENT to raise it, exactly as step 2 does for Screen Recording.
        //
        // Calling AXIsProcessTrustedWithOptions() here used to work well enough
        // that meshagent appeared in the pane, but it also made tccd record
        // kTCCServiceAccessibility|com.apple.Terminal (measured 2026-09-10) and
        // produced a SECOND dialog when the agent restarted and prompted for
        // itself. One prompt, correctly attributed, is better than two.
        printf("      restarting the agent so macOS raises its dialog...\n");
        perm_kickstart_agent();
        perm_wait_for_agent(45);
        sleep(2);
        perm_wait("Accessibility", perm_has_accessibility, "Privacy_Accessibility");
    }

    // [2] Screen Recording -- the remote screen.
    //
    // This step must NOT attempt the capture itself. TCC attributes a
    // ScreenCapture request to the process RESPONSIBLE for the requester, not to
    // the requester. The installer runs this walkthrough as a child of the
    // operator's shell, so measured attribution was
    // responsible={com.apple.Terminal} (and sshd-keygen-wrapper when driven over
    // ssh) -- the grant would land on Terminal, meshagent would never get a TCC
    // record of its own, and the Screen Recording pane would list nothing to
    // toggle. Which is exactly what happened.
    //
    // A launchd-spawned process is its own responsible process, so the capture
    // has to come from the -kvmagent LaunchAgent. Kick it so it re-runs its
    // startup probe; that raises Apple's dialog attributed to meshagent and
    // creates the record that puts it in the pane. Then just pace the operator.
    // (Accessibility above does not need this: AXIsProcessTrustedWithOptions
    // attributes to the calling process, which is why its grant landed correctly.)
    printf("\n[2/3] Screen Recording  (required: the remote screen)\n");
    if (perm_has_screencapture()) { printf("      -> already granted\n"); }
    else
    {
        printf("      opening a short local KVM session so macOS raises its dialog...\n");
        perm_fake_kvm_session();
        perm_wait("Screen Recording", perm_has_screencapture, "Privacy_ScreenCapture");
    }

    // [3] Full Disk Access -- optional, and the one step that cannot be verified
    // from here (see perm_daemon_path above). Name the exact binary and move on
    // rather than looping on a check that is guaranteed to answer about the
    // operator's shell.
    printf("\n[3/3] Full Disk Access  (optional: file transfer to protected locations)\n");
    if (!wantFDA) { printf("      -> not requested\n"); }
    else
    {
        char ans[32], dpath[PATH_MAX];
        perm_daemon_path(dpath, sizeof(dpath));
        printf("      Needed only to push/download files into protected folders\n");
        printf("      (Desktop, Documents, Downloads, Mail, etc.) on this Mac.\n");
        if (perm_ask("      Grant Full Disk Access now? [y/N]: ", ans, sizeof(ans))
            && (ans[0] == 'y' || ans[0] == 'Y'))
        {
            perm_open_pane("Privacy_AllFiles");
            printf("      In the list that just opened, click \"+\", press\n");
            printf("      Shift-Command-G, and paste this exact path:\n\n");
            printf("          %s\n\n", dpath);
            printf("      then make sure its switch is ON.\n");
            printf("      (This one cannot be checked from here -- the check would\n");
            printf("       report this Terminal's access, not the agent's.)\n");
            perm_ask("      Press Return when done : ", ans, sizeof(ans));
            printf("      -> Full Disk Access: left to you to confirm in the pane\n");
        }
        else { printf("      -> skipped (can be granted later in System Settings)\n"); }
    }

    printf("\n  confirming with the agent...\n");
    perm_refresh_agent_status();
    printf("\n=== permissions step complete ===\n");
    printf("  Accessibility   : %s\n", perm_has_accessibility() ? "granted" : "NOT granted");
    printf("  Screen Recording: %s\n", perm_has_screencapture() ? "granted" : "NOT granted");
    // Not asserted either way: this process cannot see the agent's FDA state, and
    // probing from here reports the responsible process's access, not the agent's.
    printf("  Full Disk Access: optional -- confirm in System Settings if you use file transfer\n\n");
}

// Are two files byte-identical? Used to decide whether a refresh is needed at
// all, so the common case (already in step) costs one read and no writes.
static int kvm_files_identical(const char *a, const char *b)
{
	struct stat sa, sb;
	if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
	if (sa.st_size != sb.st_size) return 0;
	FILE *fa = fopen(a, "rb"); if (!fa) return 0;
	FILE *fb = fopen(b, "rb"); if (!fb) { fclose(fa); return 0; }
	char ba[65536], bb[65536];
	int same = 1;
	for (;;)
	{
		size_t ra = fread(ba, 1, sizeof(ba), fa);
		size_t rb = fread(bb, 1, sizeof(bb), fb);
		if (ra != rb || memcmp(ba, bb, ra) != 0) { same = 0; break; }
		if (ra == 0) break;
	}
	fclose(fa); fclose(fb);
	return same;
}

// Make sure the LaunchDaemon restarts unconditionally.
//
// MeshCentral's self-update replaces this binary underneath the running
// process, and the kernel then SIGKILLs it for a code-signing violation.
// Installs made before 2026-09-15 carry KeepAlive {Crashed: true}, and launchd
// did NOT bring the agent back from that -- measured on a deployed Mac:
//     last exit reason = OS_REASON_CODESIGNING
//     state = not running        runs = 6
// So a SUCCESSFUL update could leave a machine with no management agent, which
// is the one failure a remote agent cannot ask you to fix remotely.
//
// The installer now writes KeepAlive <true/>, but a plist is not carried by an
// agent update -- only the binary is -- so already-deployed Macs keep the old
// one. Repairing it from here is the only route that needs nobody at a keyboard.
//
// The reload runs in a DETACHED helper on purpose: bootout terminates this very
// process, so the bootstrap half must outlive us or the job is left unloaded.
// Worst case that still ends at "offline until a reboot", which is exactly where
// the machine would have been anyway.
//
// Only acts when the plist is actually wrong, so it cannot loop: once repaired,
// every later start reads "true" and returns immediately.
void kvm_fix_daemon_keepalive(void)
{
	if (getuid() != 0) return;
	const char *P = "/Library/LaunchDaemons/meshagent.plist";
	struct stat st;
	if (stat(P, &st) != 0) return;          // not our layout -- leave it alone

	int already = 0;
	FILE *f = popen("/usr/libexec/PlistBuddy -c 'Print :KeepAlive' "
	                "/Library/LaunchDaemons/meshagent.plist 2>/dev/null", "r");
	if (f != NULL)
	{
		char b[64];
		if (fgets(b, sizeof(b), f) != NULL && strncmp(b, "true", 4) == 0) already = 1;
		pclose(f);
	}
	if (already) return;

	kvm_flog("kvm_keepalive: LaunchDaemon does not restart unconditionally -- repairing\n");

	// Edit the file SYNCHRONOUSLY. This is the part that must not be lost, and an
	// earlier version did lose it: the whole repair was handed to one backgrounded
	// `nohup sh -c '...' &` through system(), the function logged that it was
	// repairing, and the plist was never touched. Keep the must-happen work here,
	// in plain sight, and detach only what genuinely cannot run in this process.
	(void)system("/bin/cp -p /Library/LaunchDaemons/meshagent.plist "
	             "/Library/LaunchDaemons/meshagent.plist.bak-keepalive >/dev/null 2>&1");
	(void)system("/usr/libexec/PlistBuddy -c 'Delete :KeepAlive' "
	             "/Library/LaunchDaemons/meshagent.plist >/dev/null 2>&1");
	(void)system("/usr/libexec/PlistBuddy -c 'Add :KeepAlive bool true' "
	             "/Library/LaunchDaemons/meshagent.plist >/dev/null 2>&1");
	if (system("/usr/bin/plutil -lint /Library/LaunchDaemons/meshagent.plist >/dev/null 2>&1") != 0)
	{
		// Never leave an unparseable LaunchDaemon behind: launchd would refuse to
		// load it and the machine would lose its agent permanently.
		(void)system("/bin/cp -p /Library/LaunchDaemons/meshagent.plist.bak-keepalive "
		             "/Library/LaunchDaemons/meshagent.plist >/dev/null 2>&1");
		kvm_flog("kvm_keepalive: edit produced an invalid plist -- restored the original\n");
		return;
	}
	kvm_flog("kvm_keepalive: plist repaired; reloading the job\n");

	// launchd caches the job definition, so only bootout+bootstrap picks the new
	// KeepAlive up; a restart alone re-reads nothing. bootout kills THIS process,
	// so the bootstrap half has to live outside it. fork+setsid, not a backgrounded
	// shell: the child is then in its own session and is not torn down with the job.
	{
		pid_t pid = fork();
		if (pid == 0)
		{
			setsid();
			execl("/bin/sh", "sh", "-c",
				"sleep 3; "
				"/bin/launchctl bootout system /Library/LaunchDaemons/meshagent.plist >/dev/null 2>&1; "
				"sleep 2; "
				"/bin/launchctl bootstrap system /Library/LaunchDaemons/meshagent.plist >/dev/null 2>&1",
				(char *)NULL);
			_exit(127);
		}
	}
}

// Keep <dir>/kvm/<exe> in step with the daemon binary.
//
// MeshCentral updates the DAEMON on its own -- that is what the console's
// "force agent update" drives -- but every bit of KVM work runs in a LaunchAgent
// executing a COPY at <dir>/kvm/<exe>, and until now nothing except the
// installer ever wrote that copy. Observed on a deployed Mac: minutes after a
// server-side deploy the daemon had replaced itself while kvm/<exe> sat on the
// previous build indefinitely. So a fleet silently ran new daemons against old
// KVM agents, and every KVM fix landed only when someone re-ran the installer by
// hand on each machine. "Force agent update" could not fix it because it does
// not know that second file exists.
//
// Runs from the daemon at startup, so it happens right after any self-update.
void kvm_sync_agent_copy(void)
{
	if (getuid() != 0) return;            // only the root daemon owns that file

	char exe[PATH_MAX]; uint32_t sz = (uint32_t)sizeof(exe);
	if (_NSGetExecutablePath(exe, &sz) != 0) return;
	char self[PATH_MAX];
	if (realpath(exe, self) == NULL) return;

	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", self);
	char *base = strrchr(dir, '/');
	if (base == NULL) return;
	*base++ = 0;                          // dir = directory, base = file name

	// If we ARE the copy, stop: the kvmagent must never rewrite itself.
	const char *dname = strrchr(dir, '/');
	if (dname != NULL && strcmp(dname + 1, "kvm") == 0) return;

	char dst[PATH_MAX], tmp[PATH_MAX];
	snprintf(dst, sizeof(dst), "%s/kvm/%s", dir, base);
	struct stat st;
	if (stat(dst, &st) != 0) { kvm_flog("kvm_sync: no %s\n", dst); return; }
	if (kvm_files_identical(self, dst)) { kvm_flog("kvm_sync: %s already in step\n", dst); return; }
	kvm_flog("kvm_sync: %s differs -- refreshing\n", dst);

	snprintf(tmp, sizeof(tmp), "%s.new", dst);
	// The copy can be pinned immutable (chflags uchg); clear that or the write
	// fails with EPERM even as root.
	chflags(dst, 0);
	unlink(tmp);

	FILE *in = fopen(self, "rb"); if (!in) return;
	FILE *out = fopen(tmp, "wb"); if (!out) { fclose(in); return; }
	char buf[65536]; size_t r; int ok = 1;
	while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
		if (fwrite(buf, 1, r, out) != r) { ok = 0; break; }
	fclose(in);
	if (fclose(out) != 0) ok = 0;
	if (!ok) { unlink(tmp); kvm_flog("kvm_sync: copy failed\n"); return; }

	chmod(tmp, 0755);
	if (chown(tmp, 0, 0) != 0) { /* best effort */ }
	// Rename rather than overwrite: the kvmagent may be executing the old file,
	// and a rename swaps only the directory entry, leaving its inode intact.
	if (rename(tmp, dst) != 0) { unlink(tmp); kvm_flog("kvm_sync: rename failed\n"); return; }
	kvm_flog("kvm_sync: refreshed %s from the daemon binary\n", dst);

	// Restart EVERY kvmagent, not just the console user's.
	//
	// The same LaunchAgent is loaded into both the Aqua and the LoginWindow
	// session types, so there are normally two of them: one as the console user
	// and one as root at the login window. kickstart on gui/<uid> reaches only
	// the first. Observed on a deployed Mac 2026-09-15: after a successful
	// refresh the console agent was 43 seconds old and the LOGIN-WINDOW agent was
	// still 30 minutes old, i.e. still executing the previous binary -- which in
	// that instance was the build with the SkyLight deadlock, so login-window
	// connections would have kept coming up black despite the update having
	// "worked".
	//
	// Signalling them is enough: KeepAlive is {SuccessfulExit: false}, so a
	// signal death counts as unsuccessful and launchd brings each one back, in
	// its own session, running the new binary.
	(void)system("/usr/bin/pkill -f 'kvm/meshagent -kvmagent' >/dev/null 2>&1");

	// Then kickstart the console user's explicitly: that one is the interactive
	// session, and kickstart bypasses ThrottleInterval so it returns at once
	// instead of after the restart delay.
	uid_t cuid = 0;
	SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("meshagent-sync"), NULL, NULL);
	if (store != NULL)
	{
		CFStringRef name = SCDynamicStoreCopyConsoleUser(store, &cuid, NULL);
		if (name != NULL) CFRelease(name); else cuid = 0;
		CFRelease(store);
	}
	if (cuid > 0)
	{
		char cmd[192];
		snprintf(cmd, sizeof(cmd),
			"/bin/launchctl kickstart -k gui/%u/meshagent-launchagent >/dev/null 2>&1",
			(unsigned)cuid);
		(void)system(cmd);
		kvm_flog("kvm_sync: restarted the kvmagent for uid %u\n", (unsigned)cuid);
	}
}

void kvm_check_permission()
{
	// NOTE: this function is DEAD CODE on macOS. Its only call site, in
	// agentcore.c, is guarded by #if defined(_LINKVM), and the makefile's macos
	// target never defines it. Startup work belongs in main(), which is where
	// kvm_sync_agent_copy() and kvm_fix_daemon_keepalive() are called from --
	// they were briefly wired up here instead and silently never ran.


    //Request screen recording access
    if(__builtin_available(macOS 10.15, *)){
        if(!CGPreflightScreenCaptureAccess()) {
            CGRequestScreenCaptureAccess();
        }
    }


    // Request accessibility access
    if(__builtin_available(macOS 10.9, *)){
        const void * keys[] = { kAXTrustedCheckOptionPrompt };
        const void * values[] = { kCFBooleanTrue };

        CFDictionaryRef options = CFDictionaryCreate(
            kCFAllocatorDefault,
            keys,
            values,
            sizeof(keys) / sizeof(*keys),
            &kCFCopyStringDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);

        AXIsProcessTrustedWithOptions(options);
    }

    // Request full disk access
    if(__builtin_available(macOS 10.14, *)) {
        if(_fullDiskAuthorizationStatus() != MPAuthorizationStatusAuthorized) {
            CFStringRef URL =  CFStringCreateWithCString(NULL, "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles", kCFStringEncodingASCII);
            CFURLRef pathRef = CFURLCreateWithString( NULL, URL, NULL );
            if( pathRef )
            {
                LSOpenCFURLRef(pathRef, NULL);
                CFRelease(pathRef);
            }
            CFRelease(URL);
        }
    }
}

// Background thread for -kvmagent mode: polls Accessibility trust status every
// 3 seconds. When the user grants the permission after the initial prompt,
// exits with code 2 so launchd restarts the process. The restarted process
// will call AXIsProcessTrustedWithOptions and get true on the first check,
// making CGEventPost work immediately without manual intervention.
static void* ax_poll_thread(void* arg)
{
    (void)arg;
    // Give the process a few seconds to start before checking — avoids
    // an immediate restart loop if TCC is already granted at launch.
    sleep(5);
    while (1)
    {
        if (AXIsProcessTrusted())
        {
            // Permission just became true (it was false when we started
            // polling, otherwise we wouldn't have started). Restart so the
            // main thread picks up the new trust state for CGEventPost.
            exit(2);
        }
        sleep(3);
    }
    return NULL;
}

int kvmagent_ax_poll_start(void)
{
    if (__builtin_available(macOS 10.9, *))
    {
        // Only start the poll thread if Accessibility is NOT yet granted.
        // If it's already granted at launch, no restart needed.
        if (!AXIsProcessTrusted())
        {
            // Raise Apple's dialog from HERE rather than from the installer's
            // walkthrough. This process is spawned by launchd, so it is its own
            // responsible process and the grant/record land on meshagent. The
            // walkthrough calling this itself attributed to the operator's shell
            // and left a stray com.apple.Terminal row behind (measured
            // 2026-09-10), as well as prompting twice.
            const void *keys[] = { kAXTrustedCheckOptionPrompt };
            const void *values[] = { kCFBooleanTrue };
            CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault,
                keys, values, 1,
                &kCFCopyStringDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
            AXIsProcessTrustedWithOptions(options);
            CFRelease(options);

            pthread_t t;
            pthread_create(&t, NULL, ax_poll_thread, NULL);
            pthread_detach(t);
        }
    }
    return 0;
}