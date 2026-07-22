#include "mac_events.h"
#include <assert.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <IOKit/hidsystem/IOHIDUserDevice.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <CommonCrypto/CommonCryptor.h>
#include "../../../microstack/ILibParsers.h"
#include "../../meshdefines.h"
// csops constants (from private sys/codesign.h)
#define CS_OPS_STATUS                0
#define CS_OPS_ENTITLEMENTS_BLOB     7
#define CS_VALID                     0x00000001
#define CS_ADHOC                     0x00000002
#define CS_PLATFORM_BINARY           0x04000000
#define CS_ENTITLEMENTS_VALIDATED    0x00004000  // bit 14 (NOT 0x20000 which is CS_LINKER_SIGNED)
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#include <arpa/inet.h>  // ntohl

// mach_port_kernel_object is in <mach/mach_port.h> (SDK declares it with ipc_space_read_t).
// IKOT_IOKIT_OBJECT (24) = io_service_t / io_iterator_t
// IKOT_IOKIT_CONNECT (25) = io_connect_t  (the type we want to steal)
// IKOT_IOKIT_IDENT   (26) = IOIdentity port
#define IKOT_IOKIT_OBJECT  24
#define IKOT_IOKIT_CONNECT 25
#define IKOT_IOKIT_IDENT   26

// ---------------------------------------------------------------------------
// Input injection strategy (in priority order):
//  1. AppleVirtualPlatformHIDInterface (AVP) — the virtual keyboard kernel
//     driver used by the Parallels VM.  Injecting here is equivalent to
//     typing on the real keyboard hardware; it is completely invisible to
//     SecureEventInput and works at the loginwindow.
//     Requires com.apple.Virtualization.AppleVirtualPlatformHIDInterfaceUserClient.
//
//  2. IOHIDUserDevice — creates a virtual keyboard at the kernel HID level.
//     Also bypasses SecureEventInput.  Requires com.apple.hid.manager.user-device.
//
//  3. CGEvent (PostToPid / Post) — user-space event injection; blocked at
//     loginwindow by SecureEventInput but kept as a last-resort fallback for
//     normal user sessions.
// ---------------------------------------------------------------------------

// ---- AppleVirtualPlatformHIDInterface globals (priority 1) ----------------
static io_connect_t g_avp_kbd_conn   = IO_OBJECT_NULL;
static uint8_t      g_avp_mods       = 0;
static uint8_t      g_avp_held[6]    = {0};

// ---- IOHIDUserDevice globals (priority 2) ----------------------------------
static IOHIDUserDeviceRef g_kbd_dev  = NULL;
static IOHIDUserDeviceRef g_mouse_dev = NULL;

// ---- CGEvent fallback globals ----------------------------------------------
#define POST_MAX_INFLIGHT 16
static dispatch_queue_t     g_postQ;
static dispatch_semaphore_t g_postSem;
static dispatch_once_t      g_postOnce;
static CGEventSourceRef     g_source = NULL;
static pid_t                g_lw_pid = -1;   // loginwindow pid for direct post

// ---- VNC keyboard fallback globals (priority 4, loginwindow only) ----------
static int      g_vnc_fd          = -1;   // TCP socket to localhost:5900, -1 if disconnected
static time_t   g_vnc_last_attempt = 0;   // unix timestamp of last connect attempt
static int      g_lw_cached       = -1;   // cached loginwindow state (-1=unknown, 0=no, 1=yes)
static time_t   g_lw_cache_time   = 0;    // timestamp of last loginwindow check
static int      g_vnc_shift_down  = 0;    // tracks physical Shift state for the VNC key path

// ---- Helpers ---------------------------------------------------------------
static pid_t find_proc_by_name(const char *name)
{
    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
    size_t sz = 0;
    if (sysctl(mib, 3, NULL, &sz, NULL, 0) != 0) return -1;
    struct kinfo_proc *procs = malloc(sz);
    if (!procs) return -1;
    if (sysctl(mib, 3, procs, &sz, NULL, 0) != 0) { free(procs); return -1; }
    pid_t found = -1;
    for (int i = 0; i < (int)(sz / sizeof(*procs)); i++) {
        if (strncmp(procs[i].kp_proc.p_comm, name, MAXCOMLEN) == 0) {
            found = procs[i].kp_proc.p_pid;
            break;
        }
    }
    free(procs);
    return found;
}

static uint8_t adb_to_hid(CGKeyCode adb);
static uint8_t adb_mod_mask(CGKeyCode adb);
static int is_loginwindow(void);

// Open a connection to the AppleVirtualPlatformHIDInterface service.
// location_id=0 → Virtual Keyboard, location_id=1 → Virtual Trackpad.
// Tries user client types 0, 1, 2 in sequence.
// If all fail, kills the bridge daemon (its slot may be the only one) and retries.
static io_connect_t open_avp_hid_interface(int location_id)
{
    CFMutableDictionaryRef match = IOServiceMatching("AppleVirtualPlatformHIDInterface");
    if (!match) return IO_OBJECT_NULL;
    CFNumberRef lidNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &location_id);
    CFDictionarySetValue(match, CFSTR("LocationID"), lidNum);
    CFRelease(lidNum);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (svc == IO_OBJECT_NULL) {
        write(STDOUT_FILENO, "avp_hid: service not found\n", 27);
        return IO_OBJECT_NULL;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = KERN_FAILURE;
    // Try types 0, 1, 2 — the kext may only accept a specific type value.
    for (int utype = 0; utype <= 2 && kr != KERN_SUCCESS; utype++) {
        conn = IO_OBJECT_NULL;
        kr = IOServiceOpen(svc, mach_task_self(), (uint32_t)utype, &conn);
        char b[80]; int l = snprintf(b, sizeof(b),
            "avp_hid: open loc=%d type=%d kr=0x%x\n", location_id, utype, kr);
        write(STDOUT_FILENO, b, l);
    }

    if (kr != KERN_SUCCESS) {
        // Kill the bridge daemon so its user client slot is released, then retry.
        pid_t bridge_pid = find_proc_by_name("AppleVirtualPlat");
        if (bridge_pid > 0) {
            char b[64]; int l = snprintf(b, sizeof(b),
                "avp_hid: killing bridge pid=%d\n", (int)bridge_pid);
            write(STDOUT_FILENO, b, l);
            kill(bridge_pid, SIGKILL);
            usleep(500000);   // 500ms for kernel to release the user client slot
        }
        for (int utype = 0; utype <= 2 && kr != KERN_SUCCESS; utype++) {
            conn = IO_OBJECT_NULL;
            kr = IOServiceOpen(svc, mach_task_self(), (uint32_t)utype, &conn);
            char b[80]; int l = snprintf(b, sizeof(b),
                "avp_hid: retry loc=%d type=%d kr=0x%x\n", location_id, utype, kr);
            write(STDOUT_FILENO, b, l);
        }
    }

    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) return IO_OBJECT_NULL;
    write(STDOUT_FILENO, "avp_hid: connection OK\n", 23);
    return conn;
}

// (No threading helpers needed — mach_port_kernel_object is a fast non-messaging syscall.)

