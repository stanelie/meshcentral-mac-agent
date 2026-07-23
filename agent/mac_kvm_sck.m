/*
 * ScreenCaptureKit wrapper for MeshAgent KVM.
 *
 * Provides kvm_capture_sck(displayID) → CGImageRef (caller must CGImageRelease)
 * using SCScreenshotManager (macOS 14+, Sonoma/Tahoe).  Returns NULL on failure.
 *
 * Compiled as Objective-C because SCK has no C API.  Linked separately to
 * avoid mixing -std=gnu99 with ObjC in the same translation unit.
 */

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <Foundation/Foundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>
#include <stdarg.h>
#include <stdio.h>

static void sck_flog(const char *fmt, ...) {
    FILE *f = fopen("/tmp/kvm_debug.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fflush(f); fclose(f);
}

API_AVAILABLE(macos(14.0))
static CGImageRef _kvm_sck_capture_impl(uint32_t displayID)
{
    __block CGImageRef result = NULL;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                           onScreenWindowsOnly:NO
                                               completionHandler:^(SCShareableContent *content, NSError *err) {
        if (err || !content) {
            sck_flog("SCShareableContent failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "nil content");
            dispatch_semaphore_signal(sem);
            return;
        }

        sck_flog("SCShareableContent: %lu displays %lu windows\n",
            (unsigned long)content.displays.count,
            (unsigned long)content.windows.count);

        SCDisplay *target = nil;
        for (SCDisplay *d in content.displays) {
            sck_flog("  SCDisplay id=%u frame=%.0fx%.0f\n",
                d.displayID, d.frame.size.width, d.frame.size.height);
            if (d.displayID == displayID) { target = d; break; }
        }
        if (!target && content.displays.count > 0)
            target = content.displays.firstObject;

        if (!target) {
            sck_flog("SCShareableContent: no display\n");
            dispatch_semaphore_signal(sem);
            return;
        }

        SCContentFilter *filter =
            [[SCContentFilter alloc] initWithDisplay:target excludingWindows:@[]];

        SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
        cfg.width  = (size_t)target.frame.size.width;
        cfg.height = (size_t)target.frame.size.height;
        cfg.pixelFormat = kCVPixelFormatType_32BGRA;
        cfg.showsCursor = YES;   // include the cursor so the remote sees mouse movement

        [SCScreenshotManager captureImageWithFilter:filter
                                      configuration:cfg
                                  completionHandler:^(CGImageRef img, NSError *e2) {
            if (e2 || !img) {
                sck_flog("SCScreenshotManager failed: %s\n",
                    e2 ? [[e2 localizedDescription] UTF8String] : "nil image");
            } else {
                CGImageRetain(img);
                result = img;
                sck_flog("SCScreenshotManager: captured %zux%zu\n",
                    CGImageGetWidth(img), CGImageGetHeight(img));
            }
            dispatch_semaphore_signal(sem);
        }];
    }];

    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, 3LL * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(sem, deadline) != 0)
        sck_flog("SCK: timed out waiting for capture\n");

    return result;
}

