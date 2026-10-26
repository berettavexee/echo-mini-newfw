/* Host fuzzer for the core B Ogg codecs (docs/FUZZING.md, ETUDE §62): our oggpage.c,
 * codec_vorbis.c (Tremor lowmem) and codec_opus.c (libopus), built for the PC with ASan + UBSan
 * and gcc's trace-pc coverage. Coverage-guided like AFL: every input runs in a forked child
 * (fresh statics, crashes and hangs caught); edges hit go to a shared bitmap; a mutated input
 * reaching a new edge joins the corpus.
 *
 *   fuzz SEEDDIR OUTDIR [SECONDS]     fuzz; crashes in OUTDIR/crash-*, hangs in OUTDIR/hang-*
 *   fuzz -r FILE...                   replay (sanitizer report on stderr)
 *
 * Replay options: FUZZ_V=1 prints the format and frames decoded, FUZZ_PCM=file writes the output
 * words (whole file, no time limit), FUZZ_NOFORK=1 runs in this process (for gdb).
 * Target chosen at build time (build.sh): -DT_OGG (pg_packet only), -DT_VORBIS, -DT_OPUS,
 * -DT_MP3, -DT_AACPKT (codec_aac.c on a packet stream), and the core A readers: -DT_MP4
 * (src/mp4.c), -DT_AAC (mp4.c, then codec_aac.c on the stream it makes), -DT_TAGS (src/tags.c
 * for every format); -DREAL_HEAP keeps core B's hifi/heap.c for Vorbis (no ASan).
 * FUZZ_STREAM=file (T_MP4, T_AAC replay) writes the packet stream mp4.c makes: seeds for
 * T_AACPKT. */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include "rt.h"
#include "oggpage.h"
#if defined(T_MP4) || defined(T_AAC) || defined(T_TAGS)
#include "mp4.h"
#include "tags.h"
#endif

/* ---- core B runtime over a memory buffer ---- */
static const uint8_t *in_buf;
static uint32_t in_len;
static long waits;

uint32_t in_avail(void) { return ctl->in_wr - ctl->in_rd; }
int in_ended(void) { return 1; }
uint32_t in_peek_ahead(uint32_t off, void *d, uint32_t n)
{
    uint32_t a = in_avail();
    if (off >= a)
        return 0;
    if (n > a - off)
        n = a - off;
    memcpy(d, in_buf + ctl->in_rd + off, n);
    return n;
}
uint32_t in_peek(void *d, uint32_t n) { return in_peek_ahead(0, d, n); }
uint32_t in_read(void *d, uint32_t n) { n = in_peek(d, n); ctl->in_rd += n; return n; }
void in_skip(uint32_t n) { uint32_t a = in_avail(); ctl->in_rd += n < a ? n : a; }
uint32_t in_read_wait(void *d, uint32_t n) { return in_read(d, n); }
int in_seek(uint32_t pos)
{
    if (pos > ctl->in_end)
        return -1;
    ctl->in_rd = pos;
    return 0;
}
uint32_t out_space(void) { return 1u << 30; }
static FILE *pcm_out;                   /* FUZZ_PCM=file (replay): the output words */
void out_word(uint32_t w) { if (pcm_out) fwrite(&w, 4, 1, pcm_out); }
void out_write(const uint32_t *w, uint32_t n) { if (pcm_out) fwrite(w, 4, n, pcm_out); }
void out_done(uint32_t frames) { ctl->frames += frames; }
void rt_wait(void)
{
    if (++waits > 100000)
        _exit(77);                      /* waits for input that never comes: a hang */
}
uint32_t rt_heap_peak;
void *rt_alloc(uint32_t n) { return malloc(n ? n : 1); }
void rt_heap_reset(void) { }

#if defined(T_VORBIS) && !defined(REAL_HEAP)
/* hifi/heap.c is replaced by libc malloc (bounds seen by ASan); the codebook cache never hits */
static uint8_t zero_heap[64];
uint32_t heap_peak, heap_cur;
void heap_reset(void) { }
uint32_t heap_keep(void) { return 0; }
void heap_resume(uint32_t floor, uint32_t cur) { (void)floor; (void)cur; }
const uint8_t *heap_base(void) { return zero_heap; }
#endif

