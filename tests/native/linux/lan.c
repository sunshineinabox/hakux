/* Copyright (C) 2026-present PortareOS */
/* LAN duplicate filtering and combined-backend cleanup tests. */
#include "native/net/net.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LANPORT 43090

static int g_failed;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        if (cond) {                                                                                          \
            printf("ok: ");                                                                                  \
        } else {                                                                                             \
            printf("FAIL: ");                                                                                \
            g_failed = 1;                                                                                    \
        }                                                                                                    \
        printf(__VA_ARGS__);                                                                                 \
        printf("\n");                                                                                        \
    } while (0)

static void option(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void option(const char *fmt, ...)
{
    char arg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(arg, sizeof(arg), fmt, ap);
    va_end(ap);
    if (!nc_net_option(arg)) {
        printf("FAIL: option %s not taken\n", arg);
        g_failed = 1;
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int open_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        return -1;
    }
    while (readdir(d)) {
        n++;
    }
    closedir(d);
    return n;
}

/* Use a separate source port to simulate another LAN path. */
static int peer(void)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    return fd;
}

/* Send a tagged broadcast frame. */
static void peer_send(int fd, char tag)
{
    uint8_t f[60] = { 0 };
    memset(f, 0xff, 6);
    const uint8_t src[6] = { 0x00, 0x50, 0xf2, 0xaa, 0xbb, 0xcc };
    memcpy(f + 6, src, 6);
    f[12] = 0x88, f[13] = 0xb5;                 /* a local experimental EtherType */
    f[14] = (uint8_t)tag;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(LANPORT),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    sendto(fd, f, sizeof(f), 0, (struct sockaddr *)&to, sizeof(to));
}

/* Count matching frames received within ms. */
static int console_got(char tag, int ms)
{
    int n = 0;
    const int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        nc_net_wait_rx(20 * 1000000ull);
        uint8_t f[NC_NET_FRAME_MAX];
        size_t len;
        while ((len = nc_net_recv(f, sizeof(f)))) {
            if (len >= 15 && f[12] == 0x88 && f[13] == 0xb5 && f[14] == (uint8_t)tag) {
                n++;
            }
        }
    }
    return n;
}

int main(void)
{
    option("net=lan");
    option("lanport=%u", LANPORT);
    option("lanbcast=127.0.0.1");
    nc_net_start();
    CHECK(nc_net_link(), "lan: started");
    const int a = peer(), b = peer();

    peer_send(a, 'r');
    peer_send(a, 'r');
    CHECK(console_got('r', 300) == 2, "a frame the peer sends twice reaches the console twice");

    peer_send(a, 'd');
    peer_send(b, 'd');
    CHECK(console_got('d', 300) == 1, "the same frame from another address within 100 ms reaches it once");

    peer_send(a, 'l');
    console_got('l', 150);
    peer_send(b, 'l');
    CHECK(console_got('l', 300) == 1, "the same frame from another address after 100 ms reaches it again");
    nc_net_stop();
    close(a);
    close(b);

    /* Both backends must close on stop. */
    option("net=all");
    option("upnp=off");
    const int before = open_fds();
    for (int i = 0; i < 3; i++) {
        nc_net_start();
        CHECK(nc_net_link(), "all: started (boot %d)", i + 1);
        nc_net_stop();
    }
    const int after = open_fds();
    CHECK(after == before, "all: no descriptors left after 3 boots (%d before, %d after)", before, after);

    printf(g_failed ? "FAIL: lan-smoke\n" : "PASS: lan-smoke\n");
    return g_failed;
}
