#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <chash.h>
#include <fbuf.h>
#include <rbuf.h>
#include <linklist.h>
#include <iomux.h>

#include "connections.h"
#include "messaging.h"
#include "connections_pool.h"
#include "shardcache_client.h"

#ifdef BSD
#  define random() arc4random()
#endif

typedef struct chash_t chash_t;

struct shardcache_client_s {
    chash_t *chash;
    shardcache_node_t **shards;
    connections_pool_t *connections;
    int num_shards;
    const char *auth;
    int use_random_node;
    shardcache_node_t *current_node;
    int errno;
    char errstr[1024];
};

int
shardcache_client_tcp_timeout(shardcache_client_t *c, int new_value)
{
    return connections_pool_tcp_timeout(c->connections, new_value);
}

int shardcache_client_use_random_node(shardcache_client_t *c, int new_value)
{
    int old_value = c->use_random_node;
    c->use_random_node = new_value;
    return old_value;
}

shardcache_client_t *
shardcache_client_create(shardcache_node_t **nodes, int num_nodes, char *auth)
{
    int i;
    if (!num_nodes) {
        fprintf(stderr, "Can't create a shardcache client with no nodes\n");
        return NULL;
    }
    shardcache_client_t *c = calloc(1, sizeof(shardcache_client_t));
    size_t shard_lens[num_nodes];
    char *shard_names[num_nodes];

    c->shards = malloc(sizeof(shardcache_node_t *) * num_nodes);
    c->connections = connections_pool_create(SHARDCACHE_TCP_TIMEOUT_DEFAULT, 1);
    for (i = 0; i < num_nodes; i++) {
        c->shards[i] = shardcache_node_copy(nodes[i]);

        shard_names[i] = shardcache_node_get_label(c->shards[i]);
        shard_lens[i] = strlen(shard_names[i]);
    }

    c->num_shards = num_nodes;

    c->chash = chash_create((const char **)shard_names, shard_lens, c->num_shards, 200);

    if (auth && *auth) {
        c->auth = calloc(1, 16);
        strncpy((char *)c->auth, auth, 16);
    }

    return c;
}

static inline char *
select_node(shardcache_client_t *c, void *key, size_t klen, int *fd)
{
    const char *node_name;
    size_t name_len = 0;
    int i;
    char *addr = NULL;
    shardcache_node_t *node = NULL;

    if (c->num_shards == 1) {
        node = c->shards[0];
    } else if (c->use_random_node) {
        node = c->current_node;
        if (!node) {
            node = c->shards[random()%c->num_shards];
            c->current_node = node;
        }
    } else {
        chash_lookup(c->chash, key, klen, &node_name, &name_len);

        for (i = 0; i < c->num_shards; i++) {
            if (strncmp(node_name, shardcache_node_get_label(c->shards[i]), name_len) == 0) {
                node = c->shards[i];
                break;
            }
        }
    }

    if (node)
        addr = shardcache_node_get_address(node);
        if (fd) { 
            int retries = 3;
            do {
                *fd = connections_pool_get(c->connections, addr);
                if (*fd < 0 && c->use_random_node && c->num_shards > 1) {
                    shardcache_node_t *prev = node;
                    do {
                        node = c->shards[random()%c->num_shards];
                    } while (node == prev);
                    c->current_node = node;
                    addr = shardcache_node_get_address(node);
                }
            } while (*fd < 0 && retries--);

        }

    return addr;
}

size_t
shardcache_client_get(shardcache_client_t *c, void *key, size_t klen, void **data)
{
    int fd = -1;
    char *node = select_node(c, key, klen, &fd);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", node);
        return 0;
    }

    fbuf_t value = FBUF_STATIC_INITIALIZER;
    int rc = fetch_from_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, &value, fd);
    if (rc == 0) {
        size_t size = fbuf_used(&value);
        if (data)
            *data = fbuf_data(&value);
        else
            fbuf_destroy(&value);

        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;

        connections_pool_add(c->connections, node, fd);
        return size;
    } else {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't fetch data from node '%s'", node);
        return 0;
    }
    return 0;
}