#if defined(T_MP4) || defined(T_AAC) || defined(T_TAGS)
/* ---- core A side: the file in memory ---- */
int fat_pread(const struct fat_file *f, struct fat_cursor *c, uint32_t pos, void *buf, uint32_t n)
{
    (void)c;
    if (pos >= f->size)
        return 0;
    if (n > f->size - pos)
        n = f->size - pos;
    memcpy(buf, in_buf + pos, n);
    return (int)n;
}
void log_printf(const char *fmt, ...)
{
    if (getenv("FUZZ_V")) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}
#endif
#if defined(T_MP4) || defined(T_AAC)
/* mp4.c's packet stream from seek_ms, through the real ring (HP_IN_BASE), copied out into a
 * linear buffer (at most max bytes): its length, or 0 when mp4_open() refuses the file */
static uint32_t mp4_stream(const struct fat_file *f, uint32_t seek_ms, uint8_t *out, uint32_t max,
                           struct mp4_track *t)
{
    if (mp4_open(f, seek_ms, t) != MP4_OK)
        return 0;
    uint32_t wr = 0, nr;
    for (int i = 0; i < 100000 && !mp4_done() && wr < max; i++) {
        uint32_t room = max - wr < HP_IN_SIZE ? max - wr : HP_IN_SIZE;
        uint32_t k = mp4_feed(wr, room, &nr);
        for (uint32_t j = 0; j < k; j++)
            out[wr + j] = ((const uint8_t *)HP_IN_BASE)[(wr + j) % HP_IN_SIZE];
        wr += k;
        if (!k)
            break;
    }
    if (getenv("FUZZ_STREAM")) {
        FILE *o = fopen(getenv("FUZZ_STREAM"), "wb");
        if (o) {
            fwrite(out, 1, wr, o);
            fclose(o);
        }
    }
    return wr;
}
#endif

/* ---- coverage: edges (previous block, block) hashed into a shared 64 KiB bitmap ---- */
#define MAP_SIZE 65536
static uint8_t *cov;
static uintptr_t prev;
__attribute__((no_sanitize_address, no_sanitize("undefined")))
void __sanitizer_cov_trace_pc(void)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    pc = (pc >> 4) ^ (pc << 8);
    if (cov)
        cov[(pc ^ prev) & (MAP_SIZE - 1)]++;
    prev = pc >> 1;
}

#if !defined(T_OGG) && !defined(T_MP4) && !defined(T_TAGS)
/* A core B codec on the input buffer: open, then up to 400 blocks (all with FUZZ_PCM) */
static void run_codec(void)
{
    volatile struct hp_ctl *c = ctl;
    int r;
    while ((r = codec_open()) == 1)
        ;
    if (r < 0)
        return;
    if (getenv("FUZZ_PCM"))
        pcm_out = fopen(getenv("FUZZ_PCM"), "wb");
    for (int i = 0; pcm_out || i < 400; i++)
        if (codec_run() != 1)
            break;
    if (pcm_out)
        fclose(pcm_out);
    if (getenv("FUZZ_V"))
        fprintf(stderr, "  open ok: %u Hz, %u ch, %u frames out\n", c->rate, c->channels, c->frames);
}
#endif

static void run_one(const uint8_t *d, uint32_t n)
{
    volatile struct hp_ctl *c = ctl;
    memset((void *)c, 0, sizeof *c);
    in_buf = d;
    in_len = n;
    c->in_wr = c->in_end = n;
    if (!getenv("FUZZ_PCM"))
        alarm(5);
#if defined(T_MP4) || defined(T_AAC) || defined(T_TAGS)
    struct fat_file f;
    memset(&f, 0, sizeof f);
    memcpy(f.name, "FUZZ.M4A", 9);
    f.size = n;
#endif
#if defined(T_TAGS)
    static const uint32_t codecs[] = { HP_CODEC_MP3, HP_CODEC_AAC, HP_CODEC_FLAC, HP_CODEC_VORBIS,
                                       HP_CODEC_WAV, HP_CODEC_AIFF, HP_CODEC_MPC };
    struct tags t;
    for (unsigned i = 0; i < sizeof codecs / sizeof *codecs; i++) {
        tags_read(&f, codecs[i], &t);
        if (getenv("FUZZ_V") && (t.title[0] || t.artist[0] || t.album[0]))
            fprintf(stderr, "  tags (codec %u): '%s' / '%s' / '%s' / %u\n", codecs[i], t.title,
                    t.artist, t.album, t.track);
    }
#elif defined(T_MP4)
    static uint8_t stream[1 << 20];
    struct mp4_track t;
    uint32_t k = mp4_stream(&f, 0, stream, sizeof stream, &t);
    if (getenv("FUZZ_V"))
        fprintf(stderr, "  mp4: codec %u, %u Hz, %u ch, %u packets, stream %u bytes\n",
                t.codec, t.rate, t.channels, t.samples, k);
    if (k && t.length)
        mp4_stream(&f, (uint32_t)((uint64_t)t.length * 500 / (t.rate ? t.rate : 1)), stream,
                   sizeof stream, &t);                  /* and a seek to the middle */
#elif defined(T_AAC)
    static uint8_t stream[1 << 20];
    struct mp4_track t;
    uint32_t k = mp4_stream(&f, 0, stream, sizeof stream, &t);
    if (!k || t.codec != HP_CODEC_AAC)
        return;
    in_buf = stream;
    in_len = k;
    c->in_rd = 0;
    c->in_wr = c->in_end = k;
    run_codec();
#elif defined(T_OGG)
    static uint8_t pkt[65536];
    pg_reset();
    for (int i = 0; i < 4000; i++)
        if (pg_packet(pkt, sizeof pkt) == 0)
            break;
#else
    run_codec();
#endif
}

