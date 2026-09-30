/*
 * title: MAC address parsing, formatting variants and OUI lookup
 * topic: networking
 * covers: colon dash Cisco dotted and bare hex MAC forms, strict rejection, I/G and U/L bits, OUI extraction, sorted table binary search, IPv4 multicast MAC mapping
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static int hv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Accepts aa:bb:cc:dd:ee:ff, aa-bb-cc-dd-ee-ff, aabb.ccdd.eeff, aabbccddeeff. */
static int parse_mac(const char *s, unsigned char m[6]) {
    size_t n = strlen(s);
    int digits[12], nd = 0;
    char sep = 0;
    if (n == 17 && (s[2] == ':' || s[2] == '-')) {
        sep = s[2];
        for (size_t i = 0; i < 17; i++) {
            if (i % 3 == 2) { if (s[i] != sep) return 0; continue; }
            int h = hv(s[i]);
            if (h < 0) return 0;
            digits[nd++] = h;
        }
    } else if (n == 14 && s[4] == '.' && s[9] == '.') {
        for (size_t i = 0; i < 14; i++) {
            if (i == 4 || i == 9) continue;
            int h = hv(s[i]);
            if (h < 0) return 0;
            digits[nd++] = h;
        }
    } else if (n == 12) {
        for (size_t i = 0; i < 12; i++) {
            int h = hv(s[i]);
            if (h < 0) return 0;
            digits[nd++] = h;
        }
    } else {
        return 0;
    }
    if (nd != 12) return 0;
    for (int i = 0; i < 6; i++) m[i] = (unsigned char)(digits[2 * i] * 16 + digits[2 * i + 1]);
    return 1;
}

typedef enum { F_COLON, F_DASH, F_DOT, F_BARE, F_COLON_UPPER } Form;
static void format_mac(const unsigned char *m, Form f, char *o) {
    switch (f) {
    case F_COLON: snprintf(o, 20, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]); break;
    case F_COLON_UPPER: snprintf(o, 20, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); break;
    case F_DASH: snprintf(o, 20, "%02x-%02x-%02x-%02x-%02x-%02x", m[0], m[1], m[2], m[3], m[4], m[5]); break;
    case F_DOT: snprintf(o, 20, "%02x%02x.%02x%02x.%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]); break;
    case F_BARE: snprintf(o, 20, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]); break;
    }
}

typedef struct { uint32_t oui; const char *vendor; } Oui;
/* sorted by oui for binary search */
static const Oui table[] = {
    { 0x00000c, "Cisco Systems" }, { 0x001b63, "Apple" }, { 0x005056, "VMware" }, { 0x080027, "PCS Systems (VirtualBox)" },
    { 0x3c5ab4, "Google" }, { 0xb827eb, "Raspberry Pi Foundation" }, { 0xdca632, "Raspberry Pi Trading" },
};

static const char *lookup_oui(uint32_t oui) {
    int lo = 0, hi = (int)(sizeof table / sizeof table[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (table[mid].oui == oui) return table[mid].vendor;
        if (table[mid].oui < oui) lo = mid + 1; else hi = mid - 1;
    }
    return "unknown";
}

int main(void) {
    for (size_t i = 1; i < sizeof table / sizeof table[0]; i++) CHECK(table[i - 1].oui < table[i].oui);
    static const char *ins[] = {
        "00:1b:63:84:45:e6", "3C-5A-B4-00-11-22", "b827.eb12.3456", "080027aabbcc", "01:00:5e:7f:00:01", "ff:ff:ff:ff:ff:ff",
        "02:42:ac:11:00:02", "33:33:00:00:00:fb", "00:50:56:c0:00:08", "de:ad:be:ef:00:01",
    };
    for (size_t i = 0; i < sizeof ins / sizeof ins[0]; i++) {
        unsigned char m[6];
        CHECK(parse_mac(ins[i], m));
        uint32_t oui = ((uint32_t)m[0] << 16) | ((uint32_t)m[1] << 8) | m[2];
        char a[20], b[20], c[20], d[20];
        format_mac(m, F_COLON_UPPER, a); format_mac(m, F_DASH, b); format_mac(m, F_DOT, c); format_mac(m, F_BARE, d);
        const char *kind = (m[0] & 1) ? ((m[0] == 0xff && m[1] == 0xff && m[2] == 0xff && m[3] == 0xff && m[4] == 0xff && m[5] == 0xff) ? "broadcast" : "multicast") : "unicast";
        printf("%s\n  %s %s %s %s\n  oui=%06x %s, %s, %s, vendor: %s\n", ins[i], a, b, c, d, (unsigned)oui, kind,
               (m[0] & 2) ? "locally-administered" : "universal", (m[0] & 1) ? "group" : "individual", lookup_oui(oui));
        /* Round trip through every form. */
        for (int f = 0; f <= F_COLON_UPPER; f++) {
            char t[20];
            unsigned char back[6];
            format_mac(m, (Form)f, t);
            CHECK(parse_mac(t, back) && memcmp(back, m, 6) == 0);
        }
    }
    static const char *bad[] = { "00:1b:63:84:45", "00:1b:63:84:45:e6:00", "00:1b:63:84:45:g6", "00:1b-63:84:45:e6", "001b.6384.45e6.", "001b:6384:45e6",
                                 "0:1b:63:84:45:e6", "00 1b 63 84 45 e6", "", "00:1b:63:84:45:e6 " };
    int rej = 0;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        unsigned char m[6];
        CHECK(!parse_mac(bad[i], m));
        rej++;
    }
    printf("rejected %d malformed MACs\n", rej);

    /* IPv4 multicast to Ethernet mapping: 01:00:5e + low 23 bits, so 32 groups collide per MAC. */
    static const uint32_t groups[] = { 0xe0000001u, 0xe00000fbu, 0xe0000116u, 0xe8000001u, 0xef7f0001u, 0xe0800001u, 0xeeffffffu };
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; i++) {
        uint32_t g = groups[i];
        unsigned char m[6] = { 0x01, 0x00, 0x5e, (unsigned char)((g >> 16) & 0x7f), (unsigned char)(g >> 8), (unsigned char)g };
        char s[20];
        format_mac(m, F_COLON, s);
        printf("group %u.%u.%u.%u -> %s\n", (unsigned)(g >> 24), (unsigned)((g >> 16) & 255), (unsigned)((g >> 8) & 255), (unsigned)(g & 255), s);
    }
    CHECK((groups[0] & 0x7fffff) == (groups[5] & 0x7fffff));
    /* The 5 unused bits give 32 ambiguous groups per MAC. */
    uint32_t base = 0xe0000001u;
    int same = 0;
    for (uint32_t k = 0; k < 32; k++) {
        uint32_t g = base + (k << 23);
        if ((g & 0x7fffff) == (base & 0x7fffff) && (g >> 28) == 0xe) same++;
    }
    printf("groups sharing one MAC (of 32 candidates in 224/4): %d\n", same);
    return 0;
}
