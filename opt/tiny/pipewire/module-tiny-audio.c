/*
 * PipeWire Module: Tiny Audio Sink
 *
 * A virtual audio sink that streams captured system audio over a Unix domain
 * socket.  Designed to work with the com.fct.tc4 Android companion.
 *
 * Architecture
 * ------------
 *   pw_stream (Audio/Sink, RT callback)
 *        │
 *        ▼  ringbuffer (SPSC lock-free)
 *   [socket thread]  ── accept ──>  client_fd  ── write() ──>  Android
 *
 * Compile (single gcc command):
 *   gcc -shared -fPIC -O2 -Wall -D_GNU_SOURCE \
 *       -o libpipewire-module-tiny-audio.so module-tiny-audio.c \
 *       $(pkg-config --cflags --libs libpipewire-0.3) -lpthread
 *
 * PipeWire config snippet:
 *   context.modules = [
 *   {   name = libpipewire-module-tiny-audio
 *       args = {
 *           socket.path    = "/tmp/.tiny.audio"
 *           node.name      = "tiny_audio_sink"
 *           node.description = "Tiny Audio Output"
 *           audio.rate     = 48000
 *           audio.channels = 2
 *           audio.format   = "F32LE"
 *           audio.position = [ FL FR ]
 *       }
 *   }
 *   ]
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

/* ----------------------------------------------------------------- */
/*  Build-time defines (replaces meson-generated config.h)           */
/* ----------------------------------------------------------------- */
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0.0"
#endif

/* ----------------------------------------------------------------- */
/*  Constants                                                        */
/* ----------------------------------------------------------------- */
#define NAME                "tiny-audio"
#define DEFAULT_SOCKET_PATH "/tmp/.tiny.audio"
#define DEFAULT_FORMAT      "F32LE"
#define DEFAULT_RATE        48000
#define DEFAULT_CHANNELS    2
#define DEFAULT_POSITION    "[ FL FR ]"

/* Ringbuffer holds RINGBUFFER_SECONDS of audio at the negotiated format. */
#define RINGBUFFER_SECONDS      2
#define RINGBUFFER_MAX_FRAMES   384000   /* 192 kHz × 2 s */
#define RINGBUFFER_MAX_BYTES    (RINGBUFFER_MAX_FRAMES * 8 * 4)
                                      /* 8 channels × 4 bytes/frame */

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

/* ----------------------------------------------------------------- */
/*  Module properties                                                */
/* ----------------------------------------------------------------- */
#define MODULE_USAGE \
    "( socket.path=<unix-socket-path, default:" DEFAULT_SOCKET_PATH "> ) " \
    "( node.name=<sink name> ) "                                           \
    "( node.description=<description> ) "                                  \
    "( audio.rate=<sample rate, default:" SPA_STRINGIFY(DEFAULT_RATE) "> ) "\
    "( audio.channels=<channels, default:" SPA_STRINGIFY(DEFAULT_CHANNELS) "> ) "\
    "( audio.format=<format, default:" DEFAULT_FORMAT "> ) "               \
    "( audio.position=<channel map, default:" DEFAULT_POSITION "> ) "

static const struct spa_dict_item module_props[] = {
    { PW_KEY_MODULE_AUTHOR,      "com.fct.tc4" },
    { PW_KEY_MODULE_DESCRIPTION, "Tiny Audio – virtual sink over Unix socket" },
    { PW_KEY_MODULE_USAGE,       MODULE_USAGE },
    { PW_KEY_MODULE_VERSION,     PACKAGE_VERSION },
};

/* ================================================================= */
/*  Ringbuffer                                                       */
/*  ─────────                                                        */
/*  Producer (RT callback) / consumer (socket thread).               */
/*  spa_ringbuffer API ref: spa/include/spa/utils/ringbuffer.h       */
/*    get_write_index → fill level (line 128-132)                   */
/*    get_read_index  → avail data (line 78-82)                     */
/* ================================================================= */
#define LATENCY_LIMIT_MS  150   /* max ringbuffer backlog before skipping */

struct audio_ringbuffer {
    struct spa_ringbuffer  rb;
    uint8_t               *data;
    uint32_t               size;
    uint32_t               latency_limit_bytes;
};