/* ---- corpus and mutations ---- */
struct entry { uint8_t *d; uint32_t n; };
static struct entry *corpus;
static int ncorpus, capcorpus;
static uint8_t virgin[MAP_SIZE];
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return n ? (uint32_t)(rng % n) : 0;
}

static void add(const uint8_t *d, uint32_t n)
{
    if (ncorpus == capcorpus)
        corpus = realloc(corpus, (capcorpus = capcorpus * 2 + 16) * sizeof *corpus);
    corpus[ncorpus].d = malloc(n ? n : 1);
    memcpy(corpus[ncorpus].d, d, n);
    corpus[ncorpus++].n = n;
}

/* The region most mutations go to: the first 12 KiB (Ogg and MP3 headers: codebooks, ID3v2,
 * Xing/LAME), or for MP4 the moov box wherever it is (the end of the file when not
 * "faststart"); hot_set() finds it for each input */
static uint32_t hot_lo, hot_hi;
static void hot_set(const uint8_t *b, uint32_t n)
{
    hot_lo = 0;
    hot_hi = n < 12288 ? n : 12288;
#if defined(T_MP4) || defined(T_AAC) || defined(T_TAGS)
    for (uint32_t i = 4; i + 4 <= n; i++)
        if (!memcmp(b + i, "moov", 4)) {
            uint32_t sz = (uint32_t)b[i - 4] << 24 | b[i - 3] << 16 | b[i - 2] << 8 | b[i - 1];
            hot_lo = i - 4;
            hot_hi = sz >= 8 && sz <= n - hot_lo ? hot_lo + sz : n;
            break;
        }
#endif
}

/* Positions: three out of four in the hot region (T_TAGS: one in eight in the last 512 bytes,
 * ID3v1 and APEv2), the rest anywhere */
static uint32_t pos(uint32_t n)
{
    if (!n)
        return 0;
#if defined(T_TAGS)
    if (!rnd(8))
        return n - 1 - rnd(n < 512 ? n : 512);
#endif
    if (rnd(4) && hot_hi > hot_lo && hot_hi <= n)
        return hot_lo + rnd(hot_hi - hot_lo);
    return rnd(n);
}

static uint32_t mutate(uint8_t *b, uint32_t n, uint32_t cap)
{
    static const uint8_t interesting[] = { 0, 1, 0x7f, 0x80, 0xff, 0xfe, 0x40, 0x20, 0x10 };
    int k = 1 + rnd(8);
    while (k--) {
        switch (rnd(8)) {
        case 0: case 1: if (n) b[pos(n)] ^= 1u << rnd(8); break;
        case 2: if (n) b[pos(n)] = interesting[rnd(sizeof interesting)]; break;
        case 3: if (n) b[pos(n)] = (uint8_t)rnd(256); break;
        case 4: if (n > 8) { uint32_t p = pos(n - 4); for (int i = 0; i < 4; i++) b[p + i] = rnd(256); } break;
        case 5: if (n > 2) n = 1 + rnd(n); break;                                /* truncate */
        case 6: if (n > 64) { uint32_t p = pos(n - 32), l = 1 + rnd(32);          /* delete */
                    memmove(b + p, b + p + l, n - p - l); n -= l; } break;
        case 7: if (n > 64 && n + 64 < cap) { uint32_t p = pos(n - 32), l = 1 + rnd(32), q = rnd(n - l);
                    memmove(b + p + l, b + p, n - p); memcpy(b + p, b + q + (q >= p ? l : 0), l); n += l; }
                break;                                                           /* duplicate */
        }
    }
    return n;
}

