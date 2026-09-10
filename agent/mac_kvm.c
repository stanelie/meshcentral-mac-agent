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

// Temporary diagnostic log for loginwindow KVM debugging
static void kvm_flog(const char *fmt, ...) {
    FILE *f = fopen("/tmp/kvm_debug.log", "a");
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

	SCREEN_NUM = CGMainDisplayID();
	if (__builtin_available(macOS 10.15, *))
	{
		// Check (but do NOT request) TCC screen capture access.
		// CGRequestScreenCaptureAccess() triggers a recurring permission banner
		// even when auth_value=2 is already in TCC.db; CGDisplayCreateImage works
		// via the path-based TCC entry without needing the request call.
		bool pre = CGPreflightScreenCaptureAccess();
		kvm_flog("kvm_init: CGMainDisplayID=%u preflight=%d uid=%d\n",
			SCREEN_NUM, (int)pre, (int)getuid());
	}
	else
	{
		kvm_flog("kvm_init: CGMainDisplayID=%u uid=%d\n", SCREEN_NUM, (int)getuid());
	}
	
	if (SCREEN_WIDTH > 0)
	{
		CGDisplayModeRef mode = CGDisplayCopyDisplayMode(SCREEN_NUM);
		SCREEN_SCALE = (int) CGDisplayModeGetPixelWidth(mode) / SCREEN_WIDTH;
		CGDisplayModeRelease(mode);
	}

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
	SCREEN_HEIGHT = CGDisplayPixelsHigh(SCREEN_NUM) * SCREEN_SCALE;
	SCREEN_WIDTH = CGDisplayPixelsWide(SCREEN_NUM) * SCREEN_SCALE;
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

		screen_num = CGMainDisplayID();
		static int logged_once = 0;
		if (!logged_once) { kvm_flog("MainLoop start: CGMainDisplayID=%u\n", screen_num); logged_once = 1; }

		if (screen_num == 0) { kvm_flog("CGMainDisplayID=0, shutdown\n"); g_shutdown = 1; senddebug(-2); break; }

		if (SCREEN_SCALE_SET == 0)
		{
			CGDisplayModeRef mode = CGDisplayCopyDisplayMode(screen_num);
			if (SCREEN_WIDTH > 0 && SCREEN_SCALE < (int) CGDisplayModeGetPixelWidth(mode) / SCREEN_WIDTH)
			{
				SCREEN_SCALE = (int) CGDisplayModeGetPixelWidth(mode) / SCREEN_WIDTH;
				SCREEN_SCALE_SET = 1;
			}			 
			CGDisplayModeRelease(mode);
		}
		
		screen_height = CGDisplayPixelsHigh(screen_num) * SCREEN_SCALE;
		screen_width = CGDisplayPixelsWide(screen_num) * SCREEN_SCALE;
		
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
		if (getuid() == 0)
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
// Closing a session makes the agent exit, and its LaunchAgent sets
// ThrottleInterval=30 -- so launchd waits half a minute before respawning.
// Measured 2026-09-10: after a session closed, the socket was still dead 40s
// later, which is what produced both "agent socket not reachable" and the
// "still not granted" loop (the walkthrough was reading a stale log while the
// agent was simply down). `launchctl kickstart -k` bypasses the throttle;
// measured 13s to a fresh, listening agent.
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

#define PERM_AGENT_LOG "/tmp/kvm_debug.log"

// Last integer following `key` in the agent's log, or `deflt` if never seen.
static int perm_log_last_int(const char *key, int deflt)
{
    FILE *f = fopen(PERM_AGENT_LOG, "r");
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
// The restart must be forced. Letting launchd do it costs ThrottleInterval=30
// seconds, and the previous fixed sleep(4) simply read a stale log while the
// agent was still dead.
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

void kvm_check_permission()
{

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