size_t
shardcache_client_offset(shardcache_client_t *c, void *key, size_t klen, uint32_t offset, void *data, uint32_t dlen)
{
    int fd = -1;
    char *node = select_node(c, key, klen, &fd);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", node);
        return 0;
    }

    fbuf_t value = FBUF_STATIC_INITIALIZER;
    int rc = offset_from_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, offset, dlen, &value, fd);
    if (rc == 0) {
        uint32_t to_copy = dlen > fbuf_used(&value) ? fbuf_used(&value) : dlen;
        if (data)
            memcpy(data, fbuf_data(&value), to_copy);

        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;

        connections_pool_add(c->connections, node, fd);
        fbuf_destroy(&value);
        return to_copy;
    } else {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't fetch data from node '%s'", node);
    }
    fbuf_destroy(&value);
    return 0;
}

int
shardcache_client_exists(shardcache_client_t *c, void *key, size_t klen)
{
    int fd = -1;
    char *node = select_node(c, key, klen, &fd);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", node);
        return -1;
    }
    int rc = exists_on_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, fd, 1);
    if (rc == -1) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr),
                "Can't check existance of data on node '%s'", node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

int
shardcache_client_touch(shardcache_client_t *c, void *key, size_t klen)
{
    int fd = -1;
    char *node = select_node(c, key, klen, &fd);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", node);
        return -1;
    }
    int rc = touch_on_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, fd);
    if (rc == -1) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr),
                 "Can't touch key '%s' on node '%s'", (char *)key, node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

static inline int
shardcache_client_set_internal(shardcache_client_t *c, void *key, size_t klen, void *data, size_t dlen, uint32_t expire, int inx)
{
    int fd = -1;
    char *node = select_node(c, key, klen, &fd);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", node);
        return -1;
    }

    int rc = -1;
    if (inx)
        rc = add_to_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, data, dlen, expire, fd, 1);
    else
        rc = send_to_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, data, dlen, expire, fd, 1);

    if (rc == -1) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't set new data on node '%s'", node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

int
shardcache_client_set(shardcache_client_t *c, void *key, size_t klen, void *data, size_t dlen, uint32_t expire)
{
     return shardcache_client_set_internal(c, key, klen, data, dlen, expire, 0);
}

int
shardcache_client_add(shardcache_client_t *c, void *key, size_t klen, void *data, size_t dlen, uint32_t expire)
{
     return shardcache_client_set_internal(c, key, klen, data, dlen, expire, 1);
}

int
shardcache_client_del(shardcache_client_t *c, void *key, size_t klen)
{
    int fd = -1;
    char *node = select_node(c, key, klen, &fd);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", node);
        return -1;
    }
    int rc = delete_from_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, fd, 1);
    if (rc != 0) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't delete data from node '%s'", node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

int
shardcache_client_evict(shardcache_client_t *c, void *key, size_t klen)
{
    int fd = -1;
    char *node = select_node(c, key, klen, &fd);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", node);
        return -1;
    }

    int rc = evict_from_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, fd, 1);
    if (rc != 0) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't evict data from node '%s'", node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }

    return rc;
}

static inline shardcache_node_t *
shardcache_get_node(shardcache_client_t *c, char *node_name)
{
    shardcache_node_t *node = NULL;

    int i;
    for (i = 0; i < c->num_shards; i++) {
        if (strcmp(node_name, shardcache_node_get_label(c->shards[i])) == 0) {
            node = c->shards[i];
            break;
        }
    }

    if (!node) {
        c->errno = SHARDCACHE_CLIENT_ERROR_ARGS;
        snprintf(c->errstr, sizeof(c->errstr), "Unknown node '%s'", node_name);
        return NULL;
    }

    return node;
}

int
shardcache_client_stats(shardcache_client_t *c, char *node_name, char **buf, size_t *len)
{

    shardcache_node_t *node = shardcache_get_node(c, node_name);
    if (!node)
        return -1;

    char *addr = shardcache_node_get_address(node);
    int fd = connections_pool_get(c->connections, addr);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", addr);
        return -1;
    }

    int rc = stats_from_peer(addr, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, buf, len, fd);
    if (rc != 0) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr),
                "Can't get stats from node '%s'", shardcache_node_get_label(node));
    } else {
        connections_pool_add(c->connections, addr, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }

    return rc;
}

