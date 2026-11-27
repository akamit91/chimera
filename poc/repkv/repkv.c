/*
 * repkv: proof-of-concept key-value backend that keeps every record in memory
 * and copies each put and delete to one peer node over TCP.
 *
 * It is a key-value-only VFS backend (CHIMERA_VFS_CAP_KV), loaded by path.
 * Records are opaque: the module never interprets a key or a value.
 *
 * Threads:
 *   - every VFS thread holds one outbound connection to the peer, on which it
 *     sends its own puts and deletes and receives acknowledgements;
 *   - one server thread owns the listener and applies what the peer sends.
 *
 * Modes:
 *   off    nothing is sent to the peer
 *   async  the operation completes locally, the copy is sent without waiting
 *   sync   the operation completes when the peer has acknowledged the copy
 *
 * With no peer connection an operation completes locally and is counted as
 * unreplicated.  A node that starts while its peer is up pulls a full copy
 * before it serves anything.  There is no catch-up after a broken connection.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <jansson.h>

#include <evpl/evpl.h>

#include "vfs/sdk/chimera_vfs_sdk.h"
#include "common/export.h"
#include "common/logging.h"
#include "common/macros.h"

#ifndef container_of
#define container_of(ptr, type, member) \
        ((type *) ((char *) (ptr) - offsetof(type, member)))
#endif /* ifndef container_of */

#define repkv_info(...)  chimera_info("repkv", __FILE__, __LINE__, __VA_ARGS__)
#define repkv_error(...) chimera_error("repkv", __FILE__, __LINE__, __VA_ARGS__)
#define repkv_abort_if(cond, ...) \
        chimera_abort_if(cond, "repkv", __FILE__, __LINE__, __VA_ARGS__)

#define REPKV_NUM_SHARDS     4096
#define REPKV_MAGIC          0x564b5052u /* "RPKV" */
#define REPKV_MAX_KEY        65536u
#define REPKV_MAX_VALUE      (64u * 1024u * 1024u)
#define REPKV_RECONNECT_US   1000000ull
#define REPKV_STATS_US       10000000ull
#define REPKV_NUM_TYPES      16

enum repkv_op {
    REPKV_OP_PUT       = 1,
    REPKV_OP_DEL       = 2,
    REPKV_OP_ACK       = 3,
    REPKV_OP_SYNC      = 4,
    REPKV_OP_SYNC_DONE = 5,
};

#define REPKV_FLAG_WANT_ACK  0x01
/* The put carries a version: the receiver keeps the stored value when its
 * version is newer (32-bit serial number comparison). */
#define REPKV_FLAG_VERSIONED 0x02

enum repkv_mode {
    REPKV_MODE_OFF   = 0,
    REPKV_MODE_ASYNC = 1,
    REPKV_MODE_SYNC  = 2,
};

/* Both nodes are the same build on the same architecture, so the header goes
 * on the wire in host byte order. */
struct repkv_hdr {
    uint32_t magic;
    uint8_t  op;
    uint8_t  flags;
    uint16_t pad;
    uint64_t seq;
    uint32_t key_len;
    uint32_t val_len;
    uint32_t version;
    uint32_t pad2;
};

struct repkv_entry {
    struct repkv_entry *next;
    uint64_t            hash;
    uint32_t            key_len;
    uint32_t            value_len;
    uint32_t            version;
    uint8_t             versioned;
    uint8_t            *key;
    uint8_t            *value;
};

struct repkv_shard {
    pthread_mutex_t     lock;
    struct repkv_entry *head;
};

struct repkv_stats {
    atomic_uint_fast64_t puts[REPKV_NUM_TYPES];
    atomic_uint_fast64_t dels[REPKV_NUM_TYPES];
    atomic_uint_fast64_t gets;
    atomic_uint_fast64_t searches;
    atomic_uint_fast64_t sent_sync;
    atomic_uint_fast64_t sent_async;
    atomic_uint_fast64_t unreplicated;
    atomic_uint_fast64_t ack_count;
    atomic_uint_fast64_t ack_ns_sum;
    atomic_uint_fast64_t ack_ns_max;
    atomic_uint_fast64_t tx_bytes;
    atomic_uint_fast64_t rx_puts;
    atomic_uint_fast64_t rx_dels;
    atomic_uint_fast64_t dropped_pending;
    atomic_uint_fast64_t stale_local;
    atomic_uint_fast64_t stale_from_peer;
};

struct repkv_shared {
    struct repkv_shard   *shards;
    enum repkv_mode       mode;
    char                  listen_host[128];
    int                   listen_port;
    char                  peer_host[128];
    int                   peer_port;
    struct evpl_endpoint *peer_ep;
    struct evpl_endpoint *listen_ep;
    struct evpl_listener *listener;
    struct evpl_thread   *server_thread;
    struct repkv_stats    stats;
    atomic_int            out_connected;
};

struct repkv_pending {
    struct repkv_pending       *next;
    uint64_t                    seq;
    uint64_t                    sent_ns;
    struct chimera_vfs_request *request;
};

struct repkv_thread {
    struct repkv_shared  *shared;
    struct evpl          *evpl;
    struct evpl_bind     *bind;
    int                   connected;
    int                   timer_armed;
    int                   shutting_down;
    struct evpl_timer     timer;
    uint64_t              next_seq;
    struct repkv_pending *pend_head;
    struct repkv_pending *pend_tail;
    struct repkv_pending *pend_free;
    uint8_t              *txbuf;
    uint32_t              txcap;
};