// Locate the bridge daemon's keyboard io_connect_t via task_for_pid + port scan.
// Uses a thread-per-port probe so that user-space ports that never reply don't hang us.
// We don't need the AVP entitlement here — we borrow the bridge daemon's existing connection.
static io_connect_t steal_avp_kbd_from_bridge(void)
{
    pid_t bridge_pid = find_proc_by_name("AppleVirtualPlat");
    if (bridge_pid < 0) {
        write(STDOUT_FILENO, "avp_steal: bridge daemon not found\n", 35);
        return IO_OBJECT_NULL;
    }
    {
        char b[64]; int l = snprintf(b, sizeof(b), "avp_steal: bridge_pid=%d\n", (int)bridge_pid);
        write(STDOUT_FILENO, b, l);
    }

    task_t bridge_task = TASK_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), bridge_pid, &bridge_task);
    if (kr != KERN_SUCCESS || bridge_task == TASK_NULL) {
        char b[64]; int l = snprintf(b, sizeof(b), "avp_steal: task_for_pid kr=0x%x\n", kr);
        write(STDOUT_FILENO, b, l);
        return IO_OBJECT_NULL;
    }

    mach_port_name_array_t names = NULL;
    mach_port_type_array_t types = NULL;
    mach_msg_type_number_t name_cnt = 0, type_cnt = 0;
    kr = mach_port_names(bridge_task, &names, &name_cnt, &types, &type_cnt);
    if (kr != KERN_SUCCESS) {
        char b[64]; int l = snprintf(b, sizeof(b), "avp_steal: mach_port_names kr=0x%x\n", kr);
        write(STDOUT_FILENO, b, l);
        mach_port_deallocate(mach_task_self(), bridge_task);
        return IO_OBJECT_NULL;
    }
    {
        mach_msg_type_number_t send_cnt = 0;
        for (mach_msg_type_number_t j = 0; j < name_cnt; j++)
            if (types[j] & MACH_PORT_TYPE_SEND) send_cnt++;
        char b[96]; int l = snprintf(b, sizeof(b),
            "avp_steal: %u ports total, %u have SEND right\n", name_cnt, send_cnt);
        write(STDOUT_FILENO, b, l);
    }

    io_connect_t found = IO_OBJECT_NULL;

    for (mach_msg_type_number_t i = 0; i < name_cnt; i++) {
        if (!(types[i] & MACH_PORT_TYPE_SEND)) continue;

        mach_port_t local_port = MACH_PORT_NULL;
        mach_msg_type_name_t type_out = 0;
        kr = mach_port_extract_right(bridge_task, names[i],
            MACH_MSG_TYPE_COPY_SEND, &local_port, &type_out);
        if (kr != KERN_SUCCESS) {
            char b[64]; int l = snprintf(b, sizeof(b),
                "avp_steal: port[%u]=0x%x extract kr=0x%x\n", i, names[i], kr);
            write(STDOUT_FILENO, b, l);
            continue;
        }

        // Use mach_port_kernel_object: a non-messaging syscall that returns the
        // Mach kobject type of the port.  No messages sent — never hangs.
        unsigned int obj_type = 0;
        unsigned int obj_addr = 0;
        kern_return_t ko_kr = mach_port_kernel_object(
            mach_task_self(), local_port, &obj_type, &obj_addr);
        {
            char b[96]; int l = snprintf(b, sizeof(b),
                "avp_steal: port[%u]=0x%x kobject type=%u ko_kr=0x%x\n",
                i, local_port, obj_type, ko_kr);
            write(STDOUT_FILENO, b, l);
        }

        // Only IKOT_IOKIT_CONNECT (25) ports are io_connect_t.
        // IKOT_IOKIT_OBJECT (24) are io_service_t / io_iterator_t; skip them.
        if (ko_kr != KERN_SUCCESS || obj_type != IKOT_IOKIT_CONNECT) {
            mach_port_deallocate(mach_task_self(), local_port);
            continue;
        }

        // Confirmed io_connect_t — get the backing service and check class + LocationID.
        io_service_t service = IO_OBJECT_NULL;
        kern_return_t svc_kr = IOConnectGetService(local_port, &service);
        if (svc_kr != KERN_SUCCESS || service == IO_OBJECT_NULL) {
            char b[96]; int l = snprintf(b, sizeof(b),
                "avp_steal: port[%u] CONNECT but get_svc kr=0x%x\n", i, svc_kr);
            write(STDOUT_FILENO, b, l);
            // Still an io_connect_t; try using it if we can't identify it.
            // Keep it and try method 1 with a zero keyboard report as a last resort.
            if (found == IO_OBJECT_NULL) found = local_port;
            else mach_port_deallocate(mach_task_self(), local_port);
            continue;
        }

        io_name_t class_name = {0};
        IOObjectGetClass(service, class_name);
        int loc_id = -1;
        CFTypeRef lid = IORegistryEntryCreateCFProperty(service, CFSTR("LocationID"),
                                                        kCFAllocatorDefault, 0);
        if (lid) {
            if (CFGetTypeID(lid) == CFNumberGetTypeID())
                CFNumberGetValue((CFNumberRef)lid, kCFNumberIntType, &loc_id);
            CFRelease(lid);
        }
        IOObjectRelease(service);
        {
            char b[128]; int l = snprintf(b, sizeof(b),
                "avp_steal: port[%u]=0x%x CONNECT class=%s loc=%d\n",
                i, local_port, class_name, loc_id);
            write(STDOUT_FILENO, b, l);
        }
        if (strcmp(class_name, "AppleVirtualPlatformHIDInterface") == 0 && loc_id == 0) {
            found = local_port;
        } else {
            mach_port_deallocate(mach_task_self(), local_port);
        }
    }

    vm_deallocate(mach_task_self(), (vm_address_t)names, name_cnt * sizeof(*names));
    vm_deallocate(mach_task_self(), (vm_address_t)types, type_cnt * sizeof(*types));
    mach_port_deallocate(mach_task_self(), bridge_task);

    if (found == IO_OBJECT_NULL)
        write(STDOUT_FILENO, "avp_steal: kbd conn not found\n", 30);
    else
        write(STDOUT_FILENO, "avp_steal: kbd conn stolen!\n", 28);
    return found;
}

// Send the current AVP keyboard state (boot-protocol 8-byte report) via selector 1.
// Returns KERN_SUCCESS on success, or the IOKit error code on failure.
// On kIOReturnNotPermitted, nulls out g_avp_kbd_conn so subsequent calls skip AVP
// and fall through to the CGEventPost path in inject_key().
static kern_return_t avp_send_kbd_report(void)
{
    if (g_avp_kbd_conn == IO_OBJECT_NULL) return kIOReturnNotReady;
    uint8_t report[8] = {
        g_avp_mods, 0,
        g_avp_held[0], g_avp_held[1], g_avp_held[2],
        g_avp_held[3], g_avp_held[4], g_avp_held[5]
    };
    kern_return_t kr = IOConnectCallStructMethod(g_avp_kbd_conn, 1,
        report, sizeof(report), NULL, NULL);
    if (kr != KERN_SUCCESS) {
        static int avp_warned = 0;
        if (!avp_warned) {
            avp_warned = 1;
            char b[80]; int l = snprintf(b, sizeof(b), "avp_kbd: sendReport kr=0x%x\n", kr);
            write(STDOUT_FILENO, b, l);
        }
        if (kr == (kern_return_t)0xe00002c2 /* kIOReturnNotPermitted — Parallels bridge unsupported */) {
            g_avp_kbd_conn = IO_OBJECT_NULL;
        }
    }
    return kr;
}

// Returns 1 on success, 0 if AVP is unavailable or the send failed.
static int avp_kbd_key(CGKeyCode adb, int down)
{
    uint8_t mod = adb_mod_mask(adb);
    if (mod) {
        if (down) g_avp_mods |= mod; else g_avp_mods &= ~mod;
        return avp_send_kbd_report() == KERN_SUCCESS ? 1 : 0;
    }
    uint8_t hid = adb_to_hid(adb);
    if (!hid || hid >= 0xE0) return 0;
    if (down) {
        for (int i = 0; i < 6; i++) if (!g_avp_held[i]) { g_avp_held[i] = hid; break; }
    } else {
        for (int i = 0; i < 6; i++) if (g_avp_held[i] == hid) { g_avp_held[i] = 0; break; }
    }
    return avp_send_kbd_report() == KERN_SUCCESS ? 1 : 0;
}

static IOHIDUserDeviceRef make_hid_device(CFDictionaryRef props)
{
    // IOHIDUserDevice needs the restricted com.apple.hid.manager.user-device
    // entitlement, which ad-hoc-signed binaries cannot carry. We never actually
    // use this virtual-HID path — login-window input goes through screensharingd
    // VNC and in-session input through CGEventPost — so it is skipped here.
    //
    // Skipping it also fixes a hard crash on macOS 11 (Big Sur): there the kernel
    // denies the create ("IOHIDResourceDeviceUserClient ... not entitled",
    // IOServiceOpen 0xe00002c2) but IOHIDUserDeviceCreateWithProperties still
    // returns a NON-NULL, invalid ref, and IOHIDUserDeviceActivate() on it
    // crashes the process right after kvm_init — before the capture loop — so the
    // agent restart-loops and the remote screen is permanently black. (On macOS
    // 14+ the create returned NULL and this was already a no-op.)
    (void)props;
    return NULL;
}

// Standard boot-protocol keyboard report descriptor
static const uint8_t kKbdDescriptor[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01,
    /* modifier byte */
    0x75,0x01, 0x95,0x08, 0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x81,0x02,
    /* reserved byte */
    0x75,0x08, 0x95,0x01, 0x81,0x01,
    /* 6 key slots */
    0x75,0x08, 0x95,0x06, 0x15,0x00, 0x25,0xFF, 0x05,0x07, 0x19,0x00, 0x29,0xFF, 0x81,0x00,
    0xC0
};

// Simple relative mouse report descriptor (3 buttons + rel X/Y)
static const uint8_t kMouseDescriptor[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
    /* 3 buttons */
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x03, 0x81,0x02,
    /* 5-bit padding */
    0x75,0x05, 0x95,0x01, 0x81,0x01,
    /* X and Y, -127..127 */
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
    0xC0,0xC0
};