static void rb_init(struct audio_ringbuffer *a, uint32_t sz) {
    a->size = sz; a->data = calloc(1, sz); spa_ringbuffer_init(&a->rb); a->latency_limit_bytes = 0;
}
static void rb_destroy(struct audio_ringbuffer *a) { free(a->data); a->data = NULL; }

/* Producer (RT thread). ringbuffer.h:128 */
static uint32_t rb_write(struct audio_ringbuffer *a, const void *src, uint32_t n) {
    uint32_t w; int32_t fill = spa_ringbuffer_get_write_index(&a->rb, &w);
    if (fill >= (int32_t)a->size) return 0;
    uint32_t space = a->size - (uint32_t)fill;
    if (n > space) n = space; if (!n) return 0;
    spa_ringbuffer_write_data(&a->rb, a->data, a->size, w % a->size, src, n);
    spa_ringbuffer_write_update(&a->rb, w + n);
    return n;
}

/*
 * Consumer (socket thread) with latency guard.
 * If backlog > latency_limit_bytes, skip forward to keep latency bounded.
 * ringbuffer.h:78
 */
static uint32_t rb_read(struct audio_ringbuffer *a, void *dst, uint32_t n) {
    uint32_t r, w;
    int32_t avail = spa_ringbuffer_get_read_index(&a->rb, &r);
    if (avail <= 0) return 0;

    /* latency guard: if consumer fell behind, skip old data */
    (void)spa_ringbuffer_get_write_index(&a->rb, &w);
    uint32_t lim = a->latency_limit_bytes ? a->latency_limit_bytes : (a->size / 4);
    if (w - r > lim + n) {
        uint32_t nr = w - lim;
        spa_ringbuffer_read_update(&a->rb, nr);
        r = nr; avail = (int32_t)(w - r);
        if (avail <= 0) return 0;
    }

    if (n > (uint32_t)avail) n = (uint32_t)avail;
    spa_ringbuffer_read_data(&a->rb, a->data, a->size, r % a->size, dst, n);
    spa_ringbuffer_read_update(&a->rb, r + n);
    return n;
}

/* Discard all when new client connects. */
static void rb_flush(struct audio_ringbuffer *a) {
    uint32_t w; (void)spa_ringbuffer_get_write_index(&a->rb, &w);
    spa_ringbuffer_read_update(&a->rb, w);
}

/* ================================================================= */
/*  Private implementation struct                                    */
/* ================================================================= */
struct impl {
    struct pw_context     *context;
    struct pw_properties  *props;

    struct pw_impl_module *module;
    struct spa_hook        module_listener;

    struct pw_core        *core;
    struct spa_hook        core_proxy_listener;
    struct spa_hook        core_listener;

    /* PipeWire audio stream (virtual sink) */
    struct pw_stream      *stream;
    struct spa_hook        stream_listener;
    struct spa_audio_info_raw  info;
    uint32_t               frame_size;

    unsigned int do_disconnect : 1;

    /* ---- socket server ---- */
    char                   socket_path[108];  /* max sun_path on Linux */
    pthread_t              socket_thread;
    atomic_bool            thread_running;
    atomic_bool            client_connected;
    int                    server_fd;

    /* ---- ringbuffer ---- */
    struct audio_ringbuffer  ringbuf;
};

/* ----------------------------------------------------------------- */
/*  Forward declaration                                              */
/* ----------------------------------------------------------------- */
static void impl_destroy(struct impl *impl);