/* Server thread state and one inbound connection. */
struct repkv_server {
    struct repkv_shared          *shared;
    struct evpl                  *evpl;
    struct evpl_listener_binding *binding;
    struct evpl_timer             stats_timer;
    uint64_t                      last_total;
    uint8_t                      *txbuf;
    uint32_t                      txcap;
};

struct repkv_conn {
    struct repkv_server *server;
    uint8_t             *rxbuf;
    uint32_t             rxcap;
};

static inline uint64_t
repkv_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
} /* repkv_now_ns */

/* Record type of a chimera state key ([0xC4, version, type, ...]); 0 for any
 * other key.  Used only to break the counters down. */
static inline unsigned int
repkv_key_type(
    const uint8_t *key,
    uint32_t       key_len)
{
    if (key_len >= 3 && key[0] == 0xC4 && key[2] < REPKV_NUM_TYPES) {
        return key[2];
    }
    return 0;
} /* repkv_key_type */

/* ------------------------------------------------------------------ *
*  local store                                                       *
* ------------------------------------------------------------------ */

static inline struct repkv_shard *
repkv_shard_for(
    struct repkv_shared *shared,
    uint64_t             hash)
{
    return &shared->shards[hash % REPKV_NUM_SHARDS];
} /* repkv_shard_for */

static struct repkv_entry *
repkv_find_locked(
    struct repkv_shard *shard,
    uint64_t            hash,
    const void         *key,
    uint32_t            key_len,
    struct repkv_entry ***linkp)
{
    struct repkv_entry **link = &shard->head;
    struct repkv_entry  *e;

    for (e = shard->head; e; link = &e->next, e = e->next) {
        if (e->hash == hash && e->key_len == key_len &&
            memcmp(e->key, key, key_len) == 0) {
            if (linkp) {
                *linkp = link;
            }
            return e;
        }
    }
    return NULL;
} /* repkv_find_locked */

#define REPKV_PUT_STALE (-1)

/* Returns 0 when the value was stored, CHIMERA_VFS_EEXIST when if_absent is
 * set and the key exists, and REPKV_PUT_STALE when the put is versioned and
 * the stored version is newer: the stored value is then left alone. */
static int
repkv_store_put(
    struct repkv_shared *shared,
    const void          *key,
    uint32_t             key_len,
    const void          *value,
    uint32_t             value_len,
    int                  if_absent,
    int                  versioned,
    uint32_t             version)
{
    uint64_t            hash  = chimera_vfs_hash(key, key_len);
    struct repkv_shard *shard = repkv_shard_for(shared, hash);
    struct repkv_entry *e;
    uint8_t            *nv    = malloc(value_len ? value_len : 1);

    memcpy(nv, value, value_len);

    pthread_mutex_lock(&shard->lock);
    e = repkv_find_locked(shard, hash, key, key_len, NULL);
    if (e) {
        if (if_absent) {
            pthread_mutex_unlock(&shard->lock);
            free(nv);
            return CHIMERA_VFS_EEXIST;
        }
        if (versioned && e->versioned &&
            (int32_t) (version - e->version) < 0) {
            pthread_mutex_unlock(&shard->lock);
            free(nv);
            return REPKV_PUT_STALE;
        }
        free(e->value);
        e->value     = nv;
        e->value_len = value_len;
    } else {
        e            = malloc(sizeof(*e));
        e->hash      = hash;
        e->key_len   = key_len;
        e->value_len = value_len;
        e->key       = malloc(key_len ? key_len : 1);
        memcpy(e->key, key, key_len);
        e->value    = nv;
        e->next     = shard->head;
        shard->head = e;
    }
    e->versioned = versioned ? 1 : 0;
    e->version   = version;
    pthread_mutex_unlock(&shard->lock);
    return 0;
} /* repkv_store_put */

/* Returns 0, or CHIMERA_VFS_ENOENT. */
static int
repkv_store_del(
    struct repkv_shared *shared,
    const void          *key,
    uint32_t             key_len)
{
    uint64_t             hash  = chimera_vfs_hash(key, key_len);
    struct repkv_shard  *shard = repkv_shard_for(shared, hash);
    struct repkv_entry  *e;
    struct repkv_entry **link;

    pthread_mutex_lock(&shard->lock);
    e = repkv_find_locked(shard, hash, key, key_len, &link);
    if (!e) {
        pthread_mutex_unlock(&shard->lock);
        return CHIMERA_VFS_ENOENT;
    }
    *link = e->next;
    pthread_mutex_unlock(&shard->lock);

    free(e->key);
    free(e->value);
    free(e);
    return 0;
} /* repkv_store_del */

/* ------------------------------------------------------------------ *
*  frames                                                            *
* ------------------------------------------------------------------ */

