/*
 * PipeWire Module: Tiny Microphone  (virtual source)
 * ==================================================
 *
 * Accepts PCM audio from a Unix-domain-socket client and exposes it
 * as a virtual microphone (Audio/Source) to PipeWire.
 *
 * Architecture:
 *   [Android AAudio capture] ─socket─> [socket thread] ─ringbuf─> [RT callback] ─> pw_stream
 *
 * API references (from PipeWire source at pipewire/src/modules/):
 *   module-example-source.c:182    buf->size = size / impl->frame_size;  // ← MUST set
 *   module-example-source.c:179-181  bd->chunk->{size,stride,offset}     // ← 3 fields
 *   module-example-source.c:218      PW_DIRECTION_OUTPUT                 // ← source direction
 *   module-example-source.c:385      PW_KEY_MEDIA_CLASS = "Audio/Source"
 *
 *   spa/include/spa/utils/ringbuffer.h:128-132  spa_ringbuffer_get_write_index  → fill level
 *   spa/include/spa/utils/ringbuffer.h:78-82    spa_ringbuffer_get_read_index   → available data
 *
 * Latency control:
 *   Ringbuffer consumer checks fill level each read cycle.
 *   If backlog > LATENCY_LIMIT_MS, read pointer snaps forward
 *   to w - latency_limit_bytes, discarding old data.
 *   → latency stays bounded at ~LATENCY_LIMIT_MS.
 *
 * Discard (anti-latency):
 *   On client connect, read-pointer is set to w - discard_bytes
 *   (skipping the first discard.seconds of received data).
 *   This is a one-shot pointer jump, not iterative drain.
 *
 * Compile:
 *   gcc -shared -fPIC -O2 -Wall -D_GNU_SOURCE \
 *       -o libpipewire-module-tiny-microphone.so module-tiny-microphone.c \
 *       $(pkg-config --cflags --libs libpipewire-0.3) -lpthread
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <pipewire/impl.h>

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0.0"
#endif

#define NAME                "tiny-microphone"
#define DEFAULT_SOCKET_PATH "/tmp/.tiny.mic"
#define DEFAULT_FORMAT      "F32LE"
#define DEFAULT_RATE        48000
#define DEFAULT_CHANNELS    1
#define DEFAULT_POSITION    "[ MONO ]"
#define DEFAULT_DISCARD_SEC 2.0f

/* Ringbuffer: 4 seconds at 48kHz mono F32 = 768 KB */
#define RINGBUFFER_SECONDS   4
#define RINGBUFFER_MAX_BYTES (384000u * 8u * 4u)

/* Max latency before we discard old data (milliseconds) */
#define LATENCY_LIMIT_MS     150

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE \
    "( socket.path=<path, default:" DEFAULT_SOCKET_PATH "> ) " \
    "( node.name=<source name> ) "                                 \
    "( node.description=<description> ) "                          \
    "( audio.rate=<rate, default:" SPA_STRINGIFY(DEFAULT_RATE) "> ) "\
    "( audio.channels=<channels, default:" SPA_STRINGIFY(DEFAULT_CHANNELS) "> ) "\
    "( audio.format=<format, default:" DEFAULT_FORMAT "> ) "       \
    "( audio.position=<position, default:" DEFAULT_POSITION "> ) " \
    "( discard.seconds=<seconds, default:" SPA_STRINGIFY(DEFAULT_DISCARD_SEC) "> ) "

static const struct spa_dict_item module_props[] = {
    { PW_KEY_MODULE_AUTHOR,      "com.fct.tc4" },
    { PW_KEY_MODULE_DESCRIPTION, "Tiny Microphone – virtual source from Unix socket" },
    { PW_KEY_MODULE_USAGE,       MODULE_USAGE },
    { PW_KEY_MODULE_VERSION,     PACKAGE_VERSION },
};

/* ================================================================= */
/*  Ringbuffer (wraps spa_ringbuffer with owned backing memory)      */
/*  ─────────                                                        */
/*  Producer: socket thread   (rb_write)                             */
/*  Consumer: RT callback     (rb_read / rb_read_latency_bound)      */
/* ================================================================= */
struct audio_ringbuffer {
    struct spa_ringbuffer rb;
    uint8_t              *mem;
    uint32_t              size;
};