int
shardcache_client_check(shardcache_client_t *c, char *node_name) {
    shardcache_node_t *node = shardcache_get_node(c, node_name);
    if (!node)
        return -1;

    char *addr = shardcache_node_get_address(node);
    int fd = connections_pool_get(c->connections, addr);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", addr);
        return -1;
    }

    int rc = check_peer(addr, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, fd);
    if (rc != 0) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr),
                "Can't check node '%s'", shardcache_node_get_label(node));
    } else {
        connections_pool_add(c->connections, addr, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

void
shardcache_client_destroy(shardcache_client_t *c)
{
    chash_free(c->chash);
    shardcache_free_nodes(c->shards, c->num_shards);
    if (c->auth)
        free((void *)c->auth);
    connections_pool_destroy(c->connections);
    free(c);
}

int
shardcache_client_errno(shardcache_client_t *c)
{
    return c->errno;
}

char *
shardcache_client_errstr(shardcache_client_t *c)
{
    return c->errstr;
}

shardcache_storage_index_t *
shardcache_client_index(shardcache_client_t *c, char *node_name)
{
    shardcache_node_t *node = shardcache_get_node(c, node_name);
    if (!node)
        return NULL;

    char *addr = shardcache_node_get_address(node);
    int fd = connections_pool_get(c->connections, addr);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", addr);
        return NULL;
    }

    shardcache_storage_index_t *index = index_from_peer(addr, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, fd);
    if (!index) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr),
                "Can't get index from node '%s'", shardcache_node_get_label(node));
    } else {
        connections_pool_add(c->connections, addr, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }

    return index;
}

int
shardcache_client_migration_begin(shardcache_client_t *c, shardcache_node_t **nodes, int num_nodes)
{
    fbuf_t mgb_message = FBUF_STATIC_INITIALIZER;

    int i;
    for (i = 0; i < num_nodes; i++) {
        if (i > 0) 
            fbuf_add(&mgb_message, ",");
        fbuf_printf(&mgb_message, "%s:%s", shardcache_node_get_string(nodes[i]));
    }

    for (i = 0; i < c->num_shards; i++) {
        char *addr = shardcache_node_get_address(c->shards[i]);
        int fd = connections_pool_get(c->connections, addr);
        if (fd < 0) {
            c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
            snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", addr);
            fbuf_destroy(&mgb_message);
            return -1;
        }

        int rc = migrate_peer(addr,
                              (char *)c->auth,
                              SHC_HDR_SIGNATURE_SIP,
                              fbuf_data(&mgb_message),
                              fbuf_used(&mgb_message), fd);
        if (rc != 0) {
            close(fd);
            c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
            snprintf(c->errstr, sizeof(c->errstr), "Node '%s' (%s) didn't aknowledge the migration\n",
                    shardcache_node_get_label(c->shards[i]), addr);
            fbuf_destroy(&mgb_message);
            // XXX - should we abort migration on peers that have been notified (if any)? 
            return -1;
        }
        connections_pool_add(c->connections, addr, fd);
    }
    fbuf_destroy(&mgb_message);

    c->errno = SHARDCACHE_CLIENT_OK;
    c->errstr[0] = 0;

    return 0;
}

int
shardcache_client_migration_abort(shardcache_client_t *c)
{
    int i;
    for (i = 0; i < c->num_shards; i++) {
        char *addr = shardcache_node_get_address(c->shards[i]);
        char *label = shardcache_node_get_label(c->shards[i]);

        int fd = connections_pool_get(c->connections, addr);
        if (fd < 0) {
            c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
            snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", addr);
            return -1;
        }

        int rc =  abort_migrate_peer(label, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, fd);

        if (rc != 0) {
            close(fd);
            c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
            snprintf(c->errstr, sizeof(c->errstr),
                     "Can't abort migration from node '%s'", label);
            return -1;
        }
        connections_pool_add(c->connections, addr, fd);
    }
 
    c->errno = SHARDCACHE_CLIENT_OK;
    c->errstr[0] = 0;

    return 0;
}

int
shardcache_client_get_async(shardcache_client_t *c,
                            void *key,
                            size_t klen,
                            shardcache_client_get_aync_data_cb data_cb,
                            void *priv)
{
    int fd = -1;
    char *node = select_node(c, key, klen, &fd);
    if (fd < 0) {
        c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
        snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", node);
        return -1;
    }

    return fetch_from_peer_async(node, (char *)c->auth, SHC_HDR_CSIGNATURE_SIP, key, klen, 0, 0, data_cb, priv, fd, NULL);
}