/* ================================================================= */
/*  Socket helpers                                                   */
/* ================================================================= */
static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_block(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

/* Write all bytes, handling partial writes and EINTR. */
static int write_all(int fd, const void *buf, size_t count)
{
    const uint8_t *p = buf;
    size_t remain = count;
    while (remain > 0) {
        ssize_t n = write(fd, p, remain);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p      += (size_t)n;
        remain -= (size_t)n;
    }
    return 0;
}

/* ================================================================= */
/*  Socket server thread                                             */
/* ================================================================= */
static void *socket_thread_func(void *data)
{
    struct impl *impl = data;
    int client_fd = -1;

    pw_log_info("socket thread started on %s", impl->socket_path);

    while (atomic_load_explicit(&impl->thread_running,
                                 memory_order_acquire)) {

        /* ── Accept next client ── */
        if (client_fd < 0) {
            while (atomic_load_explicit(&impl->thread_running,
                                         memory_order_acquire)) {
                client_fd = accept(impl->server_fd, NULL, NULL);
                if (client_fd >= 0)
                    break;
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(100000);  /* 100 ms */
                    continue;
                }
                pw_log_error("accept: %m");
                goto exit_thread;
            }
            if (client_fd < 0)
                goto exit_thread;

            pw_log_info("client connected (fd=%d)", client_fd);
            set_block(client_fd);

            /* Flush ringbuffer: start streaming from "now". */
            rb_flush(&impl->ringbuf);

            atomic_store_explicit(&impl->client_connected, true,
                                   memory_order_release);

            /* Send format header: 3 × uint32_t, little-endian */
            uint32_t hdr[3] = {
                htole32(impl->info.rate),
                htole32(impl->info.channels),
                htole32((uint32_t)impl->info.format)
            };
            if (write_all(client_fd, hdr, sizeof(hdr)) < 0) {
                pw_log_warn("failed to send format header: %m");
                goto disconnect_client;
            }
            pw_log_info("sent format header: rate=%u ch=%u fmt=%u",
                         impl->info.rate, impl->info.channels,
                         (unsigned)impl->info.format);
        }

        /* ── Stream data ── */
        while (atomic_load_explicit(&impl->thread_running,
                                     memory_order_acquire)) {
            uint8_t buf[16384];
            uint32_t n = rb_read(&impl->ringbuf, buf, sizeof(buf));
            if (n > 0) {
                if (write_all(client_fd, buf, n) < 0) {
                    pw_log_info("write to client failed: %m");
                    goto disconnect_client;
                }
            } else {
                /* No data yet – avoid busy-wait */
                usleep(2000);  /* 2 ms */
            }
        }

disconnect_client:
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        client_fd = -1;
        atomic_store_explicit(&impl->client_connected, false,
                               memory_order_release);
        pw_log_info("client disconnected");
    }

exit_thread:
    if (client_fd >= 0) {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
    }
    pw_log_info("socket thread exiting");
    return NULL;
}

static int start_socket_server(struct impl *impl)
{
    struct sockaddr_un addr;
    int fd, rc;

    unlink(impl->socket_path);   /* remove stale socket */

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        pw_log_error("socket(): %m");
        return -errno;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, impl->socket_path, sizeof(addr.sun_path) - 1);

    rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        pw_log_error("bind(%s): %m", impl->socket_path);
        close(fd);
        return -errno;
    }

    chmod(impl->socket_path, 0666);

    rc = listen(fd, 1);
    if (rc < 0) {
        pw_log_error("listen(): %m");
        unlink(impl->socket_path);
        close(fd);
        return -errno;
    }

    set_nonblock(fd);     /* so accept() does not block the thread exit */
    impl->server_fd = fd;

    atomic_store_explicit(&impl->thread_running, true, memory_order_release);

    rc = pthread_create(&impl->socket_thread, NULL,
                         socket_thread_func, impl);
    if (rc != 0) {
        pw_log_error("pthread_create: %s", strerror(rc));
        atomic_store_explicit(&impl->thread_running, false,
                               memory_order_release);
        close(fd);
        unlink(impl->socket_path);
        return -rc;
    }

    pw_log_info("listening on %s", impl->socket_path);
    return 0;
}

static void stop_socket_server(struct impl *impl)
{
    atomic_store_explicit(&impl->thread_running, false,
                           memory_order_release);

    /* Wake up accept() by self-connecting briefly. */
    if (impl->server_fd >= 0) {
        int wk = socket(AF_UNIX, SOCK_STREAM, 0);
        if (wk >= 0) {
            struct sockaddr_un a;
            memset(&a, 0, sizeof(a));
            a.sun_family = AF_UNIX;
            strncpy(a.sun_path, impl->socket_path, sizeof(a.sun_path) - 1);
            connect(wk, (struct sockaddr *)&a, sizeof(a));
            close(wk);
        }
    }

    pthread_join(impl->socket_thread, NULL);

    if (impl->server_fd >= 0) {
        close(impl->server_fd);
        impl->server_fd = -1;
    }
    unlink(impl->socket_path);
    pw_log_info("socket server stopped");
}