// ---- IOHIDUserDevice keyboard report ---------------------------------------
#pragma pack(push,1)
typedef struct { uint8_t mods; uint8_t reserved; uint8_t keys[6]; } KbdReport;
typedef struct { uint8_t buttons; int8_t dx; int8_t dy; } MouseReport;
#pragma pack(pop)

static uint8_t  g_mods = 0;
static uint8_t  g_held[6] = {0};  // currently held HID keycodes

// ADB (macOS) keycode → USB HID page-7 keycode.
// Values >= 0xE0 are modifier bits in the HID modifier byte (handled separately).
static uint8_t adb_to_hid(CGKeyCode adb)
{
    static const uint8_t t[128] = {
    /* 00 a  */ 0x04, /* 01 s  */ 0x16, /* 02 d  */ 0x07, /* 03 f  */ 0x09,
    /* 04 h  */ 0x0B, /* 05 g  */ 0x0A, /* 06 z  */ 0x1D, /* 07 x  */ 0x1B,
    /* 08 c  */ 0x06, /* 09 v  */ 0x19, /* 0A    */ 0x00, /* 0B b  */ 0x05,
    /* 0C q  */ 0x14, /* 0D w  */ 0x1A, /* 0E e  */ 0x08, /* 0F r  */ 0x15,
    /* 10 y  */ 0x1C, /* 11 t  */ 0x17, /* 12 1  */ 0x1E, /* 13 2  */ 0x1F,
    /* 14 3  */ 0x20, /* 15 4  */ 0x21, /* 16 6  */ 0x23, /* 17 5  */ 0x22,
    /* 18 =  */ 0x2E, /* 19 9  */ 0x26, /* 1A 7  */ 0x24, /* 1B -  */ 0x2D,
    /* 1C 8  */ 0x25, /* 1D 0  */ 0x27, /* 1E ]  */ 0x30, /* 1F o  */ 0x12,
    /* 20 u  */ 0x18, /* 21 [  */ 0x2F, /* 22 i  */ 0x0C, /* 23 p  */ 0x13,
    /* 24 ret*/ 0x28, /* 25 l  */ 0x0F, /* 26 j  */ 0x0D, /* 27 '  */ 0x34,
    /* 28 k  */ 0x0E, /* 29 ;  */ 0x33, /* 2A \  */ 0x31, /* 2B ,  */ 0x36,
    /* 2C /  */ 0x38, /* 2D n  */ 0x11, /* 2E m  */ 0x10, /* 2F .  */ 0x37,
    /* 30 tab*/ 0x2B, /* 31 sp */ 0x2C, /* 32 `  */ 0x35, /* 33 bs */ 0x2A,
    /* 34    */ 0x00, /* 35 esc*/ 0x29, /* 36 rCmd*/0xE7, /* 37 Cmd*/ 0xE3,
    /* 38 Shf*/ 0xE1, /* 39 CpL*/ 0x39, /* 3A Opt*/ 0xE2, /* 3B Ctl*/ 0xE0,
    /* 3C rSh*/ 0xE5, /* 3D rOp*/ 0xE6, /* 3E rCt*/ 0xE4, /* 3F Fn */ 0x00,
    /* 40 F17*/ 0x6C, /* 41 k. */ 0x63, /* 42    */ 0x00, /* 43 k* */ 0x55,
    /* 44    */ 0x00, /* 45 k+ */ 0x57, /* 46    */ 0x00, /* 47 kCl*/ 0x53,
    /* 48 Vup*/ 0x00, /* 49 Vdn*/ 0x00, /* 4A Mut*/ 0x7F, /* 4B k/ */ 0x54,
    /* 4C kEn*/ 0x58, /* 4D    */ 0x00, /* 4E k- */ 0x56, /* 4F F18*/ 0x6D,
    /* 50 F19*/ 0x6E, /* 51 k= */ 0x67, /* 52 k0 */ 0x62, /* 53 k1 */ 0x59,
    /* 54 k2 */ 0x5A, /* 55 k3 */ 0x5B, /* 56 k4 */ 0x5C, /* 57 k5 */ 0x5D,
    /* 58 k6 */ 0x5E, /* 59 k7 */ 0x5F, /* 5A F20*/ 0x6E, /* 5B k8 */ 0x60,
    /* 5C k9 */ 0x61, /* 5D    */ 0x00, /* 5E    */ 0x00, /* 5F    */ 0x00,
    /* 60 F5 */ 0x3E, /* 61 F6 */ 0x3F, /* 62 F7 */ 0x40, /* 63 F3 */ 0x3C,
    /* 64 F8 */ 0x41, /* 65 F9 */ 0x42, /* 66    */ 0x00, /* 67 F11*/ 0x44,
    /* 68    */ 0x00, /* 69 F13*/ 0x68, /* 6A F16*/ 0x6B, /* 6B F14*/ 0x69,
    /* 6C    */ 0x00, /* 6D F10*/ 0x43, /* 6E    */ 0x00, /* 6F F12*/ 0x45,
    /* 70    */ 0x00, /* 71 F15*/ 0x6A, /* 72 Ins*/ 0x49, /* 73 Hom*/ 0x4A,
    /* 74 PgU*/ 0x4B, /* 75 Del*/ 0x4C, /* 76 F4 */ 0x3D, /* 77 End*/ 0x4D,
    /* 78 F2 */ 0x3B, /* 79 PgD*/ 0x4E, /* 7A F1 */ 0x3A, /* 7B Lt */ 0x50,
    /* 7C Rt */ 0x4F, /* 7D Dn */ 0x51, /* 7E Up */ 0x52, /* 7F    */ 0x00,
    };
    if (adb < 128) return t[adb];
    return 0;
}
// Modifier ADB keycode → HID modifier-byte bit mask
static uint8_t adb_mod_mask(CGKeyCode adb)
{
    switch (adb) {
        case 0x38: return 0x02; // L-Shift
        case 0x3C: return 0x20; // R-Shift
        case 0x3B: return 0x01; // L-Ctrl
        case 0x3E: return 0x10; // R-Ctrl
        case 0x3A: return 0x04; // L-Alt/Option
        case 0x3D: return 0x40; // R-Alt/Option
        case 0x37: return 0x08; // L-GUI/Command
        case 0x36: return 0x80; // R-GUI/Command
        default:   return 0;
    }
}

static void hidd_send_kbd(void)
{
    if (!g_kbd_dev) return;
    KbdReport r = {0};
    r.mods = g_mods;
    memcpy(r.keys, g_held, 6);
    IOHIDUserDeviceHandleReportWithTimeStamp(g_kbd_dev, mach_absolute_time(),
        (uint8_t*)&r, sizeof(r));
}

static void hidd_key(CGKeyCode adb, int down)
{
    uint8_t mod = adb_mod_mask(adb);
    if (mod) {
        if (down) g_mods |= mod; else g_mods &= ~mod;
        hidd_send_kbd();
        return;
    }
    uint8_t hid = adb_to_hid(adb);
    // Modifier codes embedded in adb_to_hid (>= 0xE0) shouldn't reach here,
    // but handle them just in case.
    if (!hid || hid >= 0xE0) return;
    if (down) {
        for (int i = 0; i < 6; i++) if (!g_held[i]) { g_held[i] = hid; break; }
    } else {
        for (int i = 0; i < 6; i++) if (g_held[i] == hid) { g_held[i] = 0; break; }
    }
    hidd_send_kbd();
}

// ---- CGEvent fallback init -------------------------------------------------
static void post_init_once(void *ctx)
{
    (void)ctx;
    g_postSem = dispatch_semaphore_create(POST_MAX_INFLIGHT);
    g_postQ   = dispatch_queue_create("meshagent.cgevent", DISPATCH_QUEUE_SERIAL);
    g_source  = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    g_lw_pid  = find_proc_by_name("loginwindow");
    { char b[96]; int l=snprintf(b,sizeof(b),"cg_fallback: source=%p lw_pid=%d\n",(void*)g_source,(int)g_lw_pid); write(STDOUT_FILENO,b,l); }
}

typedef struct { CGEventRef e; } PostCtx;
static void post_worker(void *arg)
{
    PostCtx *c = (PostCtx *)arg;
    // Always inject at the HID event tap — the system-wide path that moves the
    // cursor and reaches the focused UI, in both the logged-in session AND at
    // the login window. (Posting to the loginwindow PID directly never worked.)
    CGEventPost(kCGHIDEventTap, c->e);
    CFRelease(c->e);
    free(c);
    dispatch_semaphore_signal(g_postSem);
}

