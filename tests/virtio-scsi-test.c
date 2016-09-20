/*
 * QTest testcase for VirtIO SCSI
 *
 * Copyright (c) 2014 SUSE LINUX Products GmbH
 * Copyright (c) 2015 Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "block/scsi.h"
#include "libqos/virtio.h"
#include "libqos/virtio-pci.h"
#include "libqos/pci-pc.h"
#include "libqos/malloc.h"
#include "libqos/malloc-pc.h"
#include "libqos/malloc-generic.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_pci.h"
#include "standard-headers/linux/virtio_scsi.h"

#define HEXDUMP 0
#define DEBUG_QTEST 0

#if DEBUG_QTEST
#  define DPRINTF(fmt, ...) \
    do {                                                           \
        printf(fmt, ## __VA_ARGS__);           \
    } while (0)
#else
static inline GCC_FMT_ATTR(1, 2) int DPRINTF(const char *fmt, ...)
{
    return 0;
}
#endif

#define PCI_SLOT                0x02
#define PCI_FN                  0x00
#define QVIRTIO_SCSI_TIMEOUT_US (1 * 1000 * 1000)

#define MAX_NUM_QUEUES 64

typedef struct {
    QVirtioDevice *dev;
    QGuestAllocator *alloc;
    QPCIBus *bus;
    int num_queues;
    QVirtQueue *vq[MAX_NUM_QUEUES + 2];
} QVirtIOSCSI;

static void GCC_FMT_ATTR(1, 2)
qvirtio_scsi_start(const char *extra_opts, ...)
{
    char *cmdline, *cmdline1;
    va_list ap;

    va_start(ap, extra_opts);
    cmdline1 = g_strdup_vprintf(extra_opts, ap);
    va_end(ap);

    cmdline = g_strdup_printf("-device virtio-scsi-pci %s", cmdline1);
    qtest_start(cmdline);
    g_free(cmdline);
    g_free(cmdline1);
}

static void qvirtio_scsi_stop(void)
{
    qtest_end();
}

static void qvirtio_scsi_pci_free(QVirtIOSCSI *vs)
{
    int i;

    for (i = 0; i < vs->num_queues + 2; i++) {
        qvirtqueue_cleanup(&qvirtio_pci, vs->vq[i], vs->alloc);
    }
    pc_alloc_uninit(vs->alloc);
    qvirtio_pci_device_disable(container_of(vs->dev, QVirtioPCIDevice, vdev));
    g_free(vs->dev);
    qpci_free_pc(vs->bus);
}

static uint64_t qvirtio_scsi_alloc(QVirtIOSCSI *vs, size_t alloc_size,
                                   const void *data)
{
    uint64_t addr;

    addr = guest_alloc(vs->alloc, alloc_size);
    if (data) {
        memwrite(addr, data, alloc_size);
    }

    return addr;
}

static uint8_t virtio_scsi_do_command(QVirtIOSCSI *vs, const uint8_t *cdb,
                                      const uint8_t *data_in,
                                      size_t data_in_len,
                                      uint8_t *data_out, size_t data_out_len,
                                      struct virtio_scsi_cmd_resp *resp_out)
{
    QVirtQueue *vq;
    struct virtio_scsi_cmd_req req = { { 0 } };
    struct virtio_scsi_cmd_resp resp = { .response = 0xff, .status = 0xff };
    uint64_t req_addr, resp_addr, data_in_addr = 0, data_out_addr = 0;
    uint8_t response;
    uint32_t free_head;

    vq = vs->vq[2];

    req.lun[0] = 1; /* Select LUN */
    req.lun[1] = 1; /* Select target 1 */
    memcpy(req.cdb, cdb, VIRTIO_SCSI_CDB_SIZE);

    /* XXX: Fix endian if any multi-byte field in req/resp is used */

    /* Add request header */
    req_addr = qvirtio_scsi_alloc(vs, sizeof(req), &req);
    free_head = qvirtqueue_add(vq, req_addr, sizeof(req), false, true);

    if (data_out_len) {
        data_out_addr = qvirtio_scsi_alloc(vs, data_out_len, data_out);
        qvirtqueue_add(vq, data_out_addr, data_out_len, false, true);
    }

    /* Add response header */
    resp_addr = qvirtio_scsi_alloc(vs, sizeof(resp), &resp);
    qvirtqueue_add(vq, resp_addr, sizeof(resp), true, !!data_in_len);

    if (data_in_len) {
        data_in_addr = qvirtio_scsi_alloc(vs, data_in_len, data_in);
        qvirtqueue_add(vq, data_in_addr, data_in_len, true, false);
    }

    qvirtqueue_kick(&qvirtio_pci, vs->dev, vq, free_head);
    qvirtio_wait_queue_isr(&qvirtio_pci, vs->dev, vq, QVIRTIO_SCSI_TIMEOUT_US);

    response = readb(resp_addr +
                     offsetof(struct virtio_scsi_cmd_resp, response));

    if (resp_out) {
        memread(resp_addr, resp_out, sizeof(*resp_out));
    }

    guest_free(vs->alloc, req_addr);
    guest_free(vs->alloc, resp_addr);
    guest_free(vs->alloc, data_in_addr);
    guest_free(vs->alloc, data_out_addr);
    return response;
}