static int new_edges(void)
{
    int fresh = 0;
    for (int i = 0; i < MAP_SIZE; i++)
        if (cov[i] && !virgin[i]) {
            virgin[i] = 1;
            fresh++;
        }
    return fresh;
}

static int exec_child(const uint8_t *d, uint32_t n, int quiet)
{
    memset(cov, 0, MAP_SIZE);
    if (getenv("FUZZ_NOFORK")) {                /* replay in this process (for a debugger) */
        run_one(d, n);
        return 0;
    }
    pid_t p = fork();
    if (!p) {
        if (quiet) {
            int fd = open("/dev/null", 1);
            dup2(fd, 2);
        }
        run_one(d, n);
        _exit(0);
    }
    int st;
    waitpid(p, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 77)
        return 'h';
    if (WIFSIGNALED(st))
        return WTERMSIG(st) == SIGALRM ? 'h' : 'c';
    if (WIFEXITED(st) && WEXITSTATUS(st))
        return 'c';                     /* ASan/UBSan exit with an error code */
    return 0;
}

static void save(const char *dir, int kind, const uint8_t *d, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ d[i]) * 16777619u;
    char name[512];
    snprintf(name, sizeof name, "%s/%s-%08x", dir, kind == 'h' ? "hang" : "crash", h);
    FILE *f = fopen(name, "wb");
    if (f) {
        fwrite(d, 1, n, f);
        fclose(f);
    }
    printf("  %s\n", name);
    fflush(stdout);
}

static uint8_t *load(const char *path, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l ? l : 1);
    *n = (uint32_t)fread(d, 1, l, f);
    fclose(f);
    return d;
}

const char *__asan_default_options(void) { return "detect_leaks=0:abort_on_error=0"; }

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    if (mmap((void *)(uintptr_t)HP_CTL_ADDR, HP_OUT_BASE + HP_OUT_SIZE - HP_CTL_ADDR, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) {
        perror("mmap ctl");
        return 1;
    }
    cov = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (argc > 2 && !strcmp(argv[1], "-r")) {
        for (int i = 2; i < argc; i++) {
            uint32_t n;
            uint8_t *d = load(argv[i], &n);
            int r = d ? exec_child(d, n, 0) : 'c';
            printf("%s: %s\n", argv[i], r == 'h' ? "HANG" : r ? "CRASH" : "ok");
        }
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "fuzz SEEDDIR OUTDIR [SECONDS] | fuzz -r FILE...\n");
        return 1;
    }
    int seconds = argc > 3 ? atoi(argv[3]) : 600;
    mkdir(argv[2], 0755);
    DIR *dir = opendir(argv[1]);
    struct dirent *de;
    char path[1024];
    while (dir && (de = readdir(dir))) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", argv[1], de->d_name);
        uint32_t n;
        uint8_t *d = load(path, &n);
        if (!d)
            continue;
        int r = exec_child(d, n, 1);
        printf("seed %s: %u bytes, %s, %d edges\n", de->d_name, n, r ? "FAILS" : "ok", new_edges());
        add(d, n);
        free(d);
    }
    if (!ncorpus)
        return 1;
    rng ^= (uint64_t)time(0);
    uint32_t cap = 1 << 20;
    uint8_t *b = malloc(cap);
    time_t t0 = time(0), tl = t0;
    long execs = 0, crashes = 0, hangs = 0;
    while (time(0) - t0 < seconds) {
        struct entry *e = &corpus[rnd(ncorpus)];
        uint32_t n = e->n < cap ? e->n : cap;
        memcpy(b, e->d, n);
        hot_set(b, n);
        n = mutate(b, n, cap);
        int r = exec_child(b, n, 1);
        execs++;
        if (r) {
            r == 'h' ? hangs++ : crashes++;
            save(argv[2], r, b, n);
        } else if (new_edges())
            add(b, n);
        if (time(0) - tl >= 30) {
            tl = time(0);
            printf("[%4ld s] %ld execs (%ld/s), corpus %d, crashes %ld, hangs %ld\n",
                   (long)(tl - t0), execs, execs / (long)(tl - t0), ncorpus, crashes, hangs);
            fflush(stdout);
        }
    }
    printf("done: %ld execs, corpus %d, crashes %ld, hangs %ld\n", execs, ncorpus, crashes, hangs);
    return 0;
}