static void post_cgevent(CGEventRef e)
{
    dispatch_once_f(&g_postOnce, NULL, post_init_once);
    if (dispatch_semaphore_wait(g_postSem, DISPATCH_TIME_NOW) != 0) {
        write(STDOUT_FILENO, "post_cgevent: DROP\n", 19);
        return;
    }
    PostCtx *c = malloc(sizeof(PostCtx));
    if (!c) { dispatch_semaphore_signal(g_postSem); return; }
    CFRetain(e);
    c->e = e;
    dispatch_async_f(g_postQ, c, post_worker);
}

// ---- VNC keyboard fallback implementation ----------------------------------

// Returns 1 if no user is logged in (at loginwindow). Cached for 1 second.
static int is_loginwindow(void)
{
    time_t now = time(NULL);
    if (g_lw_cached >= 0 && (now - g_lw_cache_time) < 1)
        return g_lw_cached;

    // Running as root means we ARE the LoginWindow-session kvmagent (the Aqua
    // one runs as the logged-in user's uid), so by definition we're at the
    // login screen. This is the most reliable signal.
    if (getuid() == 0) { g_lw_cached = 1; g_lw_cache_time = now; return 1; }

    SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("MeshVNC"), NULL, NULL);
    if (!store) return 0;
    CFStringRef user = SCDynamicStoreCopyConsoleUser(store, NULL, NULL);
    CFRelease(store);
    // At the login screen macOS 26 reports the console user as either NULL or
    // the literal string "loginwindow" (NOT a real account), so treat both as
    // being at the login screen.
    int lw = 0;
    if (user == NULL) {
        lw = 1;
    } else {
        if (CFStringCompare(user, CFSTR("loginwindow"), 0) == kCFCompareEqualTo) lw = 1;
        CFRelease(user);
    }
    g_lw_cached = lw;
    g_lw_cache_time = now;
    return lw;
}

