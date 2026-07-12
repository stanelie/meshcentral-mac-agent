// kvm_input_harness.c — feed KVM input frames to a kvmagent Unix socket,
// mimicking what the MeshCentral daemon relays. Used to validate the
// login-window VNC input revival WITHOUT MeshCentral/relay/onboarding.
//
// Wire format (raw, no wrapper) matches kvm_server_inputdata():
//   MNG_KVM_MOUSE(2), size=10: [00 02][00 0A][00][button][xHi xLo][yHi yLo]
//   MNG_KVM_KEY(1),   size=6 : [00 01][00 06][up][vk]
//
// Build (on host):  clang -arch arm64 -o kvm_input_harness kvm_input_harness.c
// Run (on VM, root): sudo ./kvm_input_harness /tmp/meshagent-kvm-0.sock
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

static void drain(int fd) { char b[16384]; while (read(fd, b, sizeof(b)) > 0) {} }

static int sendall(int fd, const unsigned char *b, int n) {
    int s = 0;
    while (s < n) {
        int r = (int)write(fd, b + s, n - s);
        if (r <= 0) { if (errno == EAGAIN) { usleep(2000); continue; } return -1; }
        s += r;
    }
    return 0;
}

static void mouse(int fd, int x, int y, unsigned char button) {
    unsigned char f[10] = { 0x00,0x02, 0x00,0x0A, 0x00, button,
        (unsigned char)(x>>8),(unsigned char)(x&0xFF),
        (unsigned char)(y>>8),(unsigned char)(y&0xFF) };
    sendall(fd, f, 10);
}

static void key(int fd, unsigned char vk, int up) {
    unsigned char f[6] = { 0x00,0x01, 0x00,0x06, (unsigned char)up, vk };
    sendall(fd, f, 6);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/meshagent-kvm-0.sock";
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("connect"); return 1; }
    printf("connected to %s — sweeping cursor for ~8s\n", path); fflush(stdout);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    // Sweep the cursor in a visible zig-zag so movement is unmistakable.
    for (int i = 0; i < 40; i++) {
        int x = 120 + (i * 17) % 640;
        int y = 160 + (i * 23) % 420;
        mouse(fd, x, y, 0);
        drain(fd);
        printf("mouse -> %d,%d\n", x, y); fflush(stdout);
        usleep(180000);
    }
    // Tap 'a' then Return (in case the password field is focused).
    key(fd, 0x41, 0); usleep(70000); key(fd, 0x41, 1); usleep(150000);
    key(fd, 0x0D, 0); usleep(70000); key(fd, 0x0D, 1); usleep(150000);
    drain(fd);
    printf("done\n");
    return 0;
}