static uint32_t
repkv_frame_build(
    uint8_t   **bufp,
    uint32_t   *capp,
    uint8_t     op,
    uint8_t     flags,
    uint64_t    seq,
    uint32_t    version,
    const void *key,
    uint32_t    key_len,
    const void *value,
    uint32_t    value_len)
{
    uint32_t         total = sizeof(struct repkv_hdr) + key_len + value_len;
    struct repkv_hdr hdr;

    if (total > *capp) {
        *capp = total < 4096 ? 4096 : total * 2;
        *bufp = realloc(*bufp, *capp);
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic   = REPKV_MAGIC;
    hdr.op      = op;
    hdr.flags   = flags;
    hdr.seq     = seq;
    hdr.version = version;
    hdr.key_len = key_len;
    hdr.val_len = value_len;

    memcpy(*bufp, &hdr, sizeof(hdr));
    if (key_len) {
        memcpy(*bufp + sizeof(hdr), key, key_len);
    }
    if (value_len) {
        memcpy(*bufp + sizeof(hdr) + key_len, value, value_len);
    }
    return total;
} /* repkv_frame_build */

/* ------------------------------------------------------------------ *
*  outbound side: one connection per VFS thread                      *
* ------------------------------------------------------------------ */

static void repkv_out_connect(
    struct repkv_thread *thread);

static void
repkv_thread_free(struct repkv_thread *thread)
{
    struct repkv_pending *p;

    while ((p = thread->pend_free)) {
        thread->pend_free = p->next;
        free(p);
    }
    while ((p = thread->pend_head)) {
        thread->pend_head = p->next;
        free(p);
    }
    free(thread->txbuf);
    free(thread);
} /* repkv_thread_free */

static void
repkv_out_timer(
    struct evpl       *evpl,
    struct evpl_timer *timer)
{
    struct repkv_thread *thread = container_of(timer, struct repkv_thread, timer);

    (void) evpl;
    thread->timer_armed = 0;
    if (!thread->bind) {
        repkv_out_connect(thread);
    }
} /* repkv_out_timer */

static void
repkv_out_retry_later(struct repkv_thread *thread)
{
    if (!thread->timer_armed) {
        thread->timer_armed = 1;
        evpl_add_oneshot_timer(thread->evpl, &thread->timer, repkv_out_timer,
                               REPKV_RECONNECT_US);
    }
} /* repkv_out_retry_later */

/* The peer is gone: nothing waits for it any more.  The local copy is already
 * in place, so the operations complete successfully without a replica. */
static void
repkv_out_flush_pending(struct repkv_thread *thread)
{
    struct repkv_pending       *p;
    struct chimera_vfs_request *request;

    while ((p = thread->pend_head)) {
        thread->pend_head = p->next;
        if (!thread->pend_head) {
            thread->pend_tail = NULL;
        }
        request      = p->request;
        p->next      = thread->pend_free;
        thread->pend_free = p;
        atomic_fetch_add(&thread->shared->stats.dropped_pending, 1);
        request->status = CHIMERA_VFS_OK;
        request->complete(request);
    }
} /* repkv_out_flush_pending */

static void
repkv_out_notify(
    struct evpl        *evpl,
    struct evpl_bind   *bind,
    struct evpl_notify *notify,
    void               *private_data)
{
    struct repkv_thread        *thread = private_data;
    struct repkv_shared        *shared = thread->shared;
    struct repkv_hdr            hdr;
    struct repkv_pending       *p;
    struct chimera_vfs_request *request;
    uint64_t                    ns, prev;

    switch (notify->notify_type) {
        case EVPL_NOTIFY_CONNECTED:
            thread->connected = 1;
            atomic_fetch_add(&shared->out_connected, 1);
            break;

        case EVPL_NOTIFY_RECV_DATA:
            while (evpl_recv(evpl, bind, &hdr, sizeof(hdr),
                             EVPL_RECV_FLAG_ALL_OR_NONE) == (int) sizeof(hdr)) {
                if (hdr.magic != REPKV_MAGIC || hdr.op != REPKV_OP_ACK) {
                    repkv_error("unexpected frame on outbound connection");
                    evpl_close(evpl, bind);
                    return;
                }
                p = thread->pend_head;
                if (!p || p->seq != hdr.seq) {
                    repkv_error("acknowledgement %llu does not match the oldest pending operation",
                                (unsigned long long) hdr.seq);
                    continue;
                }
                thread->pend_head = p->next;
                if (!thread->pend_head) {
                    thread->pend_tail = NULL;
                }
                ns = repkv_now_ns() - p->sent_ns;
                atomic_fetch_add(&shared->stats.ack_count, 1);
                atomic_fetch_add(&shared->stats.ack_ns_sum, ns);
                prev = atomic_load(&shared->stats.ack_ns_max);
                while (ns > prev &&
                       !atomic_compare_exchange_weak(&shared->stats.ack_ns_max, &prev, ns)) {
                }
                request           = p->request;
                p->next           = thread->pend_free;
                thread->pend_free = p;
                request->status   = CHIMERA_VFS_OK;
                request->complete(request);
            }
            break;

        case EVPL_NOTIFY_DISCONNECTED:
            if (thread->connected) {
                thread->connected = 0;
                atomic_fetch_sub(&shared->out_connected, 1);
            }
            thread->bind = NULL;
            if (thread->shutting_down) {
                /* The VFS thread is gone; this was its last reference. */
                repkv_thread_free(thread);
                return;
            }
            repkv_out_flush_pending(thread);
            repkv_out_retry_later(thread);
            break;

        default:
            break;
    } /* switch */
} /* repkv_out_notify */

static void
repkv_out_connect(struct repkv_thread *thread)
{
    struct repkv_shared *shared = thread->shared;

    thread->bind = evpl_connect(thread->evpl, EVPL_STREAM_SOCKET_TCP, NULL,
                                shared->peer_ep, repkv_out_notify, NULL, thread);
    if (!thread->bind) {
        repkv_out_retry_later(thread);
    }
} /* repkv_out_connect */

/* Send one put or delete to the peer.  Returns 1 when the request now waits
 * for the acknowledgement, 0 when the caller completes it. */
static int
repkv_replicate(
    struct repkv_thread        *thread,
    struct chimera_vfs_request *request,
    uint8_t                     op,
    int                         versioned,
    uint32_t                    version,
    const void                 *key,
    uint32_t                    key_len,
    const void                 *value,
    uint32_t                    value_len)
{
    struct repkv_shared  *shared = thread->shared;
    struct repkv_pending *p;
    uint32_t              total;
    uint64_t              seq;
    int                   sync = shared->mode == REPKV_MODE_SYNC;

    if (shared->mode == REPKV_MODE_OFF) {
        return 0;
    }
    if (!thread->connected || !thread->bind) {
        atomic_fetch_add(&shared->stats.unreplicated, 1);
        return 0;
    }

    seq   = ++thread->next_seq;
    total = repkv_frame_build(&thread->txbuf, &thread->txcap, op,
                              (sync ? REPKV_FLAG_WANT_ACK : 0) |
                              (versioned ? REPKV_FLAG_VERSIONED : 0),
                              seq, version,
                              key, key_len, value, value_len);

    if (sync) {
        p = thread->pend_free;
        if (p) {
            thread->pend_free = p->next;
        } else {
            p = malloc(sizeof(*p));
        }
        p->next    = NULL;
        p->seq     = seq;
        p->request = request;
        p->sent_ns = repkv_now_ns();
        if (thread->pend_tail) {
            thread->pend_tail->next = p;
        } else {
            thread->pend_head = p;
        }
        thread->pend_tail = p;
        atomic_fetch_add(&shared->stats.sent_sync, 1);
    } else {
        atomic_fetch_add(&shared->stats.sent_async, 1);
    }
    atomic_fetch_add(&shared->stats.tx_bytes, total);

    evpl_send(thread->evpl, thread->bind, thread->txbuf, total);
    return sync;
} /* repkv_replicate */

/* ------------------------------------------------------------------ *
*  inbound side: the server thread                                   *
* ------------------------------------------------------------------ */

static void
repkv_server_send(
    struct repkv_server *server,
    struct evpl_bind    *bind,
    uint8_t              op,
    uint8_t              flags,
    uint64_t             seq,
    uint32_t             version,
    const void          *key,
    uint32_t             key_len,
    const void          *value,
    uint32_t             value_len)
{
    uint32_t total = repkv_frame_build(&server->txbuf, &server->txcap, op, flags,
                                       seq, version, key, key_len, value, value_len);

    evpl_send(server->evpl, bind, server->txbuf, total);
} /* repkv_server_send */

static void
repkv_server_dump(
    struct repkv_server *server,
    struct evpl_bind    *bind)
{
    struct repkv_shared *shared = server->shared;
    struct repkv_shard  *shard;
    struct repkv_entry  *e;
    uint64_t             n = 0;
    int                  i;

    for (i = 0; i < REPKV_NUM_SHARDS; i++) {
        shard = &shared->shards[i];
        pthread_mutex_lock(&shard->lock);
        for (e = shard->head; e; e = e->next) {
            repkv_server_send(server, bind, REPKV_OP_PUT,
                              e->versioned ? REPKV_FLAG_VERSIONED : 0, 0, e->version,
                              e->key, e->key_len, e->value, e->value_len);
            n++;
        }
        pthread_mutex_unlock(&shard->lock);
    }
    repkv_server_send(server, bind, REPKV_OP_SYNC_DONE, 0, n, 0, NULL, 0, NULL, 0);
    repkv_info("sent a full copy to the peer: %llu records", (unsigned long long) n);
} /* repkv_server_dump */

static void
repkv_server_notify(
    struct evpl        *evpl,
    struct evpl_bind   *bind,
    struct evpl_notify *notify,
    void               *private_data)
{
    struct repkv_conn   *conn   = private_data;
    struct repkv_server *server = conn->server;
    struct repkv_shared *shared = server->shared;
    struct repkv_hdr     hdr;
    uint32_t             total;
    const uint8_t       *key, *value;

    switch (notify->notify_type) {
        case EVPL_NOTIFY_RECV_DATA:
            for (;;) {
                if (evpl_peek(evpl, bind, &hdr, sizeof(hdr)) < (int) sizeof(hdr)) {
                    break;
                }
                if (hdr.magic != REPKV_MAGIC || hdr.key_len > REPKV_MAX_KEY ||
                    hdr.val_len > REPKV_MAX_VALUE) {
                    repkv_error("bad frame from the peer; closing the connection");
                    evpl_close(evpl, bind);
                    return;
                }
                total = sizeof(hdr) + hdr.key_len + hdr.val_len;
                if (total > conn->rxcap) {
                    conn->rxcap = total < 4096 ? 4096 : total * 2;
                    conn->rxbuf = realloc(conn->rxbuf, conn->rxcap);
                }
                if (evpl_recv(evpl, bind, conn->rxbuf, total,
                              EVPL_RECV_FLAG_ALL_OR_NONE) != (int) total) {
                    break;
                }
                key   = conn->rxbuf + sizeof(hdr);
                value = key + hdr.key_len;

                switch (hdr.op) {
                    case REPKV_OP_PUT:
                        if (repkv_store_put(shared, key, hdr.key_len, value, hdr.val_len, 0,
                                            hdr.flags & REPKV_FLAG_VERSIONED,
                                            hdr.version) == REPKV_PUT_STALE) {
                            atomic_fetch_add(&shared->stats.stale_from_peer, 1);
                        }
                        atomic_fetch_add(&shared->stats.rx_puts, 1);
                        break;
                    case REPKV_OP_DEL:
                        repkv_store_del(shared, key, hdr.key_len);
                        atomic_fetch_add(&shared->stats.rx_dels, 1);
                        break;
                    case REPKV_OP_SYNC:
                        repkv_server_dump(server, bind);
                        break;
                    default:
                        repkv_error("unknown operation %u from the peer", hdr.op);
                        break;
                } /* switch */

                if (hdr.flags & REPKV_FLAG_WANT_ACK) {
                    repkv_server_send(server, bind, REPKV_OP_ACK, 0, hdr.seq, 0, NULL, 0, NULL, 0);
                }
            }
            break;

        case EVPL_NOTIFY_DISCONNECTED:
            free(conn->rxbuf);
            free(conn);
            break;

        default:
            break;
    } /* switch */
} /* repkv_server_notify */

static void
repkv_server_accept(
    struct evpl             *evpl,
    struct evpl_bind        *bind,
    evpl_notify_callback_t  *notify_callback,
    evpl_segment_callback_t *segment_callback,
    void                   **conn_private_data,
    void                    *private_data)
{
    struct repkv_server *server = private_data;
    struct repkv_conn   *conn   = calloc(1, sizeof(*conn));

    (void) evpl;
    (void) bind;
    conn->server       = server;
    *notify_callback   = repkv_server_notify;
    *segment_callback  = NULL;
    *conn_private_data = conn;
} /* repkv_server_accept */

static void
repkv_stats_timer(
    struct evpl       *evpl,
    struct evpl_timer *timer)
{
    struct repkv_server *server = container_of(timer, struct repkv_server, stats_timer);
    struct repkv_stats  *s      = &server->shared->stats;
    uint64_t             puts   = 0, dels = 0, total, acks, sum;
    char                 line[512];
    int                  i, off = 0;

    (void) evpl;

    for (i = 0; i < REPKV_NUM_TYPES; i++) {
        uint64_t p = atomic_load(&s->puts[i]);
        uint64_t d = atomic_load(&s->dels[i]);

        puts += p;
        dels += d;
        if ((p || d) && off < (int) sizeof(line) - 48) {
            off += snprintf(line + off, sizeof(line) - off, " t%02x=%llu/%llu", i,
                            (unsigned long long) p, (unsigned long long) d);
        }
    }
    line[off] = '\0';

    total = puts + dels + atomic_load(&s->rx_puts) + atomic_load(&s->rx_dels);
    if (total == server->last_total) {
        return;
    }
    server->last_total = total;

    acks = atomic_load(&s->ack_count);
    sum  = atomic_load(&s->ack_ns_sum);

    repkv_info("stats: put=%llu del=%llu get=%llu search=%llu by-type(put/del):%s | "
               "sent sync=%llu async=%llu unreplicated=%llu tx_bytes=%llu | "
               "ack n=%llu mean_us=%.1f max_us=%.1f flushed=%llu | "
               "from-peer put=%llu del=%llu | stale local=%llu from-peer=%llu | links=%d",
               (unsigned long long) puts, (unsigned long long) dels,
               (unsigned long long) atomic_load(&s->gets),
               (unsigned long long) atomic_load(&s->searches), line,
               (unsigned long long) atomic_load(&s->sent_sync),
               (unsigned long long) atomic_load(&s->sent_async),
               (unsigned long long) atomic_load(&s->unreplicated),
               (unsigned long long) atomic_load(&s->tx_bytes),
               (unsigned long long) acks,
               acks ? (double) sum / (double) acks / 1000.0 : 0.0,
               (double) atomic_load(&s->ack_ns_max) / 1000.0,
               (unsigned long long) atomic_load(&s->dropped_pending),
               (unsigned long long) atomic_load(&s->rx_puts),
               (unsigned long long) atomic_load(&s->rx_dels),
               (unsigned long long) atomic_load(&s->stale_local),
               (unsigned long long) atomic_load(&s->stale_from_peer),
               atomic_load(&server->shared->out_connected));
} /* repkv_stats_timer */

static void *
repkv_server_thread_init(
    struct evpl *evpl,
    void        *private_data)
{
    struct repkv_shared *shared = private_data;
    struct repkv_server *server = calloc(1, sizeof(*server));

    server->shared  = shared;
    server->evpl    = evpl;
    server->binding = evpl_listener_attach(evpl, shared->listener,
                                           repkv_server_accept, server);
    evpl_add_timer(evpl, &server->stats_timer, repkv_stats_timer, REPKV_STATS_US);
    return server;
} /* repkv_server_thread_init */

static void
repkv_server_thread_shutdown(
    struct evpl *evpl,
    void        *private_data)
{
    struct repkv_server *server = private_data;

    evpl_remove_timer(evpl, &server->stats_timer);
    evpl_listener_detach(evpl, server->binding);
    free(server->txbuf);
    free(server);
} /* repkv_server_thread_shutdown */

/* ------------------------------------------------------------------ *
*  start-up copy from the peer (plain blocking sockets)              *
* ------------------------------------------------------------------ */

static int
repkv_read_full(
    int    fd,
    void  *buf,
    size_t len)
{
    size_t  done = 0;
    ssize_t n;

    while (done < len) {
        n = read(fd, (char *) buf + done, len - done);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t) n;
    }
    return 0;
} /* repkv_read_full */