/* ================================================================= */
/*  PipeWire stream callbacks                                        */
/* ================================================================= */
static void stream_destroy(void *d)
{
    struct impl *impl = d;
    spa_hook_remove(&impl->stream_listener);
    impl->stream = NULL;
}

static void stream_state_changed(void *d, enum pw_stream_state old,
                                  enum pw_stream_state state,
                                  const char *error)
{
    struct impl *impl = d;
    (void)old;

    switch (state) {
    case PW_STREAM_STATE_ERROR:
        pw_log_error("stream error: %s", error ? error : "unknown");
        pw_impl_module_schedule_destroy(impl->module);
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        pw_log_info("stream unconnected");
        pw_impl_module_schedule_destroy(impl->module);
        break;
    case PW_STREAM_STATE_STREAMING:
        pw_log_info("stream streaming: rate=%u ch=%u fmt=%d",
                     impl->info.rate, impl->info.channels,
                     (int)impl->info.format);
        break;
    default:
        break;
    }
}

/*
 * RT process callback.  PipeWire delivers audio buffers here.
 *
 * IMPORTANT: Runs in real-time context.  No blocking I/O, no heap alloc.
 */
static void stream_process(void *d)
{
    struct impl *impl = d;
    struct pw_buffer *buf;

    if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
        pw_log_debug("out of buffers: %m");
        return;
    }

    if (atomic_load_explicit(&impl->client_connected,
                              memory_order_acquire)) {
        struct spa_data *bd = &buf->buffer->datas[0];
        uint32_t offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
        uint32_t size = SPA_MIN(bd->chunk->size, bd->maxsize - offs);
        const void *data = SPA_PTROFF(bd->data, offs, void);

        if (size > 0)
            rb_write(&impl->ringbuf, data, size);
    }
    /* When no client: audio is silently dropped.  The virtual sink
     * continues to exist so PipeWire routing is unaffected. */

    pw_stream_queue_buffer(impl->stream, buf);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .destroy       = stream_destroy,
    .state_changed = stream_state_changed,
    .process       = stream_process,
};

/* ================================================================= */
/*  Create the audio stream                                          */
/* ================================================================= */
static int create_stream(struct impl *impl,
                          struct pw_properties *stream_props)
{
    uint8_t buffer[1024];
    struct spa_pod_builder b;
    const struct spa_pod *params[1];

    impl->stream = pw_stream_new(impl->core, "tiny audio sink",
                                  stream_props);
    /* stream_props ownership transferred to pw_stream */
    if (impl->stream == NULL)
        return -errno;

    pw_stream_add_listener(impl->stream, &impl->stream_listener,
                            &stream_events, impl);

    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
                                            &impl->info);

    return pw_stream_connect(impl->stream,
                              PW_DIRECTION_INPUT,    /* sink */
                              PW_ID_ANY,
                              PW_STREAM_FLAG_AUTOCONNECT |
                              PW_STREAM_FLAG_MAP_BUFFERS |
                              PW_STREAM_FLAG_RT_PROCESS,
                              params, 1);
}

/* ================================================================= */
/*  Core events                                                      */
/* ================================================================= */
static void core_error(void *data, uint32_t id, int seq, int res,
                        const char *message)
{
    struct impl *impl = data;
    pw_log_error("core error  id:%u seq:%d res:%d (%s): %s",
                  id, seq, res, spa_strerror(res), message);
    if (id == PW_ID_CORE && res == -EPIPE)
        pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = core_error,
};

static void core_proxy_destroy(void *d)
{
    struct impl *impl = d;
    spa_hook_remove(&impl->core_listener);
    impl->core = NULL;
    pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
    .destroy = core_proxy_destroy,
};

/* ================================================================= */
/*  Lifecycle                                                        */
/* ================================================================= */
static void impl_destroy(struct impl *impl)
{
    if (atomic_load_explicit(&impl->thread_running, memory_order_acquire))
        stop_socket_server(impl);

    if (impl->stream)
        pw_stream_destroy(impl->stream);

    if (impl->core && impl->do_disconnect)
        pw_core_disconnect(impl->core);

    rb_destroy(&impl->ringbuf);
    pw_properties_free(impl->props);
    free(impl);
}