// Exact I/O helpers
static int vnc_read_all(int fd, uint8_t *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = (int)read(fd, buf + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int vnc_write_all(int fd, const uint8_t *buf, int n)
{
    int sent = 0;
    while (sent < n) {
        int r = (int)write(fd, buf + sent, n - sent);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

static void vnc_disconnect(void)
{
    if (g_vnc_fd >= 0) { close(g_vnc_fd); g_vnc_fd = -1; }
}

// Read VNC password from file. Applies VNC DES key-bit-reversal quirk.
// Returns 1 on success, 0 if file missing or unreadable.
static int vnc_read_password(uint8_t key[8])
{
    FILE *f = fopen("/usr/local/mesh_services/meshagent/meshagent/kvm/vnc.pw", "r");
    if (!f) return 0;
    char pw[256] = {0};
    if (!fgets(pw, sizeof(pw), f)) { fclose(f); return 0; }
    fclose(f);
    int len = (int)strlen(pw);
    while (len > 0 && (pw[len-1] == '\n' || pw[len-1] == '\r')) pw[--len] = 0;

    memset(key, 0, 8);
    memcpy(key, pw, len < 8 ? len : 8);

    // VNC DES quirk: bit-reverse each byte of the key before use
    for (int i = 0; i < 8; i++) {
        uint8_t b = key[i], r = 0;
        for (int bit = 0; bit < 8; bit++) r |= ((b >> bit) & 1) << (7 - bit);
        key[i] = r;
    }
    return 1;
}

// Connect to localhost:5900, complete RFB handshake (protocol 3.3, VNC Auth or None).
// Returns 1 on success, 0 on failure. Updates g_vnc_fd on success.
static int vnc_connect(void)
{
    vnc_disconnect();
    g_vnc_last_attempt = time(NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(5900);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Non-blocking connect so we can apply a short timeout
    fcntl(fd, F_SETFL, O_NONBLOCK);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    struct pollfd pfd = { fd, POLLOUT, 0 };
    if (poll(&pfd, 1, 500) <= 0 || !(pfd.revents & POLLOUT)) {
        close(fd);
        write(STDOUT_FILENO, "vnc_connect: timeout\n", 21);
        return 0;
    }
    int sockerr = 0; socklen_t elen = sizeof(sockerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &elen);
    if (sockerr != 0) { close(fd); return 0; }
    fcntl(fd, F_SETFL, 0); // restore blocking

    // Server greeting: "RFB xxx.yyy\n"
    uint8_t greeting[12];
    if (vnc_read_all(fd, greeting, 12) < 0) { close(fd); return 0; }

    // Negotiate protocol 3.3 (simplest; server drives security)
    const uint8_t client_ver[] = "RFB 003.003\n";
    if (vnc_write_all(fd, client_ver, 12) < 0) { close(fd); return 0; }

    // Protocol 3.3: server sends 4-byte security type
    uint8_t sec_buf[4];
    if (vnc_read_all(fd, sec_buf, 4) < 0) { close(fd); return 0; }
    uint32_t sec_type = ((uint32_t)sec_buf[0]<<24)|((uint32_t)sec_buf[1]<<16)|
                        ((uint32_t)sec_buf[2]<<8)|(uint32_t)sec_buf[3];

    if (sec_type == 2) { // VNC Authentication: DES challenge-response
        uint8_t challenge[16];
        if (vnc_read_all(fd, challenge, 16) < 0) { close(fd); return 0; }

        uint8_t key[8];
        if (!vnc_read_password(key)) {
            write(STDOUT_FILENO, "vnc_connect: no password file\n", 30);
            close(fd); return 0;
        }

        // Encrypt two 8-byte halves of the challenge with DES-ECB
        uint8_t response[16];
        size_t moved = 0;
        CCCrypt(kCCEncrypt, kCCAlgorithmDES, kCCOptionECBMode,
                key, 8, NULL, challenge,     8, response,     8, &moved);
        CCCrypt(kCCEncrypt, kCCAlgorithmDES, kCCOptionECBMode,
                key, 8, NULL, challenge + 8, 8, response + 8, 8, &moved);

        if (vnc_write_all(fd, response, 16) < 0) { close(fd); return 0; }

        // Auth result: 4 bytes, 0x00000000 = OK
        uint8_t result[4];
        if (vnc_read_all(fd, result, 4) < 0) { close(fd); return 0; }
        if (result[0]|result[1]|result[2]|result[3]) {
            write(STDOUT_FILENO, "vnc_connect: auth failed\n", 25);
            close(fd); return 0;
        }
    } else if (sec_type != 1) { // 1 = None (no auth)
        char b[64]; int l = snprintf(b, sizeof(b), "vnc_connect: unknown sec_type=%u\n", sec_type);
        write(STDOUT_FILENO, b, l);
        close(fd); return 0;
    }

    // ClientInit: shared=1 (allow multiple clients)
    uint8_t cinit = 1;
    if (vnc_write_all(fd, &cinit, 1) < 0) { close(fd); return 0; }

    // ServerInit: width(2)+height(2)+pixel-format(16)+name-len(4) = 24 bytes header
    uint8_t sinit[24];
    if (vnc_read_all(fd, sinit, 24) < 0) { close(fd); return 0; }
    uint32_t name_len = ((uint32_t)sinit[20]<<24)|((uint32_t)sinit[21]<<16)|
                        ((uint32_t)sinit[22]<<8)|(uint32_t)sinit[23];
    if (name_len > 256) name_len = 256;
    if (name_len > 0) {
        uint8_t name[256];
        if (vnc_read_all(fd, name, (int)name_len) < 0) { close(fd); return 0; }
    }

    g_vnc_fd = fd;
    write(STDOUT_FILENO, "vnc_connect: ready\n", 19);
    return 1;
}

// ADB keycode → X11 keysym for RFB KeyEvent messages.
static uint32_t adb_to_x11keysym(CGKeyCode adb)
{
    static const struct { uint8_t adb; uint32_t sym; } t[] = {
        {0x00,0x0061},{0x01,0x0073},{0x02,0x0064},{0x03,0x0066}, // a s d f
        {0x04,0x0068},{0x05,0x0067},{0x06,0x007A},{0x07,0x0078}, // h g z x
        {0x08,0x0063},{0x09,0x0076},{0x0B,0x0062},{0x0C,0x0071}, // c v b q
        {0x0D,0x0077},{0x0E,0x0065},{0x0F,0x0072},{0x10,0x0079}, // w e r y
        {0x11,0x0074},{0x12,0x0031},{0x13,0x0032},{0x14,0x0033}, // t 1 2 3
        {0x15,0x0034},{0x16,0x0036},{0x17,0x0035},{0x18,0x003D}, // 4 6 5 =
        {0x19,0x0039},{0x1A,0x0037},{0x1B,0x002D},{0x1C,0x0038}, // 9 7 - 8
        {0x1D,0x0030},{0x1E,0x005D},{0x1F,0x006F},{0x20,0x0075}, // 0 ] o u
        {0x21,0x005B},{0x22,0x0069},{0x23,0x0070},{0x24,0xFF0D}, // [ i p Return
        {0x25,0x006C},{0x26,0x006A},{0x27,0x0027},{0x28,0x006B}, // l j ' k
        {0x29,0x003B},{0x2A,0x005C},{0x2B,0x002C},{0x2C,0x002F}, // ; \ , /
        {0x2D,0x006E},{0x2E,0x006D},{0x2F,0x002E},{0x30,0xFF09}, // n m . Tab
        {0x31,0x0020},{0x32,0x0060},{0x33,0xFF08},{0x35,0xFF1B}, // Space ` BS Esc
        // Modifiers
        {0x36,0xFFE8},{0x37,0xFFE7},{0x38,0xFFE1},{0x39,0xFFE5}, // R-Meta L-Meta L-Shft CpLk
        {0x3A,0xFFE9},{0x3B,0xFFE3},{0x3C,0xFFE2},{0x3D,0xFFEA}, // L-Alt L-Ctrl R-Shft R-Alt
        {0x3E,0xFFE4},                                            // R-Ctrl
        // Keypad
        {0x41,0xFFAE},{0x43,0xFFAA},{0x45,0xFFAB},{0x47,0xFF7F}, // kp. kp* kp+ NumLk/Clr
        {0x4B,0xFFAF},{0x4C,0xFF8D},{0x4E,0xFFAD},{0x51,0xFFBD}, // kp/ kpEnter kp- kp=
        {0x52,0xFFB0},{0x53,0xFFB1},{0x54,0xFFB2},{0x55,0xFFB3}, // kp0 kp1 kp2 kp3
        {0x56,0xFFB4},{0x57,0xFFB5},{0x58,0xFFB6},{0x59,0xFFB7}, // kp4 kp5 kp6 kp7
        {0x5B,0xFFB8},{0x5C,0xFFB9},                             // kp8 kp9
        // Function keys (kVK_F1=0x7A … kVK_F12=0x6F, F13=0x69, etc.)
        {0x7A,0xFFBE},{0x78,0xFFBF},{0x63,0xFFC0},{0x76,0xFFC1}, // F1 F2 F3 F4
        {0x60,0xFFC2},{0x61,0xFFC3},{0x62,0xFFC4},{0x64,0xFFC5}, // F5 F6 F7 F8
        {0x65,0xFFC6},{0x6D,0xFFC7},{0x67,0xFFC8},{0x6F,0xFFC9}, // F9 F10 F11 F12
        {0x69,0xFFCA},{0x6B,0xFFCB},{0x71,0xFFCC},{0x6A,0xFFCD}, // F13 F14 F15 F16
        {0x40,0xFFCE},{0x4F,0xFFCF},{0x50,0xFFD0},{0x5A,0xFFD1}, // F17 F18 F19 F20
        // Navigation
        {0x73,0xFF50},{0x74,0xFF55},{0x75,0xFFFF},{0x77,0xFF57}, // Home PgUp FwdDel End
        {0x79,0xFF56},{0x7B,0xFF51},{0x7C,0xFF53},{0x7D,0xFF54}, // PgDn Left Right Down
        {0x7E,0xFF52},                                            // Up
    };
    for (int i = 0; i < (int)(sizeof(t)/sizeof(t[0])); i++)
        if (t[i].adb == (uint8_t)adb) return t[i].sym;
    return 0;
}

// US-layout shifted keysym for physical digit/punctuation keys (e.g. '1' -> '!').
// screensharingd does not reliably combine a separately-sent Shift keysym with
// an unshifted character keysym, so the VNC path resolves the shifted character
// itself and sends that keysym directly instead.
static uint32_t adb_to_shifted_keysym(uint8_t adb)
{
    switch (adb) {
        case 0x12: return 0x0021; // 1 -> !
        case 0x13: return 0x0040; // 2 -> @
        case 0x14: return 0x0023; // 3 -> #
        case 0x15: return 0x0024; // 4 -> $
        case 0x17: return 0x0025; // 5 -> %
        case 0x16: return 0x005E; // 6 -> ^
        case 0x1A: return 0x0026; // 7 -> &
        case 0x1C: return 0x002A; // 8 -> *
        case 0x19: return 0x0028; // 9 -> (
        case 0x1D: return 0x0029; // 0 -> )
        case 0x1B: return 0x005F; // - -> _
        case 0x18: return 0x002B; // = -> +
        case 0x21: return 0x007B; // [ -> {
        case 0x1E: return 0x007D; // ] -> }
        case 0x2A: return 0x007C; // \ -> |
        case 0x29: return 0x003A; // ; -> :
        case 0x27: return 0x0022; // ' -> "
        case 0x32: return 0x007E; // ` -> ~
        case 0x2B: return 0x003C; // , -> <
        case 0x2F: return 0x003E; // . -> >
        case 0x2C: return 0x003F; // / -> ?
        default:   return 0;
    }
}

// Send an RFB KeyEvent to screensharingd/localhost:5900.
// Reconnects automatically if the connection dropped (with 5s backoff).
static void vnc_inject_key(CGKeyCode adb, int down)
{
    // Reconnect if needed, with 5s backoff between attempts
    if (g_vnc_fd < 0) {
        time_t now = time(NULL);
        if ((now - g_vnc_last_attempt) < 5) return;
        if (!vnc_connect()) return;
    }

    // L-Shift (0x38) / R-Shift (0x3C): track state, send their own keysym as before.
    if (adb == 0x38 || adb == 0x3C) { g_vnc_shift_down = down; }

    uint32_t sym = 0;
    if (g_vnc_shift_down && adb != 0x38 && adb != 0x3C) {
        sym = adb_to_shifted_keysym((uint8_t)adb);
        if (!sym) {
            uint32_t base = adb_to_x11keysym(adb);
            if (base >= 0x61 && base <= 0x7A) sym = base - 0x20; // a-z -> A-Z
        }
    }
    if (!sym) sym = adb_to_x11keysym(adb);
    if (!sym) {
        char b[64]; int l = snprintf(b, sizeof(b), "vnc_key: adb=0x%x no keysym\n", (unsigned)adb);
        write(STDOUT_FILENO, b, l);
        return;
    }

    // RFB message type 4 (KeyEvent): [type, down_flag, pad, pad, keysym_u32_be]
    uint8_t msg[8];
    msg[0] = 4;
    msg[1] = down ? 1 : 0;
    msg[2] = 0; msg[3] = 0;
    msg[4] = (sym >> 24) & 0xFF;
    msg[5] = (sym >> 16) & 0xFF;
    msg[6] = (sym >>  8) & 0xFF;
    msg[7] = (sym      ) & 0xFF;

    if (vnc_write_all(g_vnc_fd, msg, 8) < 0) {
        write(STDOUT_FILENO, "vnc_key: send failed, disconnecting\n", 36);
        vnc_disconnect();
    }
}

// Current RFB button mask (bit0=left, bit1=middle, bit2=right, bit3=wheelUp,
// bit4=wheelDown). RFB PointerEvents carry a mask, not up/down transitions.
static uint8_t g_vnc_btnmask = 0;

// Send an RFB PointerEvent to screensharingd/localhost:5900 for login-screen
// mouse (movement, buttons, wheel). Coordinates are absolute framebuffer pixels.
static void vnc_inject_mouse(double x, double y, int button, short wheel)
{
    if (g_vnc_fd < 0) {
        time_t now = time(NULL);
        if ((now - g_vnc_last_attempt) < 5) return;
        if (!vnc_connect()) return;
    }

    switch (button) {
        case MOUSEEVENTF_LEFTDOWN:  g_vnc_btnmask |=  0x01; break;
        case MOUSEEVENTF_LEFTUP:    g_vnc_btnmask &= ~0x01; break;
        case MOUSEEVENTF_RIGHTDOWN: g_vnc_btnmask |=  0x04; break;
        case MOUSEEVENTF_RIGHTUP:   g_vnc_btnmask &= ~0x04; break;
        default: break;
    }

    uint16_t xi = (x < 0) ? 0 : (uint16_t)x;
    uint16_t yi = (y < 0) ? 0 : (uint16_t)y;

    if (wheel != 0) {
        // Wheel = momentary press+release of button 4 (up) or 5 (down).
        uint8_t wbit = (wheel > 0) ? 0x08 : 0x10;
        uint8_t down[6] = { 5, (uint8_t)(g_vnc_btnmask | wbit),
                            (uint8_t)(xi>>8), (uint8_t)(xi&0xFF),
                            (uint8_t)(yi>>8), (uint8_t)(yi&0xFF) };
        uint8_t up[6]   = { 5, g_vnc_btnmask,
                            (uint8_t)(xi>>8), (uint8_t)(xi&0xFF),
                            (uint8_t)(yi>>8), (uint8_t)(yi&0xFF) };
        if (vnc_write_all(g_vnc_fd, down, 6) < 0 || vnc_write_all(g_vnc_fd, up, 6) < 0)
            vnc_disconnect();
        return;
    }

    uint8_t msg[6] = { 5, g_vnc_btnmask,
                       (uint8_t)(xi>>8), (uint8_t)(xi&0xFF),
                       (uint8_t)(yi>>8), (uint8_t)(yi&0xFF) };
    if (vnc_write_all(g_vnc_fd, msg, 6) < 0) {
        write(STDOUT_FILENO, "vnc_mouse: send failed, disconnecting\n", 38);
        vnc_disconnect();
    }
}

// ---- Init ------------------------------------------------------------------
void hid_inject_init(void)
{
    // ---- Diagnostic: code signing status and entitlement blob ----
    {
        uint32_t cs_flags = 0;
        if (csops(getpid(), CS_OPS_STATUS, &cs_flags, sizeof(cs_flags)) == 0) {
            char b[128]; int l = snprintf(b, sizeof(b),
                "cs_status: flags=0x%x (valid=%d adhoc=%d plat=%d ent_ok=%d)\n",
                cs_flags,
                (cs_flags & CS_VALID)                  ? 1 : 0,
                (cs_flags & CS_ADHOC)                  ? 1 : 0,
                (cs_flags & CS_PLATFORM_BINARY)        ? 1 : 0,
                (cs_flags & CS_ENTITLEMENTS_VALIDATED) ? 1 : 0);
            write(STDOUT_FILENO, b, l);
        } else {
            char b[64]; int l = snprintf(b, sizeof(b),
                "cs_status: csops errno=%d\n", errno);
            write(STDOUT_FILENO, b, l);
        }

        char ent_buf[4096] = {0};
        int rc = csops(getpid(), CS_OPS_ENTITLEMENTS_BLOB, ent_buf, sizeof(ent_buf));
        if (rc == 0) {
            uint32_t magic = ntohl(*(uint32_t*)(ent_buf + 0));
            uint32_t size  = ntohl(*(uint32_t*)(ent_buf + 4));
            char b[128]; int l = snprintf(b, sizeof(b),
                "cs_ent_blob: magic=0x%x size=%u\n", magic, size);
            write(STDOUT_FILENO, b, l);
            if (size > 8 && size < sizeof(ent_buf)) {
                // Print the embedded plist (null-terminate it first)
                ent_buf[size] = '\0';
                write(STDOUT_FILENO, ent_buf + 8, size - 8);
                write(STDOUT_FILENO, "\n", 1);
            }
        } else {
            char b[64]; int l = snprintf(b, sizeof(b),
                "cs_ent_blob: csops failed rc=%d errno=%d\n", rc, errno);
            write(STDOUT_FILENO, b, l);
        }
    }

    // ---- Priority 1: IOHIDUserDevice (kernel-level virtual HID device) ----
    // Creates events at the IOHIDFamily layer, below SecureEventInput — works at loginwindow.
    // Requires com.apple.hid.manager.user-device entitlement (checked by IOHIDFamily kext).
    {
        CFDataRef kbd_desc = CFDataCreate(kCFAllocatorDefault, kKbdDescriptor, sizeof(kKbdDescriptor));
        CFStringRef keys[]   = { CFSTR(kIOHIDReportDescriptorKey), CFSTR(kIOHIDVendorIDKey),
                                  CFSTR(kIOHIDProductIDKey),        CFSTR(kIOHIDTransportKey) };
        CFNumberRef vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &(int){0x05ac});
        CFNumberRef pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &(int){0x8601});
        CFTypeRef   vals[] = { kbd_desc, vid, pid, CFSTR("USB") };
        CFDictionaryRef props = CFDictionaryCreate(kCFAllocatorDefault,
            (const void**)keys, (const void**)vals, 4,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        g_kbd_dev = make_hid_device(props);
        CFRelease(props); CFRelease(kbd_desc); CFRelease(vid); CFRelease(pid);
        { char b[64]; int l=snprintf(b,sizeof(b),"hid_inject_init: kbd_dev=%p\n",(void*)g_kbd_dev); write(STDOUT_FILENO,b,l); }
    }
    {
        CFDataRef m_desc = CFDataCreate(kCFAllocatorDefault, kMouseDescriptor, sizeof(kMouseDescriptor));
        CFStringRef keys[]  = { CFSTR(kIOHIDReportDescriptorKey), CFSTR(kIOHIDVendorIDKey),
                                 CFSTR(kIOHIDProductIDKey),        CFSTR(kIOHIDTransportKey) };
        CFNumberRef vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &(int){0x05ac});
        CFNumberRef pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &(int){0x8602});
        CFTypeRef   vals[] = { m_desc, vid, pid, CFSTR("USB") };
        CFDictionaryRef props = CFDictionaryCreate(kCFAllocatorDefault,
            (const void**)keys, (const void**)vals, 4,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        g_mouse_dev = make_hid_device(props);
        CFRelease(props); CFRelease(m_desc); CFRelease(vid); CFRelease(pid);
        { char b[64]; int l=snprintf(b,sizeof(b),"hid_inject_init: mouse_dev=%p\n",(void*)g_mouse_dev); write(STDOUT_FILENO,b,l); }
    }
    if (g_kbd_dev && g_mouse_dev) return;  // IOHIDUserDevice succeeded — best path

    // Priority 2 / 2b (AppleVirtualPlatform HID bridge) are intentionally DISABLED.
    // They are never the working injection path on our targets: login-window input
    // goes through screensharingd VNC and in-session input through CGEventPost, and
    // on a Parallels/Apple VM guest the bridge routes events to the host rather than
    // the guest login window. More importantly, on macOS 11 (Big Sur)
    // steal_avp_kbd_from_bridge()/open_avp_hid_interface() crash the process (IOKit)
    // — like the IOHIDUserDevice path above — killing the agent right after kvm_init,
    // before the capture loop, so the remote screen stays black. Skipping them leaves
    // g_avp_kbd_conn/g_kbd_dev NULL, which is exactly what the injection code already
    // expects when it falls through to CGEventPost (KeyAction) or vnc_inject (login).
    (void)&steal_avp_kbd_from_bridge; (void)&open_avp_hid_interface;

    // Fall back to the CGEvent path (in-session). Login window uses vnc_inject.
    dispatch_once_f(&g_postOnce, NULL, post_init_once);
    write(STDOUT_FILENO, "hid_inject_init: using CGEvent path\n", 35);
}

static const int g_keymapLen = 114;
static int g_capsLock = 0;

static struct keymap_t g_keymap[] = {
	{ kVK_Space,		 VK_SPACE },
	{ kVK_CapsLock,		 VK_CAPITAL },
	{ kVK_ANSI_Q,        VK_Q },
	{ kVK_ANSI_W,        VK_W },
	{ kVK_ANSI_E,        VK_E },
	{ kVK_ANSI_R,        VK_R },
	{ kVK_ANSI_T,        VK_T },
	{ kVK_ANSI_Y,        VK_Y },
	{ kVK_ANSI_U,        VK_U },
	{ kVK_ANSI_I,        VK_I },
	{ kVK_ANSI_O,        VK_O },
	{ kVK_ANSI_P,        VK_P },
	{ kVK_ANSI_A,        VK_A },
	{ kVK_ANSI_S,        VK_S },
	{ kVK_ANSI_D,        VK_D },
	{ kVK_ANSI_F,        VK_F },
	{ kVK_ANSI_G,        VK_G },
	{ kVK_ANSI_H,        VK_H },
	{ kVK_ANSI_J,        VK_J },
	{ kVK_ANSI_K,        VK_K },
	{ kVK_ANSI_L,        VK_L },
	{ kVK_ANSI_Z,        VK_Z },
	{ kVK_ANSI_X,        VK_X },
	{ kVK_ANSI_C,        VK_C },
	{ kVK_ANSI_V,        VK_V },
	{ kVK_ANSI_B,        VK_B },
	{ kVK_ANSI_N,        VK_N },
	{ kVK_ANSI_M,        VK_M },
	{ kVK_ANSI_1,        VK_1 },
	{ kVK_ANSI_2,        VK_2 },
	{ kVK_ANSI_3,        VK_3 },
	{ kVK_ANSI_4,        VK_4 },
	{ kVK_ANSI_5,        VK_5 },
	{ kVK_ANSI_6,        VK_6 },
	{ kVK_ANSI_7,        VK_7 },
	{ kVK_ANSI_8,        VK_8 },
	{ kVK_ANSI_9,        VK_9 },
	{ kVK_ANSI_0,        VK_0 },
	{ kVK_Delete,        VK_BACK },
	{ kVK_Tab,           VK_TAB },
	{ kVK_ANSI_KeypadClear,            VK_CLEAR },
	{ kVK_Return,           VK_RETURN },
	{ kVK_Help,            VK_PAUSE },
	{ kVK_Escape,           VK_ESCAPE },
	{ kVK_ForwardDelete,           VK_DELETE },
	{ kVK_Home,             VK_HOME },
	{ kVK_LeftArrow,             VK_LEFT },
	{ kVK_UpArrow,               VK_UP },
	{ kVK_RightArrow,            VK_RIGHT },
	{ kVK_DownArrow,             VK_DOWN },
	{ kVK_PageUp,          VK_PRIOR },
	{ kVK_PageDown,        VK_NEXT },
	{ kVK_End,              VK_END },
	{ kVK_Help,           VK_SELECT },
	{ kVK_Help,            VK_SNAPSHOT },
	{ kVK_Help,          VK_EXECUTE },
	{ kVK_Help,           VK_INSERT },
	{ kVK_Help,             VK_HELP },
	{ kVK_Escape,            VK_CANCEL },
	{ kVK_F1,               VK_F1 },
	{ kVK_F2,               VK_F2 },
	{ kVK_F3,               VK_F3 },
	{ kVK_F4,               VK_F4 },
	{ kVK_F5,               VK_F5 },
	{ kVK_F6,               VK_F6 },
	{ kVK_F7,               VK_F7 },
	{ kVK_F8,               VK_F8 },
	{ kVK_F9,               VK_F9 },
	{ kVK_F10,              VK_F10 },
	{ kVK_F11,              VK_F11 },
	{ kVK_F12,              VK_F12 },
	{ kVK_F13,              VK_F13 },
	{ kVK_F14,              VK_F14 },
	{ kVK_F15,              VK_F15 },
	{ kVK_F16,              VK_F16 },
	{ kVK_F17,              VK_F17 },
	{ kVK_F18,              VK_F18 },
	{ kVK_F19,              VK_F19 },
	{ kVK_F20,              VK_F20 },
	{ kVK_Home,          VK_HOME },
	{ kVK_ANSI_KeypadMultiply,      VK_MULTIPLY },
	{ kVK_ANSI_Equal,           VK_ADD },
	{ kVK_ANSI_Comma,     VK_SEPARATOR },
	{ kVK_ANSI_Minus,      VK_SUBTRACT },
	{ kVK_ANSI_KeypadDecimal,       VK_DECIMAL },
	{ kVK_ANSI_KeypadDivide,        VK_DIVIDE },
	{ kVK_ANSI_Keypad0,             VK_NUMPAD0 },
	{ kVK_ANSI_Keypad1,             VK_NUMPAD1 },
	{ kVK_ANSI_Keypad2,             VK_NUMPAD2 },
	{ kVK_ANSI_Keypad3,             VK_NUMPAD3 },
	{ kVK_ANSI_Keypad4,             VK_NUMPAD4 },
	{ kVK_ANSI_Keypad5,             VK_NUMPAD5 },
	{ kVK_ANSI_Keypad6,             VK_NUMPAD6 },
	{ kVK_ANSI_Keypad7,             VK_NUMPAD7 },
	{ kVK_ANSI_Keypad8,             VK_NUMPAD8 },
	{ kVK_ANSI_Keypad9,             VK_NUMPAD9 },
	{ kVK_Shift,          VK_SHIFT },
	{ kVK_Control,        VK_CONTROL },
	{ kVK_Option,            VK_MENU },
	{ kVK_Command,          VK_RWIN },
	{ kVK_Command,          VK_LWIN },
	{ kVK_Option,             VK_APPS },
	{ kVK_JIS_Kana,       VK_KANA },
	{ kVK_ANSI_Semicolon,			   VK_OEM_1 },
	{ kVK_ANSI_Equal,		 	   VK_OEM_PLUS },
	{ kVK_ANSI_Comma,			   VK_OEM_COMMA },
	{ kVK_ANSI_Minus,		 	   VK_OEM_MINUS },
	{ kVK_ANSI_Period, 		   VK_OEM_PERIOD },
	{ kVK_ANSI_Slash, 	       VK_OEM_2  },
	{ kVK_ANSI_Grave, 		   VK_OEM_3 },
	{ kVK_ANSI_LeftBracket, 	   VK_OEM_4 },
	{ kVK_ANSI_Backslash,		   VK_OEM_5 },
	{ kVK_ANSI_RightBracket,	   VK_OEM_6 },
	{ kVK_ANSI_Quote,	   VK_OEM_7 }
};
extern int KVM_SEND(char *buffer, int bufferLen);

void kvm_server_sendmsg(char *msg)
{
	int msgLen = strnlen_s(msg, 255);
	char buffer[512];
	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_ERROR);
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)(msgLen + 4));
	memcpy_s(buffer + 4, 512 - 4, msg, msgLen);
	KVM_SEND(buffer, msgLen + 4);
}