static void rb_init(struct audio_ringbuffer *a, uint32_t sz) {
    a->size = sz; a->mem = calloc(1, sz); spa_ringbuffer_init(&a->rb);
}
static void rb_destroy(struct audio_ringbuffer *a) { free(a->mem); a->mem = NULL; }

/* Producer.  Returns bytes written (0 = buffer full → drop). */
static uint32_t rb_write(struct audio_ringbuffer *a, const void *src, uint32_t n)
{
    uint32_t w;
    int32_t fill = spa_ringbuffer_get_write_index(&a->rb, &w);
    if (fill >= (int32_t)a->size) return 0;
    uint32_t space = a->size - (uint32_t)fill;
    if (n > space) n = space;
    if (!n) return 0;
    spa_ringbuffer_write_data(&a->rb, a->mem, a->size, w % a->size, src, n);
    spa_ringbuffer_write_update(&a->rb, w + n);
    return n;
}

/*
 * Consumer with latency limit.
 * If the backlog exceeds `limit_bytes`, the read pointer is snapped
 * forward to (write_index - limit_bytes), discarding old data.
 * Then reads up to `n` bytes.  Returns bytes actually read.
 *
 * Called from the RT callback – zero blocking, zero alloc.
 */
static uint32_t rb_read_latency_bound(struct audio_ringbuffer *a,
                                       void *dst, uint32_t n,
                                       uint32_t limit_bytes)
{
    uint32_t r, w;
    int32_t avail = spa_ringbuffer_get_read_index(&a->rb, &r);
    if (avail <= 0) return 0;

    /* ── latency guard ── */
    (void)spa_ringbuffer_get_write_index(&a->rb, &w);
    uint32_t backlog = w - r;
    if (backlog > limit_bytes + n) {
        /* Too much old data – skip to within limit_bytes of write pointer */
        uint32_t new_r = w - limit_bytes;
        spa_ringbuffer_read_update(&a->rb, new_r);
        r = new_r;
        avail = (int32_t)(w - r);
        if (avail <= 0) return 0;
    }

    if (n > (uint32_t)avail) n = (uint32_t)avail;
    spa_ringbuffer_read_data(&a->rb, a->mem, a->size, r % a->size, dst, n);
    spa_ringbuffer_read_update(&a->rb, r + n);
    return n;
}

/* Jump read pointer to `target` (one-shot, e.g. for initial discard). */
static void rb_jump_read_to(struct audio_ringbuffer *a, uint32_t target)
{
    spa_ringbuffer_read_update(&a->rb, target);
}

/* ================================================================= */
/*  Private state                                                    */
/* ================================================================= */
struct impl {
    struct pw_context    *context;
    struct pw_properties *props;
    struct pw_impl_module *module;
    struct spa_hook       module_listener;
    struct pw_core       *core;
    struct spa_hook       core_proxy_listener, core_listener;

    struct pw_stream     *stream;
    struct spa_hook       stream_listener;
    struct spa_audio_info_raw info;
    uint32_t              frame_size;
    unsigned int          do_disconnect : 1;

    /* socket */
    char                  sock_path[108];
    pthread_t             sock_thread;
    atomic_bool           thread_running;
    atomic_bool           client_connected;
    int                   server_fd;

    /* ringbuffer */
    struct audio_ringbuffer ring;
    uint32_t                latency_limit_bytes; /* LATENCY_LIMIT_MS in bytes */

    /* discard */
    float                 discard_s;
    atomic_bool           discarding;
    uint32_t              discard_bytes;
    atomic_uint           total_recv;  /* raw bytes read from client socket */
};

static void impl_destroy(struct impl *impl);