static void module_destroy(void *data)
{
    struct impl *impl = data;
    spa_hook_remove(&impl->module_listener);
    impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
    PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = module_destroy,
};

/* ================================================================= */
/*  Helpers                                                          */
/* ================================================================= */
static int parse_audio_info(const struct pw_properties *props,
                             struct spa_audio_info_raw *info)
{
    return spa_audio_info_raw_init_dict_keys(info,
        &SPA_DICT_ITEMS(
            SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT,   DEFAULT_FORMAT),
            SPA_DICT_ITEM(SPA_KEY_AUDIO_RATE,     SPA_STRINGIFY(DEFAULT_RATE)),
            SPA_DICT_ITEM(SPA_KEY_AUDIO_CHANNELS, SPA_STRINGIFY(DEFAULT_CHANNELS)),
            SPA_DICT_ITEM(SPA_KEY_AUDIO_POSITION,  DEFAULT_POSITION)),
        &props->dict,
        SPA_KEY_AUDIO_FORMAT, SPA_KEY_AUDIO_RATE,
        SPA_KEY_AUDIO_CHANNELS, SPA_KEY_AUDIO_LAYOUT,
        SPA_KEY_AUDIO_POSITION, NULL);
}

static uint32_t calc_frame_size(const struct spa_audio_info_raw *info)
{
    uint32_t ch = info->channels;
    switch (info->format) {
    case SPA_AUDIO_FORMAT_U8:   case SPA_AUDIO_FORMAT_S8:
    case SPA_AUDIO_FORMAT_ALAW: case SPA_AUDIO_FORMAT_ULAW:
        return ch;
    case SPA_AUDIO_FORMAT_S16_LE: case SPA_AUDIO_FORMAT_S16_BE:
    case SPA_AUDIO_FORMAT_U16_LE: case SPA_AUDIO_FORMAT_U16_BE:
        return ch * 2;
    case SPA_AUDIO_FORMAT_S24_32_LE: case SPA_AUDIO_FORMAT_S24_32_BE:
    case SPA_AUDIO_FORMAT_S32_LE:    case SPA_AUDIO_FORMAT_S32_BE:
    case SPA_AUDIO_FORMAT_F32_LE:    case SPA_AUDIO_FORMAT_F32_BE:
        return ch * 4;
    case SPA_AUDIO_FORMAT_F64_LE: case SPA_AUDIO_FORMAT_F64_BE:
        return ch * 8;
    default:
        return 0;
    }
}

static void copy_props(struct pw_properties *dst,
                        const struct pw_properties *src, const char *key)
{
    const char *str;
    if ((str = pw_properties_get(src, key)) != NULL &&
        pw_properties_get(dst, key) == NULL)
        pw_properties_set(dst, key, str);
}