static QVirtIOSCSI *qvirtio_scsi_pci_init(int slot)
{
    const uint8_t test_unit_ready_cdb[VIRTIO_SCSI_CDB_SIZE] = {};
    QVirtIOSCSI *vs;
    QVirtioPCIDevice *dev;
    struct virtio_scsi_cmd_resp resp;
    void *addr;
    int i;

    vs = g_new0(QVirtIOSCSI, 1);
    vs->alloc = pc_alloc_init();
    vs->bus = qpci_init_pc(NULL);

    dev = qvirtio_pci_device_find(vs->bus, VIRTIO_ID_SCSI);
    vs->dev = (QVirtioDevice *)dev;
    g_assert(dev != NULL);
    g_assert_cmphex(vs->dev->device_type, ==, VIRTIO_ID_SCSI);

    qvirtio_pci_device_enable(dev);
    qvirtio_reset(&qvirtio_pci, vs->dev);
    qvirtio_set_acknowledge(&qvirtio_pci, vs->dev);
    qvirtio_set_driver(&qvirtio_pci, vs->dev);

    addr = dev->addr + VIRTIO_PCI_CONFIG_OFF(false);
    vs->num_queues = qvirtio_config_readl(&qvirtio_pci, vs->dev,
                                          (uint64_t)(uintptr_t)addr);

    g_assert_cmpint(vs->num_queues, <, MAX_NUM_QUEUES);

    for (i = 0; i < vs->num_queues + 2; i++) {
        vs->vq[i] = qvirtqueue_setup(&qvirtio_pci, vs->dev, vs->alloc, i);
    }

    /* Clear the POWER ON OCCURRED unit attention */
    g_assert_cmpint(virtio_scsi_do_command(vs, test_unit_ready_cdb,
                                           NULL, 0, NULL, 0, &resp),
                    ==, 0);
    g_assert_cmpint(resp.status, ==, CHECK_CONDITION);
    g_assert_cmpint(resp.sense[0], ==, 0x70); /* Fixed format sense buffer */
    g_assert_cmpint(resp.sense[2], ==, UNIT_ATTENTION);
    g_assert_cmpint(resp.sense[12], ==, 0x29); /* POWER ON */
    g_assert_cmpint(resp.sense[13], ==, 0x00);

    return vs;
}

static void hotplug(void)
{
    QDict *response;

    qvirtio_scsi_start("-drive id=drv1,if=none,file=/dev/null,format=raw");
    response = qmp("{\"execute\": \"device_add\","
                   " \"arguments\": {"
                   "   \"driver\": \"scsi-hd\","
                   "   \"id\": \"scsi-hd\","
                   "   \"drive\": \"drv1\""
                   "}}");

    g_assert(response);
    g_assert(!qdict_haskey(response, "error"));
    QDECREF(response);

    response = qmp("{\"execute\": \"device_del\","
                   " \"arguments\": {"
                   "   \"id\": \"scsi-hd\""
                   "}}");

    g_assert(response);
    g_assert(!qdict_haskey(response, "error"));
    g_assert(qdict_haskey(response, "event"));
    g_assert(!strcmp(qdict_get_str(response, "event"), "DEVICE_DELETED"));
    QDECREF(response);
    qvirtio_scsi_stop();
}