static void
repkv_pull_from_peer(struct repkv_shared *shared)
{
    struct addrinfo  hints, *res = NULL;
    struct repkv_hdr hdr;
    struct timeval   tv   = { .tv_sec = 10, .tv_usec = 0 };
    char             port[16];
    uint8_t         *buf  = NULL;
    uint32_t         cap  = 0, len;
    uint64_t         n    = 0;
    int              fd   = -1, done = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", shared->peer_port);

    if (getaddrinfo(shared->peer_host, port, &hints, &res) != 0 || !res) {
        repkv_error("cannot resolve peer %s", shared->peer_host);
        return;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        repkv_info("peer %s:%d is not up; starting with an empty store",
                   shared->peer_host, shared->peer_port);
        goto out;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = REPKV_MAGIC;
    hdr.op    = REPKV_OP_SYNC;
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t) sizeof(hdr)) {
        repkv_error("could not ask the peer for a full copy");
        goto out;
    }

    while (!done) {
        if (repkv_read_full(fd, &hdr, sizeof(hdr)) != 0 || hdr.magic != REPKV_MAGIC ||
            hdr.key_len > REPKV_MAX_KEY || hdr.val_len > REPKV_MAX_VALUE) {
            repkv_abort_if(1, "full copy from the peer broke off after %llu records",
                           (unsigned long long) n);
        }
        len = hdr.key_len + hdr.val_len;
        if (len > cap) {
            cap = len < 4096 ? 4096 : len * 2;
            buf = realloc(buf, cap);
        }
        if (len && repkv_read_full(fd, buf, len) != 0) {
            repkv_abort_if(1, "full copy from the peer broke off after %llu records",
                           (unsigned long long) n);
        }
        switch (hdr.op) {
            case REPKV_OP_PUT:
                repkv_store_put(shared, buf, hdr.key_len, buf + hdr.key_len, hdr.val_len, 0,
                                hdr.flags & REPKV_FLAG_VERSIONED, hdr.version);
                n++;
                break;
            case REPKV_OP_SYNC_DONE:
                done = 1;
                break;
            default:
                break;
        } /* switch */
    }
    repkv_info("pulled a full copy from peer %s:%d: %llu records",
               shared->peer_host, shared->peer_port, (unsigned long long) n);

 out:
    if (fd >= 0) {
        close(fd);
    }
    free(buf);
    freeaddrinfo(res);
} /* repkv_pull_from_peer */