char* getCurrentSession() {
	SCDynamicStoreRef store;
	CFStringRef name;
	uid_t uid;
	char *buf;
	Boolean ok;
	buf = (char *)malloc(BUFSIZ);
	store = SCDynamicStoreCreate(NULL, CFSTR("GetConsoleUser"), NULL, NULL);
	assert(store != NULL);
	name = SCDynamicStoreCopyConsoleUser(store, &uid, NULL);
	CFRelease(store);
	if (name != NULL) {
		ok = CFStringGetCString(name, buf, BUFSIZ, kCFStringEncodingUTF8);
		assert(ok == true);
		CFRelease(name);
	} else {
		strcpy(buf, "<none>");
	}
	return buf;
}

// Last known absolute mouse position (for computing relative deltas for HIDUserDevice)
static double g_lastX = 0, g_lastY = 0;

void MouseAction(double absX, double absY, int button, short wheel)
{
    // At the login window, route the mouse through the same screensharingd VNC
    // path as the keyboard (localhost:5900) so both share one proven injection
    // channel. Coordinates are absolute framebuffer pixels. Proven 2026-07-08.
    if (is_loginwindow()) {
        vnc_inject_mouse(absX, absY, button, wheel);
        return;
    }

    // Mouse is NOT covered by SecureEventInput, so CGEventPost(kCGHIDEventTap)
    // should move the cursor and click at the login window. Fall through to the
    // standard CGEvent path below (no special-casing needed for the login screen).
    if (g_mouse_dev) {
        MouseReport r = {0};
        if (button == MOUSEEVENTF_LEFTDOWN)  r.buttons |= 0x01;
        if (button == MOUSEEVENTF_RIGHTDOWN) r.buttons |= 0x02;
        // For move events keep previous button state — not tracked here, just send 0
        int dx = (int)(absX - g_lastX);
        int dy = (int)(absY - g_lastY);
        g_lastX = absX; g_lastY = absY;
        if (dx < -127) dx = -127; if (dx > 127) dx = 127;
        if (dy < -127) dy = -127; if (dy > 127) dy = 127;
        r.dx = (int8_t)dx; r.dy = (int8_t)dy;
        IOHIDUserDeviceHandleReportWithTimeStamp(g_mouse_dev, mach_absolute_time(),
            (uint8_t*)&r, sizeof(r));
        return;
    }

	dispatch_once_f(&g_postOnce, NULL, post_init_once);
	CGEventRef e = NULL;
	if (wheel != 0) {
		e = CGEventCreateScrollWheelEvent(g_source, kCGScrollEventUnitLine, 1, (int32_t)wheel);
		if (e) { post_cgevent(e); CFRelease(e); }
		return;
	}
	if (button == 0) {
		e = CGEventCreateMouseEvent(g_source, kCGEventMouseMoved,
		    CGPointMake(absX, absY), kCGMouseButtonLeft);
	} else {
		CGEventType etype;
		switch (button) {
			case MOUSEEVENTF_LEFTDOWN:  etype = kCGEventLeftMouseDown;  break;
			case MOUSEEVENTF_LEFTUP:    etype = kCGEventLeftMouseUp;    break;
			case MOUSEEVENTF_RIGHTDOWN: etype = kCGEventRightMouseDown; break;
			case MOUSEEVENTF_RIGHTUP:   etype = kCGEventRightMouseUp;   break;
			default: return;
		}
		e = CGEventCreateMouseEvent(g_source, etype,
		    CGPointMake(absX, absY), kCGMouseButtonLeft);
	}
	if (e) { post_cgevent(e); CFRelease(e); }
}