/* ================================================================= */
/*  Module entry point  (SPA_EXPORT → pipewire__module_init)         */
/* ================================================================= */
SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
    struct pw_context    *context = pw_impl_module_get_context(module);
    struct pw_properties *props = NULL;
    struct pw_properties *stream_props = NULL;
    uint32_t id  = pw_global_get_id(pw_impl_module_get_global(module));
    uint32_t pid = getpid();
    struct impl *impl;
    const char  *str;
    int          res;

    PW_LOG_TOPIC_INIT(mod_topic);

    impl = calloc(1, sizeof(*impl));
    if (!impl)
        return -errno;

    pw_log_debug("module %p: args=%s", impl, args ? args : "(null)");

    if (!args) args = "";

    /* ── Parse args ── */
    props = pw_properties_new_string(args);
    if (!props) { res = -errno; goto error; }
    impl->props = props;

    stream_props = pw_properties_new(NULL, NULL);
    if (!stream_props) { res = -errno; goto error; }

    impl->module  = module;
    impl->context = context;
    impl->server_fd = -1;

    /* socket.path */
    str = pw_properties_get(props, "socket.path");
    if (!str) str = DEFAULT_SOCKET_PATH;
    strncpy(impl->socket_path, str, sizeof(impl->socket_path) - 1);
    impl->socket_path[sizeof(impl->socket_path) - 1] = '\0';

    /* defaults */
    if (!pw_properties_get(props, PW_KEY_NODE_VIRTUAL))
        pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
    if (!pw_properties_get(props, PW_KEY_MEDIA_CLASS))
        pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
    if (!pw_properties_get(props, PW_KEY_NODE_NAME))
        pw_properties_setf(props, PW_KEY_NODE_NAME,
                            "tiny-audio-%u-%u", pid, id);
    if (!pw_properties_get(props, PW_KEY_NODE_DESCRIPTION))
        pw_properties_set(props, PW_KEY_NODE_DESCRIPTION,
                           pw_properties_get(props, PW_KEY_NODE_NAME));

    /* stream.props sub-dict */
    str = pw_properties_get(props, "stream.props");
    if (str)
        pw_properties_update_string(stream_props, str, strlen(str));

    /* Copy audio properties to stream */
    copy_props(stream_props, props, PW_KEY_AUDIO_RATE);
    copy_props(stream_props, props, PW_KEY_AUDIO_CHANNELS);
    copy_props(stream_props, props, SPA_KEY_AUDIO_LAYOUT);
    copy_props(stream_props, props, SPA_KEY_AUDIO_POSITION);
    copy_props(stream_props, props, PW_KEY_NODE_NAME);
    copy_props(stream_props, props, PW_KEY_NODE_DESCRIPTION);
    copy_props(stream_props, props, PW_KEY_NODE_GROUP);
    copy_props(stream_props, props, PW_KEY_NODE_LATENCY);
    copy_props(stream_props, props, PW_KEY_NODE_VIRTUAL);
    copy_props(stream_props, props, PW_KEY_MEDIA_CLASS);

    /* Parse audio format */
    res = parse_audio_info(stream_props, &impl->info);
    if (res < 0) {
        pw_log_error("bad audio format: %s", spa_strerror(res));
        goto error;
    }
    impl->frame_size = calc_frame_size(&impl->info);
    if (!impl->frame_size) {
        res = -EINVAL;
        pw_log_error("unsupported format: %d", (int)impl->info.format);
        goto error;
    }
    pw_log_info("audio: rate=%u ch=%u fmt=%d frame=%u",
                 impl->info.rate, impl->info.channels,
                 (int)impl->info.format, impl->frame_size);

    /* Allocate ringbuffer */
    {
        uint32_t rb_size = impl->frame_size * impl->info.rate
                           * RINGBUFFER_SECONDS;
        if (rb_size == 0 || rb_size > RINGBUFFER_MAX_BYTES)
            rb_size = RINGBUFFER_MAX_BYTES;
        rb_init(&impl->ringbuf, rb_size);
        /* latency guard: skip old data if backlog > 150ms */
        impl->ringbuf.latency_limit_bytes =
            impl->frame_size * impl->info.rate * LATENCY_LIMIT_MS / 1000u;
    }

    /* Get or connect core */
    impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
    if (!impl->core) {
        str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
        impl->core = pw_context_connect(context,
                        pw_properties_new(PW_KEY_REMOTE_NAME, str, NULL), 0);
        impl->do_disconnect = true;
    }
    if (!impl->core) { res = -errno; goto error; }

    pw_proxy_add_listener((struct pw_proxy *)impl->core,
                           &impl->core_proxy_listener,
                           &core_proxy_events, impl);
    pw_core_add_listener(impl->core, &impl->core_listener,
                          &core_events, impl);

    /* Create audio stream */
    res = create_stream(impl, stream_props);
    stream_props = NULL;  /* ownership transferred on success */
    if (res < 0) {
        pw_log_error("create_stream: %s", spa_strerror(res));
        goto error;
    }

    /* Start socket server */
    res = start_socket_server(impl);
    if (res < 0) {
        pw_log_error("socket server: %s", spa_strerror(res));
        goto error;
    }

    /* Register module */
    pw_impl_module_add_listener(module, &impl->module_listener,
                                 &module_events, impl);
    pw_impl_module_update_properties(module,
                                      &SPA_DICT_INIT_ARRAY(module_props));

    pw_log_info("module ready");
    return 0;

error:
    pw_properties_free(stream_props);
    impl_destroy(impl);
    return res;
}
