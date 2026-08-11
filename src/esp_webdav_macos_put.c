/*
 * Transport shim that makes macOS Finder uploads work.
 *
 * macOS's WebDAV client sends uploads like this:
 *
 *     PUT /index.html HTTP/1.1
 *     X-Expected-Entity-Length: 34423
 *     Transfer-Encoding: Chunked
 *     User-Agent: WebDAVFS/3.0.0 (03008000) Darwin/25.6.0 (arm64)
 *
 *     2000\r\n<body>...
 *
 * esp_http_server cannot deliver that body to a URI handler at all:
 * http_parser reports content_length as -1 for a chunked request, which
 * esp_http_server stores as 0, and httpd_req_recv() then clamps every read to
 * that 0. Reading the socket directly doesn't help either, because the header
 * parser has already pulled the front of the body into a private buffer.
 *
 * So fix it below esp_http_server instead, in a session recv override:
 *
 *   1. Hold back the request head until "\r\n\r\n" has arrived.
 *   2. Overwrite the "Transfer-Encoding: Chunked" line, in place and at
 *      exactly the same byte count, with "Content-Length: 0000034423" -- the
 *      value taken from X-Expected-Entity-Length and zero-padded to fit.
 *      Leading zeros are legal (RFC 7230 3.3.2 is 1*DIGIT), and keeping the
 *      length identical means no other offset shifts.
 *   3. De-chunk everything after the head, so the bytes esp_http_server reads
 *      are the decoded body and match the Content-Length we just wrote.
 *
 * From esp_http_server's point of view the request is then an ordinary
 * Content-Length PUT, and webdav_handle_put() needs no knowledge of any of
 * this.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_webdav_priv.h"

static const char *TAG = "webdav_macos";

/* Must hold a whole request head. esp_http_server caps requests at
 * CONFIG_HTTPD_MAX_REQ_HDR_LEN + CONFIG_HTTPD_MAX_URI_LEN; anything longer
 * than this falls back to passthrough and is rejected upstream as usual. */
#define SHIM_HEAD_MAX 2048
#define SHIM_RAW_MAX  1024

/* Only PUT can carry a chunked upload, so every other request skips the
 * buffering entirely. */
#define SHIM_SNIFF "PUT "

typedef enum { PH_SNIFF, PH_HEAD, PH_PASS, PH_BODY } phase_t;
typedef enum { CH_SIZE, CH_DATA, CH_CRLF, CH_TRAILER, CH_DONE } chunk_state_t;

typedef struct {
    int    fd;
    phase_t phase;

    char   head[SHIM_HEAD_MAX];
    size_t head_len;   /* bytes accumulated (and, once ready, rewritten) */
    size_t head_out;   /* bytes of head already handed to esp_http_server */

    char   raw[SHIM_RAW_MAX]; /* undecoded bytes read from the socket */
    size_t raw_len;
    size_t raw_off;

    chunk_state_t cs;
    char   sizeline[40];
    size_t sizeline_len;
    long   chunk_left; /* bytes left in the current chunk */
    long   body_left;  /* bytes left of the declared entity length */
} shim_t;

/* Small linear table rather than fd-modulo slots: lwip fds are dense but not
 * guaranteed to stay inside any fixed window, and a collision would splice two
 * connections together. */
#define SHIM_MAX 12
static shim_t *s_tab[SHIM_MAX];

static shim_t *shim_find(int fd)
{
    for (int i = 0; i < SHIM_MAX; i++) {
        if (s_tab[i] && s_tab[i]->fd == fd) {
            return s_tab[i];
        }
    }
    return NULL;
}

static shim_t *shim_get(int fd)
{
    shim_t *s = shim_find(fd);
    if (s) {
        return s;
    }
    for (int i = 0; i < SHIM_MAX; i++) {
        if (!s_tab[i]) {
            s = calloc(1, sizeof(*s));
            if (!s) {
                return NULL;
            }
            s->fd = fd;
            s->phase = PH_SNIFF;
            s_tab[i] = s;
            return s;
        }
    }
    return NULL; /* table full -- caller falls back to plain recv */
}