extern int set_kbd_state(int state);
extern int get_kbd_state();
extern ILibQueue g_messageQ;

static void inject_key(CGKeyCode keycode, int down)
{
    // At the login window, try the standard HID event tap first. SecureEventInput
    // may drop keyboard events when the password field is focused, but mouse and
    // non-secure typing go through. (Legacy VNC is deprecated on Tahoe and does
    // nothing, so vnc_inject_key is no longer used here.)
    if (is_loginwindow()) {
        // SecureEventInput drops CGEventPost keyboard events when the login-window
        // password field is focused. Route through screensharingd's VNC server on
        // localhost:5900 instead: it holds com.apple.private.hid.client.event-dispatch
        // and injects past SecureEventInput. Proven 2026-07-08 — legacy type-2 VNC
        // gives working login-window keyboard on Tahoe.
        vnc_inject_key(keycode, down);
        return;
    }
    if (g_avp_kbd_conn != IO_OBJECT_NULL) { if (avp_kbd_key(keycode, down)) return; }
    if (g_kbd_dev) { hidd_key(keycode, down); return; }
    dispatch_once_f(&g_postOnce, NULL, post_init_once);
    CGEventRef e = CGEventCreateKeyboardEvent(g_source, keycode, down ? true : false);
    if (e) { post_cgevent(e); CFRelease(e); }
}