/* ================================================================= */
/*  Socket helpers                                                   */
/* ================================================================= */
static int fd_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0); return f < 0 ? -1 : fcntl(fd, F_SETFL, f | O_NONBLOCK);
}
static int fd_block(int fd) {
    int f = fcntl(fd, F_GETFL, 0); return f < 0 ? -1 : fcntl(fd, F_SETFL, f & ~O_NONBLOCK);
}
static int read_exact(int fd, void *b, size_t n) {
    uint8_t *p = b; size_t r = n;
    while (r) { ssize_t got = read(fd, p, r); if (got <= 0) return got == 0 ? -1 : (errno == EINTR ? 0 : -1); p += got; r -= got; }
    return 0;
}
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* ================================================================= */
/*  Socket thread                                                    */
/* ================================================================= */
static void *sock_func(void *d)
{
    struct impl *impl = d;
    int cli = -1;

    pw_log_info("sock thread start: %s", impl->sock_path);

    while (atomic_load(&impl->thread_running)) {
        if (cli < 0) {
            while (atomic_load(&impl->thread_running)) {
                cli = accept(impl->server_fd, NULL, NULL);
                if (cli >= 0) break;
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(100000); continue; }
                pw_log_error("accept: %m"); goto out;
            }
            if (cli < 0) goto out;
            pw_log_info("client fd=%d", cli);
            fd_block(cli);

            /* read format header from client */
            uint8_t hdr[12];
            if (read_exact(cli, hdr, 12) < 0) { pw_log_warn("hdr read fail"); goto dc; }
            uint32_t rate = rd32le(hdr+0), ch = rd32le(hdr+4), fmt = rd32le(hdr+8);
            pw_log_info("client fmt: rate=%u ch=%u fmt=%u", rate, ch, fmt);
            if (rate)     impl->info.rate     = rate;
            if (ch)       impl->info.channels = ch;
            if (fmt)      impl->info.format   = (enum spa_audio_format)fmt;
            impl->frame_size = impl->info.channels * sizeof(float);

            /* re-calc latency limit & discard threshold */
            impl->latency_limit_bytes = impl->frame_size * impl->info.rate * LATENCY_LIMIT_MS / 1000u;
            impl->discard_bytes = impl->frame_size * impl->info.rate * (uint32_t)impl->discard_s;

            atomic_store(&impl->total_recv, 0);
            atomic_store(&impl->discarding, impl->discard_bytes > 0);
            atomic_store(&impl->client_connected, true);
            pw_log_info("latency_limit=%u bytes  discard=%u bytes",
                         impl->latency_limit_bytes, impl->discard_bytes);
        }

        /* read PCM */
        uint8_t buf[16384];
        while (atomic_load(&impl->thread_running)) {
            ssize_t n = read(cli, buf, sizeof(buf));
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }

            rb_write(&impl->ring, buf, (uint32_t)n);

            /* count raw received bytes (NOT rb_write return!) – see ringbuffer.h:128 */
            uint32_t prev = atomic_fetch_add(&impl->total_recv, (uint32_t)n);

            /* check discard threshold */
            if (atomic_load(&impl->discarding) && (prev + (uint32_t)n) >= impl->discard_bytes) {
                atomic_store(&impl->discarding, false);
                /* one-shot: jump read pointer to discard_bytes behind write pointer.
                 * This effectively throws away the first discard_bytes of data. */
                uint32_t w;
                (void)spa_ringbuffer_get_write_index(&impl->ring.rb, &w);
                uint32_t target = (w > impl->discard_bytes) ? (w - impl->discard_bytes) : 0;
                rb_jump_read_to(&impl->ring, target);
                pw_log_info("discard done, jumped to %u (discarded %u bytes)", target, impl->discard_bytes);
            }
        }

dc:     shutdown(cli, SHUT_RDWR); close(cli); cli = -1;
        atomic_store(&impl->client_connected, false);
        pw_log_info("client gone");
    }
out:
    if (cli >= 0) { shutdown(cli, SHUT_RDWR); close(cli); }
    pw_log_info("sock thread end");
    return NULL;
}

