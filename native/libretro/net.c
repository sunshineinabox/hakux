/* Copyright (C) 2026-present PortareOS */
/*
 * hakuX native core — the libretro core's network (native/net), the Network
 * option: off, LAN (system link with the consoles on this network) or Xbox
 * Live through Insignia (NAT with Insignia's DNS server, matching the app's
 * "Xbox Live: Insignia" connection). LAN broadcasts on the default route's
 * interface and reaches consoles listed in lan-peers.txt in the save
 * directory, including peers on another subnet or across a VPN. Use
 * insignia-dns.txt in that directory to select another DNS server, such as
 * insigniaDNS on the local network, if the provider blocks Insignia's server.
 * Insignia accounts are created in the dashboard (no content, core.c).
 * Warn game users if no account exists, and dashboard users if Insignia is
 * disabled. If another console has the same identity (e.g. after copying
 * its hard disk), warn and request a new identity at the next game start,
 * as the app's "New identity" does.
 */
#include "native/libretro/libretro_int.h"
#include "native/kernel/kernel.h"
#include "native/net/net.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool g_twin_told;                /* this run's console with our identity, said */

/* The interface the IPv4 default route leaves by (the lowest metric's); ""
 * when there is none. */
static void default_route(char *name, size_t n)
{
    name[0] = 0;
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) {
        return;
    }
    char line[256], iface[IF_NAMESIZE + 1];
    unsigned dest, gw, flags, refs, use, metric, mask, best = UINT_MAX;
    if (fgets(line, sizeof(line), f)) {         /* the header */
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%16s %x %x %x %u %u %u %x", iface, &dest, &gw, &flags, &refs, &use, &metric,
                       &mask) == 8 && !dest && !mask && (flags & 1) && metric < best) {
                best = metric;
                snprintf(name, n, "%s", iface);
            }
        }
    }
    fclose(f);
}

/* The local network's broadcast address and prefix as lanbcast= takes them
 * ("a.b.c.d/bits"), and its interface's name: the default route's if it
 * broadcasts (Wi-Fi, Ethernet), else the first up that does. Never loopback
 * or a point-to-point tunnel (a VPN's, which has no broadcast). False if
 * there is none. */
static bool lan_bcast(char *out, size_t n, char *ifname, size_t ifn)
{
    char route[IF_NAMESIZE + 1];
    default_route(route, sizeof(route));
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) {
        return false;
    }
    const unsigned want = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
    const struct ifaddrs *pick = NULL;
    for (const struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask ||
            (i->ifa_flags & want) != want || (i->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) {
            continue;
        }
        if (!pick || (!strcmp(i->ifa_name, route) && strcmp(pick->ifa_name, route))) {
            pick = i;
        }
    }
    if (pick) {
        const uint32_t ip = ((const struct sockaddr_in *)pick->ifa_addr)->sin_addr.s_addr;
        const uint32_t mask = ((const struct sockaddr_in *)pick->ifa_netmask)->sin_addr.s_addr;
        const struct in_addr b = { .s_addr = ip | ~mask };
        char a[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &b, a, sizeof(a));
        snprintf(out, n, "%s/%d", a, __builtin_popcount(mask));
        snprintf(ifname, ifn, "%s", pick->ifa_name);
    }
    freeifaddrs(list);
    return pick != NULL;
}

/* Join words from <save directory>/<name> with commas. Input words are
 * separated by commas or whitespace; # starts a comment. Read at most max
 * words (0: no limit). Write an empty string if the file is missing or empty. */
static void save_dir_list(const char *name, unsigned max, char *out, size_t n)
{
    out[0] = 0;
    const char *root = NULL;
    if (!nc_lr_env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &root) || !root || !*root) {
        return;
    }
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", root, name);
    FILE *f = fopen(p, "r");
    if (!f) {
        return;
    }
    char line[256];
    size_t len = 0;
    unsigned words = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "#")] = 0;
        for (char *save = NULL, *t = strtok_r(line, ", \t\r\n", &save); t; t = strtok_r(NULL, ", \t\r\n", &save)) {
            if ((!max || words < max) && len + strlen(t) + 2 <= n) {
                len += (size_t)snprintf(out + len, n - len, "%s%s", len ? "," : "", t);
                words++;
            }
        }
    }
    fclose(f);
}

/* Read consoles from lan-peers.txt in lanpeers= format (IP[:PORT],...). */
static void lan_peers(char *out, size_t n)
{
    save_dir_list("lan-peers.txt", 0, out, n);
}

/* Insignia's primary DNS server (insignia.live/connect), matching the
 * app's AppPrefs.INSIGNIA_DNS. */
#define INSIGNIA_DNS "46.101.64.175"

/* Use the first word in insignia-dns.txt as the DNS address for dns=, falling
 * back to Insignia's server if it is missing or invalid. Only one server is
 * used: the NAT backend gives the console dns[0] (nat_init). */
static void insignia_dns(char *out, size_t n)
{
    char first[64];
    save_dir_list("insignia-dns.txt", 1, first, sizeof(first));
    /* Require an IPv4 address; an invalid value would leave the console without DNS. */
    struct in_addr a;
    if (!*first || inet_pton(AF_INET, first, &a) != 1) {
        if (*first) {
            nc_log("hakuX-libretro", "network: insignia-dns.txt names no IPv4 address (%s): Insignia's DNS server",
                   first);
        }
        snprintf(out, n, "%s", INSIGNIA_DNS);
        return;
    }
    snprintf(out, n, "%s", first);
}