/* C-callable entry point declared in mac_kvm.c */
CGImageRef kvm_capture_sck(uint32_t displayID)
{
    if (@available(macOS 14.0, *)) {
        return _kvm_sck_capture_impl(displayID);
    }
    sck_flog("kvm_capture_sck: needs macOS 14+\n");
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Native clipboard for the in-session (Aqua) kvmagent.
 *
 * The root daemon cannot read/write the user pasteboard cheaply: pbpaste/pbcopy
 * (or any spawn) cost ~0.8s on slower hardware because the process must reach
 * pasteboardd in the user's GUI Mach-bootstrap namespace. The kvmagent already
 * runs inside that session, so NSPasteboard here is a few ms with no spawn.
 *
 * We expose it over a tiny dedicated unix socket (/tmp/meshagent-clip.sock),
 * served by an isolated detached pthread so the KVM relay path is untouched.
 * The daemon's clipboard JS connects to this socket instead of spawning shells.
 *
 * Wire protocol (one op per connection):
 *   Read:        client sends 'R'
 *                server replies [u32 changeCount][u32 dataLen][dataLen UTF8 bytes]
 *   ChangeCount: client sends 'C'
 *                server replies [u32 changeCount]
 *   Write:       client sends 'W'[u32 dataLen][dataLen UTF8 bytes]
 *                server replies 'K'
 * All integers big-endian.
 * ------------------------------------------------------------------------- */

#import <AppKit/AppKit.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#define DWCLIP_SOCK_PATH "/tmp/meshagent-clip.sock"

static long dwclip_changecount(void)
{
    @autoreleasepool {
        return (long)[[NSPasteboard generalPasteboard] changeCount];
    }
}

// Returns malloc'd UTF8 buffer (caller frees) and sets *outLen; NULL if empty.
static char* dwclip_read(size_t *outLen)
{
    @autoreleasepool {
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (s == nil) { *outLen = 0; return NULL; }
        const char *utf8 = [s UTF8String];
        if (utf8 == NULL) { *outLen = 0; return NULL; }
        size_t n = strlen(utf8);
        char *buf = (char*)malloc(n > 0 ? n : 1);
        if (buf == NULL) { *outLen = 0; return NULL; }
        memcpy(buf, utf8, n);
        *outLen = n;
        return buf;
    }
}

static void dwclip_write(const char *utf8, size_t len)
{
    @autoreleasepool {
        NSString *s = [[NSString alloc] initWithBytes:utf8 length:len encoding:NSUTF8StringEncoding];
        if (s == nil) return;
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:s forType:NSPasteboardTypeString];
    }
}

static int dwclip_read_all(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int dwclip_write_all(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t r = write(fd, (const char*)buf + sent, n - sent);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

static void dwclip_handle_conn(int fd)
{
    unsigned char op;
    if (dwclip_read_all(fd, &op, 1) != 0) return;

    if (op == 'C')
    {
        uint32_t cc = htonl((uint32_t)dwclip_changecount());
        dwclip_write_all(fd, &cc, 4);
    }
    else if (op == 'R')
    {
        size_t len = 0;
        char *data = dwclip_read(&len);
        uint32_t cc = htonl((uint32_t)dwclip_changecount());
        uint32_t nlen = htonl((uint32_t)len);
        dwclip_write_all(fd, &cc, 4);
        dwclip_write_all(fd, &nlen, 4);
        if (len > 0 && data != NULL) dwclip_write_all(fd, data, len);
        if (data != NULL) free(data);
    }
    else if (op == 'W')
    {
        uint32_t nlen;
        if (dwclip_read_all(fd, &nlen, 4) != 0) return;
        uint32_t len = ntohl(nlen);
        if (len > 32 * 1024 * 1024) return; // sanity cap
        char *data = (char*)malloc(len > 0 ? len : 1);
        if (data == NULL) return;
        if (len > 0 && dwclip_read_all(fd, data, len) != 0) { free(data); return; }
        dwclip_write(data, len);
        free(data);
        unsigned char ack = 'K';
        dwclip_write_all(fd, &ack, 1);
    }
}

static void* dwclip_listener_thread(void *arg)
{
    (void)arg;
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DWCLIP_SOCK_PATH, sizeof(addr.sun_path) - 1);
    unlink(DWCLIP_SOCK_PATH);

    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(lfd); return NULL; }
    // Root daemon connects; owner is the session user. Allow all so connect works
    // regardless of who the daemon runs as; the socket only exposes clipboard.
    chmod(DWCLIP_SOCK_PATH, 0666);
    if (listen(lfd, 8) < 0) { close(lfd); return NULL; }

    sck_flog("dwclip: listening on %s\n", DWCLIP_SOCK_PATH);

    for (;;)
    {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        dwclip_handle_conn(cfd);
        close(cfd);
    }
    close(lfd);
    return NULL;
}

// C-callable entry point: start the clipboard socket listener in a detached
// thread. Safe to call once at kvmagent startup (Aqua/in-session only).
void dwclip_start_listener(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, dwclip_listener_thread, NULL) == 0)
    {
        pthread_detach(t);
    }
}