/* ================================================================= */
/*  Server start / stop                                              */
/* ================================================================= */
static int sock_start(struct impl *impl)
{
    struct sockaddr_un a; int fd;
    unlink(impl->sock_path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { pw_log_error("socket: %m"); return -errno; }
    memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
    strncpy(a.sun_path, impl->sock_path, sizeof(a.sun_path)-1);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { pw_log_error("bind: %m"); close(fd); return -errno; }
    chmod(impl->sock_path, 0666);
    if (listen(fd, 1) < 0) { pw_log_error("listen: %m"); unlink(impl->sock_path); close(fd); return -errno; }
    fd_nonblock(fd);
    impl->server_fd = fd;
    atomic_store(&impl->thread_running, true);
    if (pthread_create(&impl->sock_thread, NULL, sock_func, impl)) {
        atomic_store(&impl->thread_running, false); close(fd); unlink(impl->sock_path); return -1;
    }
    pw_log_info("listening %s", impl->sock_path);
    return 0;
}
static void sock_stop(struct impl *impl)
{
    atomic_store(&impl->thread_running, false);
    if (impl->server_fd >= 0) {
        int w = socket(AF_UNIX, SOCK_STREAM, 0);
        if (w >= 0) { struct sockaddr_un a; memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX;
            strncpy(a.sun_path,impl->sock_path,sizeof(a.sun_path)-1);
            connect(w,(struct sockaddr*)&a,sizeof(a)); close(w); }
    }
    pthread_join(impl->sock_thread, NULL);
    if (impl->server_fd >= 0) { close(impl->server_fd); impl->server_fd = -1; }
    unlink(impl->sock_path);
}

/* ================================================================= */
/*  PipeWire stream (Audio/Source, PW_DIRECTION_OUTPUT)              */
/*  ───────────────────────────────────────────────────              */
/*  Exact pattern from pipewire/src/modules/module-example-source.c  */
/* ================================================================= */
static void stream_destroy(void *d) { struct impl *i = d; spa_hook_remove(&i->stream_listener); i->stream = NULL; }
static void stream_state(void *d, enum pw_stream_state old, enum pw_stream_state st, const char *err) {
    struct impl *i = d; (void)old;
    if (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED) pw_impl_module_schedule_destroy(i->module);
    else if (st == PW_STREAM_STATE_STREAMING) pw_log_info("source streaming: rate=%u ch=%u", i->info.rate, i->info.channels);
}

/*
 * RT callback – PipeWire wants audio from our source.
 * Pattern: module-example-source.c:158-185
 */
static void stream_process(void *d)
{
    struct impl *impl = d;
    struct pw_buffer *buf;
    struct spa_data *bd;
    void *data;
    uint32_t size;

    /* --- module-example-source.c:166 --- */
    if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) { pw_log_debug("no buf"); return; }

    /* --- module-example-source.c:171-174 --- */
    bd   = &buf->buffer->datas[0];
    data = bd->data;
    size = buf->requested ? buf->requested * impl->frame_size : bd->maxsize;

    if (atomic_load(&impl->client_connected) && !atomic_load(&impl->discarding)) {
        /* Normal: fill from ringbuffer with latency bound.
         * Latency limit: if backlog > LATENCY_LIMIT_MS, skip old data. */
        uint32_t got = rb_read_latency_bound(&impl->ring, data, size,
                                              impl->latency_limit_bytes);
        if (got < size)
            memset((uint8_t*)data + got, 0, size - got);
    } else {
        /* Discarding or no client → silence */
        memset(data, 0, size);
    }

    /* --- module-example-source.c:179-182 --- */
    bd->chunk->offset = 0;
    bd->chunk->size   = size;
    bd->chunk->stride = impl->frame_size;
    buf->size = size / impl->frame_size;    /* ← CRITICAL: was missing before */

    /* --- module-example-source.c:184 --- */
    pw_stream_queue_buffer(impl->stream, buf);
}

static const struct pw_stream_events stream_ev = {
    PW_VERSION_STREAM_EVENTS, .destroy = stream_destroy, .state_changed = stream_state, .process = stream_process,
};

static int stream_create(struct impl *impl, struct pw_properties *sp)
{
    uint8_t b[1024]; struct spa_pod_builder pb; const struct spa_pod *p[1];
    impl->stream = pw_stream_new(impl->core, "tiny mic source", sp);
    if (!impl->stream) return -errno;
    pw_stream_add_listener(impl->stream, &impl->stream_listener, &stream_ev, impl);
    spa_pod_builder_init(&pb, b, sizeof(b));
    p[0] = spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &impl->info);
    /* --- module-example-source.c:217-223 --- */
    return pw_stream_connect(impl->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, p, 1);
}

/* ================================================================= */
/*  Core events                                                      */
/* ================================================================= */
static void core_err(void *d, uint32_t id, int seq, int res, const char *msg) {
    struct impl *i = d;
    pw_log_error("core err id:%u res:%d: %s", id, res, msg);
    if (id == PW_ID_CORE && res == -EPIPE) pw_impl_module_schedule_destroy(i->module);
}
static const struct pw_core_events core_ev = { PW_VERSION_CORE_EVENTS, .error = core_err };
static void core_proxy_die(void *d) { struct impl *i = d; spa_hook_remove(&i->core_listener); i->core = NULL; pw_impl_module_schedule_destroy(i->module); }
static const struct pw_proxy_events proxy_ev = { .destroy = core_proxy_die };