/* ------------------------------------------------------------------ *
*  module entry points                                               *
* ------------------------------------------------------------------ */

static void *
repkv_init(
    const char                *cfgdata,
    struct prometheus_metrics *metrics)
{
    struct repkv_shared *shared = calloc(1, sizeof(*shared));
    json_error_t         json_error;
    json_t              *cfg, *v;
    const char          *mode = "off";
    int                  i;

    (void) metrics;

    snprintf(shared->listen_host, sizeof(shared->listen_host), "0.0.0.0");

    if (cfgdata && cfgdata[0] != '\0') {
        cfg = json_loads(cfgdata, 0, &json_error);
        repkv_abort_if(!cfg, "Failed to parse repkv config: %s", json_error.text);

        if (json_is_string(v = json_object_get(cfg, "mode"))) {
            mode = json_string_value(v);
        }
        if (json_is_string(v = json_object_get(cfg, "listen_host"))) {
            snprintf(shared->listen_host, sizeof(shared->listen_host), "%s", json_string_value(v));
        }
        if (json_is_integer(v = json_object_get(cfg, "listen_port"))) {
            shared->listen_port = (int) json_integer_value(v);
        }
        if (json_is_string(v = json_object_get(cfg, "peer_host"))) {
            snprintf(shared->peer_host, sizeof(shared->peer_host), "%s", json_string_value(v));
        }
        if (json_is_integer(v = json_object_get(cfg, "peer_port"))) {
            shared->peer_port = (int) json_integer_value(v);
        }

        if (strcmp(mode, "sync") == 0) {
            shared->mode = REPKV_MODE_SYNC;
        } else if (strcmp(mode, "async") == 0) {
            shared->mode = REPKV_MODE_ASYNC;
        } else {
            shared->mode = REPKV_MODE_OFF;
        }
        json_decref(cfg);
    }

