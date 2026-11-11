/* The play queue on the internal memory (ETUDE §128): NEWFWQ.DAT, created by the firmware at the
 * root (one run of clusters, fatwrite.c), a header then two halves of QF_MAX entries of 64 bytes:
 * a track's cluster, size, flags (codec, F_SD, F_CONTIG), its card's serial (version 2, 0.102b13:
 * a track of another card is never read at its cluster on the card in) and its name. A new queue is
 * written into the half not in use, then the header names it (a power cut leaves the queue before);
 * tracks added at the end are written after the last one, then the header counts them. The
 * entries are read one sector at a time (8 a sector), never the whole queue: it needs no RAM. */
#include "ui_priv.h"
#include "emmc.h"
#include "fatwrite.h"
#include "library.h"
#include "qfile.h"

#define QF_FILE     "NEWFWQ.DAT"
#define QF_ENT      64u
#define QF_PER_SEC  (512u / QF_ENT)
#define QF_HALF_SEC (QF_MAX / QF_PER_SEC)       /* 3072 sectors: 1.5 MiB */
#define QF_SIZE     (512u + 2u * QF_HALF_SEC * 512u)

struct qf_ent {
    uint32_t cluster, size;
    uint16_t flags, pad;
    uint32_t serial;                            /* the card's (F_SD), else 0 */
    char name[QF_ENT - 16];
};
#define QF_VERSION 2
_Static_assert(sizeof(struct qf_ent) == QF_ENT, "a queue entry is 64 bytes");

/* the sectors: one read, one written (PMU-SRAM after the queue's folders: not the BSS) */
static uint8_t rbuf[512] __attribute__((section(".pmusram"), aligned(4)));
static uint8_t wbuf[512] __attribute__((section(".pmusram"), aligned(4)));

static struct {
    int state;                                  /* QF_NONE, QF_OPEN, QF_FAILED */
    uint32_t first;                             /* the file's first sector (the header) */
    uint32_t active, count, gen;                /* the header's */
    uint32_t rsec;                              /* the sector in rbuf, or ~0 */
    int writing;                                /* a session: the half, the next entry */
    uint32_t whalf, wn, wsec;
} qf;
enum { QF_NONE, QF_OPEN, QF_FAILED };

static uint32_t half_sec(uint32_t h) { return qf.first + 1 + QF_HALF_SEC * h; }

static int header_read(void)
{
    if (emmc_read(qf.first, 1, rbuf))
        return -1;
    qf.rsec = ~0u;
    const uint32_t *w = (const uint32_t *)rbuf;
    if (__builtin_memcmp(rbuf, "NEWFWQUE", 8) || w[2] != QF_VERSION ||
        ~lib_crc(0xffffffffu, rbuf, 508) != w[127] || (w[3] > 1 && w[3] != 0xffffffffu) ||
        w[4] > QF_MAX) {
        qf.active = 0xffffffffu;                /* none (or damaged: a queue no more) */
        qf.count = qf.gen = 0;
        return 0;
    }
    qf.active = w[3];
    qf.count = w[3] > 1 ? 0 : w[4];
    qf.gen = w[5];
    return 0;
}

static int header_write(uint32_t active, uint32_t count, uint32_t gen)
{
    uint32_t *w = (uint32_t *)wbuf;
    for (int i = 0; i < 128; i++)
        w[i] = 0;
    __builtin_memcpy(wbuf, "NEWFWQUE", 8);
    w[2] = QF_VERSION;
    w[3] = active;
    w[4] = count;
    w[5] = gen;
    w[127] = ~lib_crc(0xffffffffu, wbuf, 508);
    if (emmc_user_area(qf.first, 1 + 2 * QF_HALF_SEC) || emmc_write_user(qf.first, 1, wbuf)) {
        emmc_user_area(0, 0);
        return -1;
    }
    emmc_user_area(0, 0);
    qf.active = active;
    qf.count = count;
    qf.gen = gen;
    return 0;
}

int qf_open(void)
{
    if (qf.state == QF_OPEN)
        return 0;
    if (qf.state == QF_FAILED)
        return -1;
    qf.state = QF_FAILED;
    struct fat_file f;
    uint32_t count;
    int r = fat_find_root(QF_FILE, &f);
    if (r) {                                    /* not there: made, its header naming no half */
        uint32_t *w = (uint32_t *)wbuf;
        for (int i = 0; i < 128; i++)
            w[i] = 0;
        __builtin_memcpy(wbuf, "NEWFWQUE", 8);
        w[2] = QF_VERSION;
        w[3] = 0xffffffffu;
        w[127] = ~lib_crc(0xffffffffu, wbuf, 508);
        r = fat_create_contig(QF_FILE, QF_SIZE, wbuf, &f);
        log_printf("queue: %s %s (%d)", QF_FILE, r < 0 ? "not created" : "created", r);
        if (r < 0)
            return -1;
    }
    if (f.size != QF_SIZE || fat_extent(&f, &qf.first, &count)) {
        log_printf("queue: %s is not one (%u bytes): left as it is", QF_FILE, f.size);
        return -1;
    }
    if (header_read())
        return -1;
    qf.state = QF_OPEN;
    return 0;
}

void qf_forget(void)
{
    qf.state = QF_NONE;
    qf.writing = 0;
}

uint32_t qf_count(void)
{
    return qf.state == QF_OPEN ? qf.count : 0;
}

uint32_t qf_gen(void)
{
    return qf.state == QF_OPEN ? qf.gen : 0;
}