/* ================================================================= */
/*  Lifecycle                                                        */
/* ================================================================= */
static void impl_destroy(struct impl *i) {
    if (atomic_load(&i->thread_running)) sock_stop(i);
    if (i->stream) pw_stream_destroy(i->stream);
    if (i->core && i->do_disconnect) pw_core_disconnect(i->core);
    rb_destroy(&i->ring); pw_properties_free(i->props); free(i);
}
static void mod_destroy(void *d) { struct impl *i = d; spa_hook_remove(&i->module_listener); impl_destroy(i); }
static const struct pw_impl_module_events mod_ev = { PW_VERSION_IMPL_MODULE_EVENTS, .destroy = mod_destroy };

/* ================================================================= */
/*  Helpers                                                          */
/* ================================================================= */
static int parse_audio(const struct pw_properties *pr, struct spa_audio_info_raw *inf) {
    return spa_audio_info_raw_init_dict_keys(inf,
        &SPA_DICT_ITEMS(
            SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT,   DEFAULT_FORMAT),
            SPA_DICT_ITEM(SPA_KEY_AUDIO_RATE,     SPA_STRINGIFY(DEFAULT_RATE)),
            SPA_DICT_ITEM(SPA_KEY_AUDIO_CHANNELS, SPA_STRINGIFY(DEFAULT_CHANNELS)),
            SPA_DICT_ITEM(SPA_KEY_AUDIO_POSITION,  DEFAULT_POSITION)),
        &pr->dict, SPA_KEY_AUDIO_FORMAT, SPA_KEY_AUDIO_RATE, SPA_KEY_AUDIO_CHANNELS, SPA_KEY_AUDIO_LAYOUT, SPA_KEY_AUDIO_POSITION, NULL);
}
static uint32_t frame_sz(const struct spa_audio_info_raw *inf) {
    uint32_t ch = inf->channels;
    switch (inf->format) {
    case SPA_AUDIO_FORMAT_U8: case SPA_AUDIO_FORMAT_S8: case SPA_AUDIO_FORMAT_ALAW: case SPA_AUDIO_FORMAT_ULAW: return ch;
    case SPA_AUDIO_FORMAT_S16_LE: case SPA_AUDIO_FORMAT_S16_BE: case SPA_AUDIO_FORMAT_U16_LE: case SPA_AUDIO_FORMAT_U16_BE: return ch*2;
    case SPA_AUDIO_FORMAT_S24_32_LE: case SPA_AUDIO_FORMAT_S24_32_BE: case SPA_AUDIO_FORMAT_S32_LE: case SPA_AUDIO_FORMAT_S32_BE:
    case SPA_AUDIO_FORMAT_F32_LE: case SPA_AUDIO_FORMAT_F32_BE: return ch*4;
    case SPA_AUDIO_FORMAT_F64_LE: case SPA_AUDIO_FORMAT_F64_BE: return ch*8;
    default: return 0;
    }
}
static void cp_prop(struct pw_properties *d, const struct pw_properties *s, const char *k) {
    const char *v; if ((v = pw_properties_get(s,k)) && !pw_properties_get(d,k)) pw_properties_set(d,k,v);
}