void KeyAction(unsigned char vk, int up)
{
	if (up == 4) { up = 0; }

	if (up && (vk == 0x14 || vk == 0x90 || vk == 0x91))
	{
		int state = get_kbd_state();
		switch (vk)
		{
		case 0x14: state = set_kbd_state(state ^ 4); break;
		case 0x90: state = set_kbd_state(state ^ 1); break;
		case 0x91: state = set_kbd_state(state ^ 2); break;
		}
		unsigned char *buffer = ILibMemory_SmartAllocate(5);
		((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_KEYSTATE);
		((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)5);
		buffer[4] = (unsigned char)get_kbd_state();
		ILibQueue_Lock(g_messageQ);
		ILibQueue_EnQueue(g_messageQ, buffer);
		ILibQueue_UnLock(g_messageQ);
		return;
	}

	int i;
	CGKeyCode keycode = 0;
	for (i = 0; i < g_keymapLen; i++) {
		if (g_keymap[i].vk == vk) { keycode = g_keymap[i].keycode; break; }
	}
	if (i == g_keymapLen) {
		char _b[64]; int _l = snprintf(_b, sizeof(_b), "KeyAction: vk=0x%02x not found\n", vk);
		write(STDOUT_FILENO, _b, _l);
		return;
	}
	if (vk == VK_CAPITAL && up) { g_capsLock = g_capsLock ? 0 : 1; }

	{ char _b[64]; int _l = snprintf(_b, sizeof(_b), "KeyAction: vk=0x%02x kc=%d up=%d\n", vk, (int)keycode, up); write(STDOUT_FILENO, _b, _l); }
	inject_key(keycode, !up);
}

// Map a Unicode codepoint to (VK, need_shift) for a US keyboard layout.
typedef struct { uint16_t unicode; unsigned char vk; int shift; } UniVKEntry;
static unsigned char unicode_to_vk(uint16_t u, int *out_shift)
{
    static const UniVKEntry t[] = {
        {'0',VK_0,0},{'1',VK_1,0},{'2',VK_2,0},{'3',VK_3,0},{'4',VK_4,0},
        {'5',VK_5,0},{'6',VK_6,0},{'7',VK_7,0},{'8',VK_8,0},{'9',VK_9,0},
        {'a',VK_A,0},{'b',VK_B,0},{'c',VK_C,0},{'d',VK_D,0},{'e',VK_E,0},
        {'f',VK_F,0},{'g',VK_G,0},{'h',VK_H,0},{'i',VK_I,0},{'j',VK_J,0},
        {'k',VK_K,0},{'l',VK_L,0},{'m',VK_M,0},{'n',VK_N,0},{'o',VK_O,0},
        {'p',VK_P,0},{'q',VK_Q,0},{'r',VK_R,0},{'s',VK_S,0},{'t',VK_T,0},
        {'u',VK_U,0},{'v',VK_V,0},{'w',VK_W,0},{'x',VK_X,0},{'y',VK_Y,0},
        {'z',VK_Z,0},
        {'A',VK_A,1},{'B',VK_B,1},{'C',VK_C,1},{'D',VK_D,1},{'E',VK_E,1},
        {'F',VK_F,1},{'G',VK_G,1},{'H',VK_H,1},{'I',VK_I,1},{'J',VK_J,1},
        {'K',VK_K,1},{'L',VK_L,1},{'M',VK_M,1},{'N',VK_N,1},{'O',VK_O,1},
        {'P',VK_P,1},{'Q',VK_Q,1},{'R',VK_R,1},{'S',VK_S,1},{'T',VK_T,1},
        {'U',VK_U,1},{'V',VK_V,1},{'W',VK_W,1},{'X',VK_X,1},{'Y',VK_Y,1},
        {'Z',VK_Z,1},
        {' ',VK_SPACE,0},{'\t',VK_TAB,0},{'\r',VK_RETURN,0},{'\n',VK_RETURN,0},
        {'-',VK_OEM_MINUS,0},{'=',VK_OEM_PLUS,0},
        {'[',VK_OEM_4,0},{']',VK_OEM_6,0},{'\\',VK_OEM_5,0},
        {';',VK_OEM_1,0},{'\'',VK_OEM_7,0},{'`',VK_OEM_3,0},
        {',',VK_OEM_COMMA,0},{'.',VK_OEM_PERIOD,0},{'/',VK_OEM_2,0},
        {'!',VK_1,1},{'@',VK_2,1},{'#',VK_3,1},{'$',VK_4,1},{'%',VK_5,1},
        {'^',VK_6,1},{'&',VK_7,1},{'*',VK_8,1},{'(',VK_9,1},{')',VK_0,1},
        {'_',VK_OEM_MINUS,1},{'+',VK_OEM_PLUS,1},
        {'{',VK_OEM_4,1},{'}',VK_OEM_6,1},{'|',VK_OEM_5,1},
        {':',VK_OEM_1,1},{'"',VK_OEM_7,1},{'~',VK_OEM_3,1},
        {'<',VK_OEM_COMMA,1},{'>',VK_OEM_PERIOD,1},{'?',VK_OEM_2,1},
    };
    for (int i = 0; i < (int)(sizeof(t)/sizeof(t[0])); i++) {
        if (t[i].unicode == u) { *out_shift = t[i].shift; return t[i].vk; }
    }
    return 0;
}

void KeyActionUnicode(uint16_t unicode, int up)
{
    int need_shift = 0;
    unsigned char vk = unicode_to_vk(unicode, &need_shift);
    if (!vk) {
        char _b[64]; int _l = snprintf(_b, sizeof(_b), "KeyActionUnicode: u=0x%04x up=%d unmapped\n", unicode, up);
        write(STDOUT_FILENO, _b, _l);
        return;
    }
    { char _b[64]; int _l = snprintf(_b, sizeof(_b), "KeyActionUnicode: u=0x%04x vk=0x%02x shift=%d up=%d\n", unicode, vk, need_shift, up); write(STDOUT_FILENO, _b, _l); }

    // For CGEvent, set the unicode on the key event directly using
    // CGEventKeyboardSetUnicodeString — this lets the system handle
    // the character correctly regardless of keyboard layout.
    int i;
    CGKeyCode keycode = 0;
    for (i = 0; i < g_keymapLen; i++) {
        if (g_keymap[i].vk == vk) { keycode = g_keymap[i].keycode; break; }
    }
    if (i == g_keymapLen) return;

    // At loginwindow, VNC injection is handled exclusively by inject_key() via KeyAction.
    // Suppress Unicode injection here to avoid double characters (MeshCentral sends both).
    // Must check before AVP: AVP steal succeeds at loginwindow but sendReport fails.
    if (is_loginwindow()) return;
    if (g_avp_kbd_conn != IO_OBJECT_NULL) { if (avp_kbd_key(keycode, !up)) return; }
    if (g_kbd_dev) { hidd_key(keycode, !up); return; }
    dispatch_once_f(&g_postOnce, NULL, post_init_once);
    CGEventRef e = CGEventCreateKeyboardEvent(g_source, keycode, up ? false : true);
    if (!e) return;
    // Override the character produced by this key event with the Unicode value
    UniChar uc = (UniChar)unicode;
    CGEventKeyboardSetUnicodeString(e, 1, &uc);
    post_cgevent(e);
    CFRelease(e);
}