/* Entry i of half h: 0, or -1; its card's serial in *serial */
static int get(uint32_t h, uint32_t i, struct entry *e, char *name, int max, uint32_t *serial)
{
    uint32_t s = half_sec(h) + i / QF_PER_SEC;
    if (s != qf.rsec) {
        qf.rsec = ~0u;
        if (emmc_read(s, 1, rbuf))
            return -1;
        qf.rsec = s;
    }
    const struct qf_ent *q = (const struct qf_ent *)(rbuf + QF_ENT * (i % QF_PER_SEC));
    e->cluster = q->cluster;
    e->size = q->size;
    e->flags = q->flags;
    e->name = 0;
    *serial = q->serial;
    if (name) {
        int k = 0;
        for (; k < max - 1 && k < (int)sizeof q->name && q->name[k]; k++)
            name[k] = q->name[k];
        name[k] = 0;
    }
    return 0;
}

/* The card in: its serial, or 0 (none, or not readable) */
static uint32_t card_serial(void)
{
    if (!sd_present() || (!sdvol.mounted && (sd_card_bad() || sd_open())))
        return 0;                               /* (a card that failed: not tried again here) */
    return sdvol.mounted ? sdvol.serial : 0;
}

int qf_get(uint32_t i, struct entry *e, char *name, int max)
{
    uint32_t serial;
    if (qf.state != QF_OPEN || qf.active > 1 || i >= qf.count ||
        get(qf.active, i, e, name, max, &serial))
        return -1;
    if ((e->flags & F_SD) && (!serial || serial != card_serial()))
        e->flags |= F_OTHER;                    /* another card, or none: passed over */
    return 0;
}

/* ---- writing: a new queue in the other half (qf_begin), or more at the end (qf_append) ---- */
static int flush(void)
{
    if (emmc_user_area(qf.first, 1 + 2 * QF_HALF_SEC) || emmc_write_user(qf.wsec, 1, wbuf)) {
        emmc_user_area(0, 0);
        return -1;
    }
    emmc_user_area(0, 0);
    if (qf.wsec == qf.rsec)
        qf.rsec = ~0u;                          /* (rbuf: that sector before) */
    return 0;
}

/* the session's next entries go to sector s: its old content kept when it holds some */
static int load_wsec(uint32_t s, int keep)
{
    qf.wsec = s;
    if (keep)
        return emmc_read(s, 1, wbuf) ? -1 : 0;
    for (int i = 0; i < 512; i++)
        wbuf[i] = 0;
    return 0;
}

int qf_begin(void)
{
    if (qf_open())
        return -1;
    qf.whalf = qf.active == 0 ? 1 : 0;
    qf.wn = 0;
    qf.writing = 1;
    return load_wsec(half_sec(qf.whalf), 0);
}

int qf_append(void)
{
    if (qf_open() || qf.active > 1)
        return -1;
    qf.whalf = qf.active;
    qf.wn = qf.count;
    qf.writing = 1;
    return load_wsec(half_sec(qf.whalf) + qf.wn / QF_PER_SEC, qf.wn % QF_PER_SEC != 0);
}

static int put_s(const struct entry *e, const char *name, uint32_t serial)
{
    if (!qf.writing || qf.wn >= QF_MAX)
        return -1;
    uint32_t s = half_sec(qf.whalf) + qf.wn / QF_PER_SEC;
    if (s != qf.wsec && (flush() || load_wsec(s, 0)))
        return -1;
    struct qf_ent *q = (struct qf_ent *)(wbuf + QF_ENT * (qf.wn % QF_PER_SEC));
    q->cluster = e->cluster;
    q->size = e->size;
    q->flags = e->flags & (uint16_t)~F_OTHER;
    q->pad = 0;
    q->serial = serial;
    int k = 0;
    for (; name && name[k] && k < (int)sizeof q->name - 1; k++)
        q->name[k] = name[k];
    for (; k < (int)sizeof q->name; k++)
        q->name[k] = 0;
    qf.wn++;
    return 0;
}

/* A track the queue gets now: on the card in (F_SD), its serial with it */
int qf_put(const struct entry *e, const char *name)
{
    return put_s(e, name, e->flags & F_SD ? card_serial() : 0);
}

/* the session's entries made the queue (the header written last), or dropped */
int qf_end(int keep)
{
    if (!qf.writing)
        return -1;
    qf.writing = 0;
    if (!keep)
        return 0;
    if (flush() || header_write(qf.whalf, qf.wn, qf.gen + 1))
        return -1;
    return 0;
}

/* The queue with n entries put at place 'at' (into the other half): from 'put', which gives
 * them one by one (0, or -1 to stop). 0, or -1 (nothing changed) */
int qf_insert(uint32_t at, uint32_t n, int (*put)(uint32_t k, void *ctx), void *ctx)
{
    if (qf_open() || qf.active > 1 || at > qf.count || qf.count + n > QF_MAX)
        return -1;
    uint32_t from = qf.active, count = qf.count;
    if (qf_begin())
        return -1;
    struct entry e;
    char name[QF_ENT];
    uint32_t serial;
    for (uint32_t i = 0; i < count + n; i++) {
        int r;
        if (i >= at && i < at + n)
            r = put(i - at, ctx);
        else {                                  /* (a track kept: its card's serial kept) */
            r = get(from, i < at ? i : i - n, &e, name, sizeof name, &serial) ? -1 :
                put_s(&e, name, serial);
            if (i % 64 == 0)
                ui_background();                /* (a long queue: the music goes on) */
        }
        if (r) {
            qf_end(0);
            return -1;
        }
    }
    return qf_end(1);
}