void webdav_macos_put_forget(int fd)
{
    for (int i = 0; i < SHIM_MAX; i++) {
        if (s_tab[i] && s_tab[i]->fd == fd) {
            free(s_tab[i]);
            s_tab[i] = NULL;
            return;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Head rewriting                                                          */
/* ---------------------------------------------------------------------- */

/* memmem() is a GNU extension, so spell the search out. Returns a pointer to
 * the "\r\n\r\n" that ends the head, or NULL if it isn't there yet. */
static char *find_blank_line(char *buf, size_t len)
{
    if (len < 4) {
        return NULL;
    }
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

/* Case-insensitive search for a header line start within the head. Returns a
 * pointer to the first character of the field name, or NULL. */
static char *find_header(char *head, size_t len, const char *name)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen < len; i++) {
        /* Header lines start at the beginning of the buffer or after a CRLF. */
        if (i != 0 && !(head[i - 1] == '\n')) {
            continue;
        }
        if (strncasecmp(head + i, name, nlen) == 0 && head[i + nlen] == ':') {
            return head + i;
        }
    }
    return NULL;
}

static const char *header_value(const char *line)
{
    const char *v = strchr(line, ':');
    if (!v) {
        return NULL;
    }
    v++;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    return v;
}

static size_t line_length(const char *line, const char *end)
{
    const char *p = line;
    while (p < end && *p != '\r' && *p != '\n') {
        p++;
    }
    return (size_t)(p - line);
}

/*
 * Rewrite a macOS-style chunked PUT head in place. Returns the declared entity
 * length on success, or -1 to leave the request untouched.
 */
static long rewrite_head(shim_t *s)
{
    char *end = s->head + s->head_len;

    char *te = find_header(s->head, s->head_len, "Transfer-Encoding");
    if (!te) {
        return -1;
    }
    const char *te_val = header_value(te);
    if (!te_val || strncasecmp(te_val, "chunked", 7) != 0) {
        return -1; /* some other transfer coding -- not ours to touch */
    }

    char *xl = find_header(s->head, s->head_len, "X-Expected-Entity-Length");
    if (!xl) {
        return -1;
    }
    const char *xl_val = header_value(xl);
    if (!xl_val) {
        return -1;
    }
    long n = strtol(xl_val, NULL, 10);
    if (n < 0) {
        return -1;
    }

    /* A Content-Length alongside chunked would be ambiguous; don't guess. */
    if (find_header(s->head, s->head_len, "Content-Length")) {
        return -1;
    }

    /* Overwrite the Transfer-Encoding line with an equal-length
     * "Content-Length: <zero-padded>", so no following byte moves. */
    size_t line_len = line_length(te, end);
    const char *label = "Content-Length: ";
    size_t label_len = strlen(label);
    if (line_len < label_len + 1) {
        return -1; /* no room for even one digit */
    }
    size_t digits = line_len - label_len;

    char num[32];
    int written = snprintf(num, sizeof(num), "%ld", n);
    if (written < 0 || (size_t)written > digits || digits >= sizeof(num)) {
        return -1;
    }
    memcpy(te, label, label_len);
    memset(te + label_len, '0', digits);
    memcpy(te + label_len + digits - (size_t)written, num, (size_t)written);

    ESP_LOGI(TAG, "fd=%d rewrote chunked PUT as Content-Length: %ld", s->fd, n);
    return n;
}

/* ---------------------------------------------------------------------- */
/* De-chunking                                                             */
/* ---------------------------------------------------------------------- */

/* Decode from s->raw into dst (at most cap bytes). Returns bytes produced. */
static size_t dechunk(shim_t *s, char *dst, size_t cap)
{
    size_t out = 0;

    while (s->raw_off < s->raw_len && out < cap && s->cs != CH_DONE) {
        char c = s->raw[s->raw_off];

        switch (s->cs) {
        case CH_SIZE:
            s->raw_off++;
            if (c == '\n') {
                s->sizeline[s->sizeline_len] = '\0';
                char *semi = strchr(s->sizeline, ';'); /* chunk extensions */
                if (semi) {
                    *semi = '\0';
                }
                char *endp = NULL;
                long sz = strtol(s->sizeline, &endp, 16);
                s->sizeline_len = 0;
                if (endp == s->sizeline || sz < 0) {
                    ESP_LOGW(TAG, "fd=%d malformed chunk size", s->fd);
                    s->cs = CH_DONE;
                    break;
                }
                if (sz == 0) {
                    s->cs = CH_TRAILER;
                } else {
                    s->chunk_left = sz;
                    s->cs = CH_DATA;
                }
            } else if (c != '\r' && s->sizeline_len + 1 < sizeof(s->sizeline)) {
                s->sizeline[s->sizeline_len++] = c;
            }
            break;

        case CH_DATA: {
            size_t avail = s->raw_len - s->raw_off;
            size_t want = cap - out;
            if (avail > (size_t)s->chunk_left) {
                avail = (size_t)s->chunk_left;
            }
            if (avail > want) {
                avail = want;
            }
            /* Never hand up more than the entity length we advertised. */
            if (s->body_left >= 0 && avail > (size_t)s->body_left) {
                avail = (size_t)s->body_left;
            }
            memcpy(dst + out, s->raw + s->raw_off, avail);
            out += avail;
            s->raw_off += avail;
            s->chunk_left -= (long)avail;
            if (s->body_left >= 0) {
                s->body_left -= (long)avail;
            }
            if (s->chunk_left == 0) {
                s->cs = CH_CRLF;
            }
            break;
        }

        case CH_CRLF:
            s->raw_off++;
            if (c == '\n') {
                s->cs = CH_SIZE;
            }
            break;

        case CH_TRAILER:
            /* Zero or more trailer lines, ending at a blank line. Nothing here
             * is representable on a plain filesystem, so discard it. */
            s->raw_off++;
            if (c == '\n') {
                if (s->sizeline_len == 0) {
                    s->cs = CH_DONE;
                } else {
                    s->sizeline_len = 0;
                }
            } else if (c != '\r') {
                s->sizeline_len = 1;
            }
            break;

        case CH_DONE:
            break;
        }
    }

    if (s->raw_off >= s->raw_len) {
        s->raw_off = s->raw_len = 0;
    }
    return out;
}

/* ---------------------------------------------------------------------- */
/* recv override                                                           */
/* ---------------------------------------------------------------------- */

static int map_recv_error(void)
{
    /* Mirror esp_http_server's own errno mapping, or its timeout handling
     * breaks once we take over recv. */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    if (errno == EINVAL || errno == EBADF || errno == EFAULT || errno == ENOTSOCK) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    return HTTPD_SOCK_ERR_FAIL;
}

static int fill_raw(shim_t *s, size_t want, int fd, int flags)
{
    if (s->raw_len > s->raw_off) {
        return (int)(s->raw_len - s->raw_off);
    }
    s->raw_len = s->raw_off = 0;
    if (want > SHIM_RAW_MAX) {
        want = SHIM_RAW_MAX;
    }
    int n = recv(fd, s->raw, want, flags);
    if (n <= 0) {
        return n;
    }
    s->raw_len = (size_t)n;
    return n;
}

int webdav_macos_put_recv(httpd_handle_t hd, int sockfd, char *buf, size_t buf_len, int flags)
{
    (void)hd;
    shim_t *s = shim_get(sockfd);
    if (!s || buf_len == 0) {
        int n = recv(sockfd, buf, buf_len, flags); /* table full: stay transparent */
        return (n < 0) ? map_recv_error() : n;
    }

    while (true) {
        /* 1. Deliver any rewritten head still owed to esp_http_server. */
        if (s->head_out < s->head_len) {
            size_t n = s->head_len - s->head_out;
            if (n > buf_len) {
                n = buf_len;
            }
            memcpy(buf, s->head + s->head_out, n);
            s->head_out += n;
            return (int)n;
        }

        /* 2. Body of a rewritten request: de-chunk it. */
        if (s->phase == PH_BODY) {
            size_t produced = dechunk(s, buf, buf_len);
            if (produced > 0) {
                return (int)produced;
            }
            if (s->cs == CH_DONE) {
                /* Body finished; the next bytes are a fresh request. */
                s->phase = PH_SNIFF;
                s->head_len = s->head_out = 0;
                continue;
            }
            int n = fill_raw(s, buf_len, sockfd, flags);
            if (n <= 0) {
                return (n < 0) ? map_recv_error() : n;
            }
            continue;
        }

        /* 3. Plain passthrough. */
        if (s->phase == PH_PASS) {
            if (s->raw_len > s->raw_off) {
                size_t n = s->raw_len - s->raw_off;
                if (n > buf_len) {
                    n = buf_len;
                }
                memcpy(buf, s->raw + s->raw_off, n);
                s->raw_off += n;
                if (s->raw_off >= s->raw_len) {
                    s->raw_off = s->raw_len = 0;
                }
                return (int)n;
            }
            int n = recv(sockfd, buf, buf_len, flags);
            return (n < 0) ? map_recv_error() : n;
        }

        /* 4. Accumulate the head (PH_SNIFF / PH_HEAD). */
        int n = fill_raw(s, SHIM_RAW_MAX, sockfd, flags);
        if (n <= 0) {
            return (n < 0) ? map_recv_error() : n;
        }

        /* Always drain raw into the head buffer first. Sniffing straight out
         * of raw would deadlock: fill_raw() refuses to read more while raw
         * still holds unconsumed bytes, so a read too short to decide on
         * (a 1-byte read, say) would spin forever. */
        size_t avail = s->raw_len - s->raw_off;
        size_t room = SHIM_HEAD_MAX - s->head_len;
        size_t take = avail < room ? avail : room;
        memcpy(s->head + s->head_len, s->raw + s->raw_off, take);
        s->head_len += take;
        s->raw_off += take;
        if (s->raw_off >= s->raw_len) {
            s->raw_off = s->raw_len = 0;
        }

        if (s->phase == PH_SNIFF) {
            /* Only a PUT can be a macOS chunked upload; everything else skips
             * the buffering. The bytes gathered so far are already in head,
             * and step 1 hands them over untouched. */
            size_t sniff_len = strlen(SHIM_SNIFF);
            if (s->head_len < sniff_len) {
                continue; /* not enough to decide yet */
            }
            if (memcmp(s->head, SHIM_SNIFF, sniff_len) != 0) {
                s->phase = PH_PASS;
                s->head_out = 0;
                continue;
            }
            s->phase = PH_HEAD;
        }

        char *hdr_end = find_blank_line(s->head, s->head_len);
        if (!hdr_end) {
            if (s->head_len >= SHIM_HEAD_MAX) {
                /* Too long to be one of ours; hand it over untouched and let
                 * esp_http_server apply its own limits. */
                s->phase = PH_PASS;
                s->head_out = 0;
            }
            continue;
        }

        size_t head_bytes = (size_t)(hdr_end - s->head) + 4;
        /* Anything past the blank line is body; hand it back for de-chunking.
         * raw is necessarily empty here -- the copy above consumed all of it,
         * and the only path that leaves data behind is the head-full case,
         * which switches to passthrough instead. extra is therefore bounded by
         * one read, i.e. at most SHIM_RAW_MAX. */
        size_t extra = s->head_len - head_bytes;
        if (extra > 0 && extra <= SHIM_RAW_MAX) {
            memcpy(s->raw, s->head + head_bytes, extra);
            s->raw_off = 0;
            s->raw_len = extra;
        }
        s->head_len = head_bytes;

        long n_body = rewrite_head(s);
        if (n_body < 0) {
            s->phase = PH_PASS; /* ordinary request: emit the head unchanged */
        } else {
            s->phase = PH_BODY;
            s->cs = CH_SIZE;
            s->sizeline_len = 0;
            s->chunk_left = 0;
            s->body_left = n_body;
        }
        s->head_out = 0;
        /* Loop round: step 1 now delivers the head. */
    }
}