/* ================================================================= */
/*  Module entry                                                     */
/* ================================================================= */
SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *mod, const char *args)
{
    struct pw_context *ctx = pw_impl_module_get_context(mod);
    uint32_t id = pw_global_get_id(pw_impl_module_get_global(mod)), pid = getpid();
    struct pw_properties *pr = NULL, *sp = NULL;
    struct impl *i; const char *s; int r;

    PW_LOG_TOPIC_INIT(mod_topic);
    i = calloc(1, sizeof(*i)); if (!i) return -errno;
    if (!args) args = "";
    i->discard_s = DEFAULT_DISCARD_SEC; i->server_fd = -1;

    pr = pw_properties_new_string(args); if (!pr) { r = -errno; goto err; } i->props = pr;
    sp = pw_properties_new(NULL, NULL); if (!sp) { r = -errno; goto err; }
    i->module = mod; i->context = ctx;

    s = pw_properties_get(pr, "socket.path"); if (!s) s = DEFAULT_SOCKET_PATH;
    strncpy(i->sock_path, s, sizeof(i->sock_path)-1); i->sock_path[sizeof(i->sock_path)-1] = 0;

    s = pw_properties_get(pr, "discard.seconds");
    if (s) { float v; if (spa_atof(s, &v) && v >= 0) i->discard_s = v; }

    if (!pw_properties_get(pr, PW_KEY_NODE_VIRTUAL)) pw_properties_set(pr, PW_KEY_NODE_VIRTUAL, "true");
    if (!pw_properties_get(pr, PW_KEY_MEDIA_CLASS))  pw_properties_set(pr, PW_KEY_MEDIA_CLASS, "Audio/Source");
    if (!pw_properties_get(pr, PW_KEY_NODE_NAME))    pw_properties_setf(pr, PW_KEY_NODE_NAME, "tiny-mic-%u-%u", pid, id);
    if (!pw_properties_get(pr, PW_KEY_NODE_DESCRIPTION)) pw_properties_set(pr, PW_KEY_NODE_DESCRIPTION, pw_properties_get(pr, PW_KEY_NODE_NAME));

    s = pw_properties_get(pr, "stream.props"); if (s) pw_properties_update_string(sp, s, strlen(s));
    cp_prop(sp, pr, PW_KEY_AUDIO_RATE); cp_prop(sp, pr, PW_KEY_AUDIO_CHANNELS);
    cp_prop(sp, pr, SPA_KEY_AUDIO_LAYOUT); cp_prop(sp, pr, SPA_KEY_AUDIO_POSITION);
    cp_prop(sp, pr, PW_KEY_NODE_NAME); cp_prop(sp, pr, PW_KEY_NODE_DESCRIPTION);
    cp_prop(sp, pr, PW_KEY_NODE_GROUP); cp_prop(sp, pr, PW_KEY_NODE_LATENCY);
    cp_prop(sp, pr, PW_KEY_NODE_VIRTUAL); cp_prop(sp, pr, PW_KEY_MEDIA_CLASS);

    if ((r = parse_audio(sp, &i->info)) < 0) { pw_log_error("parse fmt: %s", spa_strerror(r)); goto err; }
    i->frame_size = frame_sz(&i->info);
    if (!i->frame_size) { r = -EINVAL; pw_log_error("bad fmt"); goto err; }

    i->latency_limit_bytes = i->frame_size * i->info.rate * LATENCY_LIMIT_MS / 1000u;
    i->discard_bytes = i->frame_size * i->info.rate * (uint32_t)i->discard_s;
    pw_log_info("rate=%u ch=%u frame=%u latency_limit=%u discard=%u",
                 i->info.rate, i->info.channels, i->frame_size, i->latency_limit_bytes, i->discard_bytes);

    { uint32_t sz = i->frame_size * i->info.rate * RINGBUFFER_SECONDS;
      if (!sz || sz > RINGBUFFER_MAX_BYTES) sz = RINGBUFFER_MAX_BYTES; rb_init(&i->ring, sz); }

    i->core = pw_context_get_object(ctx, PW_TYPE_INTERFACE_Core);
    if (!i->core) { s = pw_properties_get(pr, PW_KEY_REMOTE_NAME);
        i->core = pw_context_connect(ctx, pw_properties_new(PW_KEY_REMOTE_NAME, s, NULL), 0); i->do_disconnect = 1; }
    if (!i->core) { r = -errno; goto err; }
    pw_proxy_add_listener((struct pw_proxy*)i->core, &i->core_proxy_listener, &proxy_ev, i);
    pw_core_add_listener(i->core, &i->core_listener, &core_ev, i);

    if ((r = stream_create(i, sp)) < 0) { sp = NULL; pw_log_error("stream: %s", spa_strerror(r)); goto err; }
    sp = NULL;
    if ((r = sock_start(i)) < 0) { pw_log_error("sock: %s", spa_strerror(r)); goto err; }

    pw_impl_module_add_listener(mod, &i->module_listener, &mod_ev, i);
    pw_impl_module_update_properties(mod, &SPA_DICT_INIT_ARRAY(module_props));
    return 0;
err:
    pw_properties_free(sp); impl_destroy(i); return r;
}