    if (!shared->peer_host[0] || shared->peer_port <= 0) {
        shared->mode = REPKV_MODE_OFF;
    }

    shared->shards = calloc(REPKV_NUM_SHARDS, sizeof(*shared->shards));
    for (i = 0; i < REPKV_NUM_SHARDS; i++) {
        pthread_mutex_init(&shared->shards[i].lock, NULL);
    }

    repkv_info("mode=%s listen=%s:%d peer=%s:%d",
               shared->mode == REPKV_MODE_SYNC ? "sync" :
               shared->mode == REPKV_MODE_ASYNC ? "async" : "off",
               shared->listen_host, shared->listen_port,
               shared->peer_host, shared->peer_port);

    if (shared->mode != REPKV_MODE_OFF) {
        shared->peer_ep = evpl_endpoint_create(shared->peer_host, shared->peer_port);
        /* Before anything reads the store: take what the peer already holds. */
        repkv_pull_from_peer(shared);
    }

    if (shared->listen_port > 0) {
        shared->listen_ep = evpl_endpoint_create(shared->listen_host, shared->listen_port);
        shared->listener  = evpl_listener_create();
        shared->server_thread = evpl_thread_create(NULL, repkv_server_thread_init,
                                                   repkv_server_thread_shutdown, shared);
        repkv_abort_if(!shared->server_thread, "could not start the repkv server thread");
        repkv_abort_if(evpl_listen(shared->listener, EVPL_STREAM_SOCKET_TCP, shared->listen_ep),
                       "repkv cannot listen on %s:%d", shared->listen_host, shared->listen_port);
    }