/* XXX: Move to common scsi code, and deduplicate with scsi-bus.c. */
int scsi_cdb_length(uint8_t *buf) {
    int cdb_len;

    switch (buf[0] >> 5) {
    case 0:
        cdb_len = 6;
        break;
    case 1:
    case 2:
        cdb_len = 10;
        break;
    case 4:
        cdb_len = 16;
        break;
    case 5:
        cdb_len = 12;
        break;
    default:
        cdb_len = VIRTIO_SCSI_CDB_SIZE;
    }
    return cdb_len;
}

static void run_cmd(QVirtIOSCSI *vs, const uint8_t *cdb,
                    uint8_t *readcmp, int readlen,
                    uint8_t *writebuf, int writelen,
                    int response, int status,
                    const SCSISense *sense)
{
    int i;
    struct virtio_scsi_cmd_resp resp = { 0 };
    uint8_t *readbuf = NULL;

    DPRINTF("CDB: ");
    for (i = 0; i < scsi_cdb_length((uint8_t *)cdb); ++i) {
        DPRINTF("%02X ", cdb[i]);
    }
    DPRINTF("\n");

    if (readlen) {
        readbuf = g_malloc0(readlen);
    }
    g_assert_cmphex(response, ==,
                    virtio_scsi_do_command(vs, cdb, readlen ? readbuf : NULL,
                                           readlen, writebuf, writelen, &resp));
    g_assert_cmphex(resp.status, ==, status);
    if (response == VIRTIO_SCSI_S_OK && status == GOOD && readlen) {
        if (HEXDUMP) {
            fprintf(stderr, "\n");
            qemu_hexdump((char *)readbuf, stderr, "readbuf", readlen);
            qemu_hexdump((char *)readcmp, stderr, "readcmp", readlen);
        }
        g_assert_cmpmem(readcmp, readlen, readbuf, readlen);
    }
    if (sense) {
        g_assert_cmphex(resp.sense[0], ==, 0x70);
        g_assert_cmphex(resp.sense[2], ==, sense->key);
        g_assert_cmphex(resp.sense[12], ==, sense->asc);
        g_assert_cmphex(resp.sense[13], ==, sense->ascq);
    }
    g_free(readbuf);
}

/* Test WRITE SAME with the lba not aligned */
static void test_unaligned_write_same(void)
{
    QVirtIOSCSI *vs;
    uint8_t buf1[512] = { 0 };
    uint8_t buf2[512] = { 1 };
    const uint8_t write_same_cdb_1[VIRTIO_SCSI_CDB_SIZE] = {
        0x41, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x00
    };
    const uint8_t write_same_cdb_2[VIRTIO_SCSI_CDB_SIZE] = {
        0x41, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x33, 0x00, 0x00
    };

    qvirtio_scsi_start("-drive file=blkdebug::null-co://,if=none,id=dr1"
                       ",format=raw,file.align=4k "
                       "-device scsi-disk,drive=dr1,lun=0,scsi-id=1");
    vs = qvirtio_scsi_pci_init(PCI_SLOT);

    run_cmd(vs, write_same_cdb_1, NULL, 0, buf1, 512, 0, GOOD, NULL);
    run_cmd(vs, write_same_cdb_2, NULL, 0, buf2, 512, 0, GOOD, NULL);

    qvirtio_scsi_pci_free(vs);
    qvirtio_scsi_stop();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/virtio/scsi/pci/hotplug", hotplug);
    qtest_add_func("/virtio/scsi/pci/scsi-disk/unaligned-write-same",
                   test_unaligned_write_same);

    return g_test_run();
}