shc_multi_item_t *
shc_multi_item_create(void  *key,
                      size_t klen,
                      void  *data,
                      size_t dlen)
{
    shc_multi_item_t *item = calloc(1, sizeof(shc_multi_item_t));
    item->key = malloc(klen);
    memcpy(item->key, key, klen);
    item->klen = klen;

    if (dlen) {
        item->data = malloc(dlen);
        memcpy(item->data, data, dlen);
        item->dlen = dlen;
    }

    return item;
}

void
shc_multi_item_destroy(shc_multi_item_t *item)
{
    free(item->key);
    if (item->data)
        free(item->data);
}

static linked_list_t *
shc_split_buckets(shardcache_client_t *c, shc_multi_item_t **items)
{
    linked_list_t *pools = list_create();
    int i;
    for(i = 0; items[i]; i++) {
        const char *node_name;
        size_t name_len = 0;

        shc_multi_item_t *item = items[i];

        chash_lookup(c->chash, item->key, item->klen, &node_name, &name_len);

        char name_string[name_len+1];
        snprintf(name_string, name_len+1, "%s", node_name);

        tagged_value_t *tval = list_get_tagged_value(pools, name_string);
        if (!tval) {
            linked_list_t *sublist = list_create();
            tval = list_create_tagged_sublist(name_string, sublist);
            list_push_tagged_value(pools, tval);
        }

        list_push_value((linked_list_t *)tval->value, item);

    }

    return pools;
}


typedef struct {
    char *peer;
    fbuf_t *commands;
    shc_multi_item_t **items;
    async_read_ctx_t *reader; 
    int data_offset;
    int response_index;
    int num_requests;
    shardcache_hdr_t cmd;
} shc_multi_ctx_t;

static int
shc_multi_collect_data(void *data, size_t len, int idx, void *priv)
{
    shc_multi_ctx_t *ctx = (shc_multi_ctx_t *)priv;
    shc_multi_item_t *item = ctx->items[ctx->response_index];
    if (idx == 0 && len) {
        if (ctx->cmd == SHC_HDR_GET) {
            item->data = realloc(item->data, ctx->data_offset + len);
            memcpy(item->data + ctx->data_offset, data, len);
            ctx->data_offset += len;
        } else {
            if (len == 1) {
                item->status = (int)*((char *)data);
            }
        }
    }
    return 0;
}

static void
shc_multi_context_destroy(shc_multi_ctx_t *ctx)
{
    async_read_context_destroy(ctx->reader);
    fbuf_free(ctx->commands);
    free(ctx->items);
    free(ctx);
}

static shc_multi_ctx_t *
shc_multi_context_create(shardcache_hdr_t cmd, char *secret, char *peer, linked_list_t *items)
{
    shc_multi_ctx_t *ctx = calloc(1, sizeof(shc_multi_ctx_t));
    ctx->commands = fbuf_create(0);
    ctx->num_requests = list_count(items);
    ctx->items = calloc(1, sizeof(shc_multi_item_t *) * ctx->num_requests);
    ctx->reader = async_read_context_create(secret, shc_multi_collect_data, ctx);
    ctx->cmd = cmd;
    int n;
    for (n = 0; n < ctx->num_requests; n++) {
        shc_multi_item_t *item = list_pick_value(items, n);
        ctx->items[n] = item;
        
        shardcache_record_t record[3] = {
            {
                .v = item->key,
                .l = item->klen
            },
            {
                .v = NULL,
                .l = 0
            },
            {
                .v = NULL,
                .l = 0
            }
        };
        int num_records = 1;
        unsigned char sig_hdr = secret ? SHC_HDR_SIGNATURE_SIP : 0;

        if (cmd == SHC_HDR_SET) {
            record[1].v = item->data;
            record[1].l = item->dlen;
            num_records = 2;
            if (item->expire) {
                uint32_t expire_nbo = ntohl(item->expire);
                record[2].v = &expire_nbo;
                record[2].l = sizeof(uint32_t);
                num_records = 3;
            }
        }

        if (build_message(secret, sig_hdr, cmd, record, num_records, ctx->commands) != 0) {
            fprintf(stderr, "Can't create new command!\n");
            fbuf_free(ctx->commands);
            free(ctx->items);
            async_read_context_destroy(ctx->reader);
            free(ctx);
            return NULL;
        }
    }
    return ctx;
}