/* Read online accounts from <hdd>/raw-partition0.bin, like the app's
 * Insignia.accounts: one per 512-byte sector starting at sector 8, with a
 * 16-byte gamertag at +0x18 and a 20-byte domain at +0x30. Write comma-separated
 * Insignia gamertags to out and return the account count. */
#define ACCOUNT_SECTORS 32u

static unsigned insignia_accounts(char *out, size_t n)
{
    out[0] = 0;
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/raw-partition0.bin", nc_io_hdd_root());
    FILE *f = fopen(p, "rb");
    if (!f) {
        return 0;
    }
    static uint8_t area[ACCOUNT_SECTORS * 512];
    const size_t got = fread(area, 1, sizeof(area), f);
    fclose(f);
    unsigned count = 0;
    size_t len = 0;
    for (size_t s = 8; (s + 1) * 512 <= got; s++) {
        char name[17], domain[21];
        snprintf(name, sizeof(name), "%.16s", (const char *)area + s * 512 + 0x18);
        snprintf(domain, sizeof(domain), "%.20s", (const char *)area + s * 512 + 0x30);
        bool printable = *name;
        for (const char *c = name; *c; c++) {
            printable = printable && *c >= ' ' && *c <= '~';
        }
        if (!printable || strcasecmp(domain, "insignia.live")) {
            continue;
        }
        count++;
        if (len + strlen(name) + 3 <= n) {
            len += (size_t)snprintf(out + len, n - len, "%s%s", len ? ", " : "", name);
        }
    }
    return count;
}

/* Log accounts at boot. Tell game users where to create one if needed;
 * dashboard users are already in the right place. */
static void insignia_status(void)
{
    char names[200];
    const unsigned count = insignia_accounts(names, sizeof(names));
    if (count) {
        nc_log("hakuX-libretro", "network: Insignia account%s on this console: %s", count == 1 ? "" : "s", names);
        return;
    }
    nc_log("hakuX-libretro", "network: no Insignia account on this console");
    if (nc_lr_dashboard()) {
        return;
    }
    static const struct retro_message msg = {
        "No Insignia account on this console yet: after registering it with Insignia's Setup Assistant, "
        "start the core without a game to open the dashboard, then Xbox Live", 600};
    nc_lr_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
}

/* Configure LAN broadcast and explicit peers. */
static void lan_setup(void)
{
    char bcast[32], ifname[IF_NAMESIZE + 1], peers[240], arg[256];
    const bool found = lan_bcast(bcast, sizeof(bcast), ifname, sizeof(ifname));
    snprintf(arg, sizeof(arg), "lanbcast=%s", found ? bcast : "255.255.255.255");
    nc_net_option(arg);
    lan_peers(peers, sizeof(peers));
    snprintf(arg, sizeof(arg), "lanpeers=%s", peers);
    nc_net_option(arg);
    if (found) {
        nc_log("hakuX-libretro", "network: LAN on %s (%s), consoles named: %s", ifname, bcast, *peers ? peers : "none");
    } else {
        nc_log("hakuX-libretro", "network: LAN, no local network found (broadcast to 255.255.255.255), consoles "
               "named: %s", *peers ? peers : "none");
    }
}

void nc_lr_net_apply(const char *mode)
{
    g_twin_told = false;
    /* net=all uses NAT and LAN together. */
    const bool all = !strcmp(mode, "all");
    if (all || !strcmp(mode, "insignia")) {
        char dns[64], arg[80];
        insignia_dns(dns, sizeof(dns));
        nc_net_option(all ? "net=all" : "net=nat");
        snprintf(arg, sizeof(arg), "dns=%s", dns);
        nc_net_option(arg);
        nc_log("hakuX-libretro", "network: Xbox Live through Insignia (NAT)%s, DNS %s%s",
               all ? " and system link" : "", dns, strcmp(dns, INSIGNIA_DNS) ? " (insignia-dns.txt)" : "");
        if (all) {
            lan_setup();
        }
        insignia_status();
        return;
    }
    if (nc_lr_dashboard()) {
        /* Dashboard account sign-up and recovery require Insignia networking. */
        static const struct retro_message msg = {
            "Xbox Live in the dashboard needs Network set to Xbox Live (Insignia), or Xbox Live and system link, "
            "in the core options", 480};
        nc_lr_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
        nc_log("hakuX-libretro", "network: the dashboard without Insignia (network %s)", mode);
    }
    if (strcmp(mode, "lan")) {
        nc_net_option("net=off");
        return;
    }
    nc_net_option("net=lan");
    lan_setup();
}

void nc_lr_net_run(void)
{
    char addr[INET_ADDRSTRLEN];
    if (!nc_net_take_twin(addr, sizeof(addr)) || g_twin_told) {
        return;
    }
    g_twin_told = true;
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/eeprom.renew", nc_io_hdd_root());
    FILE *f = fopen(p, "w");
    const bool renew = f && fclose(f) == 0;
    static char text[200];
    snprintf(text, sizeof(text), "The console at %s has this one's identity (a copied hard disk): %s", addr,
             renew ? "this one gets its own when the game starts next" : "they can't see each other");
    const struct retro_message msg = {text, 600};
    nc_lr_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
    nc_log("hakuX-libretro", "network: %s%s", text, renew ? "" : " (no eeprom.renew)");
}