    return shared;
} /* repkv_init */

static void
repkv_destroy(void *private_data)
{
    struct repkv_shared *shared = private_data;
    struct repkv_entry  *e, *next;
    int                  i;

    if (shared->server_thread) {
        evpl_thread_destroy(shared->server_thread);
    }
    if (shared->listener) {
        evpl_listener_destroy(shared->listener);
    }
    for (i = 0; i < REPKV_NUM_SHARDS; i++) {
        for (e = shared->shards[i].head; e; e = next) {
            next = e->next;
            free(e->key);
            free(e->value);
            free(e);
        }
        pthread_mutex_destroy(&shared->shards[i].lock);
    }
    free(shared->shards);
    free(shared);
} /* repkv_destroy */

static void *
repkv_thread_init(
    struct evpl *evpl,
    void        *private_data)
{
    struct repkv_shared *shared = private_data;
    struct repkv_thread *thread = calloc(1, sizeof(*thread));

    thread->shared = shared;
    thread->evpl   = evpl;

    if (shared->mode != REPKV_MODE_OFF) {
        repkv_out_connect(thread);
    }
    return thread;
} /* repkv_thread_init */

static void
repkv_thread_destroy(void *private_data)
{
    struct repkv_thread *thread = private_data;

    thread->shutting_down = 1;

    if (thread->timer_armed) {
        evpl_remove_timer(thread->evpl, &thread->timer);
        thread->timer_armed = 0;
    }

    /* An open connection still delivers its disconnect notification after
     * this returns, so the state is freed there. */
    if (thread->bind) {
        evpl_close(thread->evpl, thread->bind);
        return;
    }
    repkv_thread_free(thread);
} /* repkv_thread_destroy */

static void
repkv_put_key(
    struct repkv_thread        *thread,
    struct chimera_vfs_request *request)
{
    struct repkv_shared *shared    = thread->shared;
    int                  versioned = (request->put_key.flags & CHIMERA_VFS_PUT_KEY_IF_NEWER) != 0;
    int                  rc;

    rc = repkv_store_put(shared, request->put_key.key, request->put_key.key_len,
                         request->put_key.value, request->put_key.value_len,
                         request->put_key.flags & CHIMERA_VFS_PUT_KEY_IF_ABSENT,
                         versioned, request->put_key.version);
    if (rc == REPKV_PUT_STALE) {
        /* A newer value is already stored and was already sent to the peer. */
        atomic_fetch_add(&shared->stats.stale_local, 1);
        request->status = CHIMERA_VFS_OK;
        request->complete(request);
        return;
    }
    if (rc) {
        request->status = rc;
        request->complete(request);
        return;
    }

    atomic_fetch_add(&shared->stats.puts[repkv_key_type(request->put_key.key,
                                                        request->put_key.key_len)], 1);

    if (repkv_replicate(thread, request, REPKV_OP_PUT, versioned, request->put_key.version,
                        request->put_key.key, request->put_key.key_len,
                        request->put_key.value, request->put_key.value_len)) {
        return;
    }
    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* repkv_put_key */

static void
repkv_delete_key(
    struct repkv_thread        *thread,
    struct chimera_vfs_request *request)
{
    struct repkv_shared *shared = thread->shared;
    int                  rc;

    rc = repkv_store_del(shared, request->delete_key.key, request->delete_key.key_len);
    if (rc) {
        request->status = rc;
        request->complete(request);
        return;
    }

    atomic_fetch_add(&shared->stats.dels[repkv_key_type(request->delete_key.key,
                                                        request->delete_key.key_len)], 1);

    if (repkv_replicate(thread, request, REPKV_OP_DEL, 0, 0,
                        request->delete_key.key, request->delete_key.key_len, NULL, 0)) {
        return;
    }
    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* repkv_delete_key */

static void
repkv_get_key(
    struct repkv_thread        *thread,
    struct chimera_vfs_request *request)
{
    struct repkv_shared *shared = thread->shared;
    uint64_t             hash   = chimera_vfs_hash(request->get_key.key, request->get_key.key_len);
    struct repkv_shard  *shard  = repkv_shard_for(shared, hash);
    struct repkv_entry  *e;
    void                *copy;

    atomic_fetch_add(&shared->stats.gets, 1);

    pthread_mutex_lock(&shard->lock);
    e = repkv_find_locked(shard, hash, request->get_key.key, request->get_key.key_len, NULL);
    if (!e) {
        pthread_mutex_unlock(&shard->lock);
        request->status = CHIMERA_VFS_ENOENT;
        request->complete(request);
        return;
    }