static void
shc_multi_close_connection(iomux_t *iomux, int fd, void *priv)
{
    shc_multi_ctx_t *ctx = (shc_multi_ctx_t *)priv;
    shc_multi_context_destroy(ctx);
    close(fd);
    if (iomux_isempty(iomux))
        iomux_end_loop(iomux);
}

int
shc_multi_fetch_response(iomux_t *iomux, int fd, unsigned char *data, int len, void *priv)
{
    shc_multi_ctx_t *ctx = (shc_multi_ctx_t *)priv;
    int processed = 0;
    
    //printf("received %d\n", len);
    async_read_context_state_t state = async_read_context_input_data(ctx->reader, data, len, &processed);
    while (state == SHC_STATE_READING_DONE) {
        ctx->data_offset = 0;
        ctx->response_index++;
        state = async_read_context_update(ctx->reader);
    }
    if (state == SHC_STATE_READING_ERR) {
        fprintf(stderr, "Async context returned error\n");
        iomux_close(iomux, fd);
    }
    if (ctx->response_index == ctx->num_requests)
        iomux_close(iomux, fd);

    return len;
}

void
shc_multi_send_command(iomux_t *iomux, int fd, unsigned char *data, int *len, void *priv)
{
    shc_multi_ctx_t *ctx = (shc_multi_ctx_t *)priv;
    fbuf_t *output_buffer = ctx->commands;

    // flush as much as we can
    if (fbuf_used(output_buffer)) {
        if (*len > fbuf_used(output_buffer)) {
            *len = fbuf_used(output_buffer);
            memcpy(data, fbuf_data(output_buffer), *len);
            fbuf_clear(output_buffer);
        } else {
            memcpy(data, fbuf_data(output_buffer), *len);
            fbuf_remove(output_buffer, *len);
        }
    } else {
        *len = 0;
    }
}

static inline int
shardcache_client_multi(shardcache_client_t *c,
                         shc_multi_item_t **items,
                         shardcache_hdr_t cmd)
{
    linked_list_t *pools = shc_split_buckets(c, items);

    iomux_t *iomux = iomux_create(0, 0);
    uint32_t count = list_count(pools);
    int i;
    for (i = 0; i < count; i++) {

        tagged_value_t *tval = list_pick_tagged_value(pools, i);
        linked_list_t *items = (linked_list_t *)tval->value;
        shc_multi_ctx_t *ctx = shc_multi_context_create(cmd, (char *)c->auth, tval->tag, items);
        if (!ctx) {
            iomux_destroy(iomux);
            list_destroy(pools);
            return -1;
        }

        iomux_callbacks_t cbs = {
            .mux_output = shc_multi_send_command,
            .mux_timeout = NULL,
            .mux_input = shc_multi_fetch_response,
            .mux_eof = shc_multi_close_connection,
            .priv = ctx
        };

        shardcache_node_t *node = shardcache_get_node(c, tval->tag);
        if (!node) {
            shc_multi_context_destroy(ctx);
            iomux_destroy(iomux);
            list_destroy(pools);
            return -1;
        }

        char *addr = shardcache_node_get_address(node);
        int fd = connections_pool_get(c->connections, addr);
        if (fd < 0) {
            c->errno = SHARDCACHE_CLIENT_ERROR_NETWORK;
            snprintf(c->errstr, sizeof(c->errstr), "Can't connect to '%s'", addr);
            shc_multi_context_destroy(ctx);
            iomux_destroy(iomux);
            list_destroy(pools);
            return -1;
        }

        iomux_add(iomux, fd, &cbs);
    }

    struct timeval tv = { 1, 0 };
    iomux_loop(iomux, &tv);
    iomux_destroy(iomux);
    list_destroy(pools);
    return 0;
}

int
shardcache_client_get_multi(shardcache_client_t *c,
                            shc_multi_item_t **items)

{
    return shardcache_client_multi(c, items, SHC_HDR_GET);
}

int
shardcache_client_set_multi(shardcache_client_t *c,
                            shc_multi_item_t **items)

{
    return shardcache_client_multi(c, items, SHC_HDR_SET);
}