    /* The peer's copy can replace this value at any moment, so the caller gets
     * its own copy in request memory. */
    if (e->value_len <= CHIMERA_VFS_PLUGIN_DATA_SIZE) {
        copy = request->plugin_data;
    } else {
        copy = chimera_vfs_request_alloc_memory(request, e->value_len);
    }
    if (!copy) {
        pthread_mutex_unlock(&shard->lock);
        request->status = CHIMERA_VFS_EIO;
        request->complete(request);
        return;
    }
    memcpy(copy, e->value, e->value_len);
    request->get_key.r_value     = copy;
    request->get_key.r_value_len = e->value_len;
    pthread_mutex_unlock(&shard->lock);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* repkv_get_key */

static int
repkv_key_in_range(
    const void *key,
    uint32_t    key_len,
    const void *start_key,
    uint32_t    start_key_len,
    const void *end_key,
    uint32_t    end_key_len,
    uint32_t    flags)
{
    int cmp;

    if (start_key_len > 0) {
        cmp = memcmp(key, start_key, key_len < start_key_len ? key_len : start_key_len);
        if (cmp < 0 || (cmp == 0 && key_len < start_key_len)) {
            return 0;
        }
    }
    if (end_key_len > 0) {
        cmp = memcmp(key, end_key, key_len < end_key_len ? key_len : end_key_len);
        if (cmp > 0 || (cmp == 0 && key_len > end_key_len)) {
            return 0;
        }
        if ((flags & CHIMERA_VFS_SEARCH_KEYS_END_EXCLUSIVE) &&
            cmp == 0 && key_len == end_key_len) {
            return 0;
        }
    }
    return 1;
} /* repkv_key_in_range */

struct repkv_search_item {
    uint8_t *key;
    uint32_t key_len;
    uint8_t *value;
    uint32_t value_len;
};

static int
repkv_search_item_cmp(
    const void *a,
    const void *b)
{
    const struct repkv_search_item *ia  = a;
    const struct repkv_search_item *ib  = b;
    uint32_t                        min = ia->key_len < ib->key_len ? ia->key_len : ib->key_len;
    int                             cmp = memcmp(ia->key, ib->key, min);

    if (cmp) {
        return cmp;
    }
    return (ia->key_len > ib->key_len) - (ia->key_len < ib->key_len);
} /* repkv_search_item_cmp */

static void
repkv_search_keys(
    struct repkv_thread        *thread,
    struct chimera_vfs_request *request)
{
    struct repkv_shared               *shared   = thread->shared;
    chimera_vfs_search_keys_callback_t callback = request->search_keys.callback;
    struct repkv_search_item          *items    = NULL, *item;
    struct repkv_shard                *shard;
    struct repkv_entry                *e;
    size_t                             n = 0, cap = 0, k;
    int                                i;

    atomic_fetch_add(&shared->stats.searches, 1);

    /* Collect the matches under the shard locks, then sort by key and call
     * back with no lock held. */
    for (i = 0; i < REPKV_NUM_SHARDS; i++) {
        shard = &shared->shards[i];
        pthread_mutex_lock(&shard->lock);
        for (e = shard->head; e; e = e->next) {
            if (!repkv_key_in_range(e->key, e->key_len,
                                    request->search_keys.start_key,
                                    request->search_keys.start_key_len,
                                    request->search_keys.end_key,
                                    request->search_keys.end_key_len,
                                    request->search_keys.flags)) {
                continue;
            }
            if (n == cap) {
                cap   = cap ? cap * 2 : 16;
                items = realloc(items, cap * sizeof(*items));
            }
            item            = &items[n++];
            item->key_len   = e->key_len;
            item->value_len = e->value_len;
            item->key       = malloc(e->key_len ? e->key_len : 1);
            item->value     = malloc(e->value_len ? e->value_len : 1);
            memcpy(item->key, e->key, e->key_len);
            memcpy(item->value, e->value, e->value_len);
        }
        pthread_mutex_unlock(&shard->lock);
    }

    if (n > 1) {
        qsort(items, n, sizeof(*items), repkv_search_item_cmp);
    }

    for (k = 0; k < n; k++) {
        if (callback(items[k].key, items[k].key_len,
                     items[k].value, items[k].value_len,
                     request->proto_private_data)) {
            break;
        }
    }

    for (k = 0; k < n; k++) {
        free(items[k].key);
        free(items[k].value);
    }
    free(items);

    request->status = CHIMERA_VFS_OK;
    request->complete(request);
} /* repkv_search_keys */

static void
repkv_dispatch(
    struct chimera_vfs_request *request,
    void                       *private_data)
{
    struct repkv_thread *thread = private_data;

    switch (request->opcode) {
        case CHIMERA_VFS_OP_PUT_KEY:
            repkv_put_key(thread, request);
            break;
        case CHIMERA_VFS_OP_GET_KEY:
            repkv_get_key(thread, request);
            break;
        case CHIMERA_VFS_OP_DELETE_KEY:
            repkv_delete_key(thread, request);
            break;
        case CHIMERA_VFS_OP_SEARCH_KEYS:
            repkv_search_keys(thread, request);
            break;
        default:
            request->status = CHIMERA_VFS_ENOTSUP;
            request->complete(request);
            break;
    } /* switch */
} /* repkv_dispatch */

SYMBOL_EXPORT struct chimera_vfs_module vfs_repkv = {
    .sdk_version    = CHIMERA_VFS_SDK_VERSION,
    .name           = "repkv",
    .fh_magic       = CHIMERA_VFS_FH_MAGIC_VENDOR0,
    .capabilities   = CHIMERA_VFS_CAP_KV,
    .init           = repkv_init,
    .destroy        = repkv_destroy,
    .thread_init    = repkv_thread_init,
    .thread_destroy = repkv_thread_destroy,
    .dispatch       = repkv_dispatch,
};
