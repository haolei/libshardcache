#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#include "shardcache.h"
#include "shardcache_internal.h"
#include "arc_ops.h"
#include "messaging.h"

/**
 * * Here are the operations implemented
 *
 * */

typedef struct {
    cached_object_t *obj;
    void *data;
    size_t len;
} shardcache_fetch_from_peer_notify_arg;

static int
arc_ops_fetch_from_peer_notify_listener (void *item, uint32_t idx, void *user)
{
    shardcache_get_listener_t *listener = (shardcache_get_listener_t *)item;
    shardcache_fetch_from_peer_notify_arg *arg = (shardcache_fetch_from_peer_notify_arg *)user;
    cached_object_t *obj = arg->obj;
    int rc = (listener->cb(obj->key, obj->klen, arg->data, arg->len, 0, NULL, listener->priv) == 0);
    if (!rc) {
        free(listener);
        return -1;
    }
    return 1;
}

static int
arc_ops_fetch_from_peer_notify_listener_complete(void *item, uint32_t idx, void *user)
{
    shardcache_get_listener_t *listener = (shardcache_get_listener_t *)item;
    cached_object_t *obj = (cached_object_t *)user;
    listener->cb(obj->key, obj->klen, NULL, 0, obj->dlen, &obj->ts, listener->priv);
    free(listener);
    return -1;
}

static int
arc_ops_fetch_from_peer_notify_listener_error(void *item, uint32_t idx, void *user)
{
    shardcache_get_listener_t *listener = (shardcache_get_listener_t *)item;
    cached_object_t *obj = (cached_object_t *)user;
    listener->cb(obj->key, obj->klen, NULL, 0, 0, NULL, listener->priv);
    free(listener);
    return -1;
}

static void
arc_ops_evict_object(shardcache_t *cache, cached_object_t *obj)
{
    if (list_count(obj->listeners)) {
        COBJ_SET_FLAG(obj, COBJ_FLAG_EVICT);
        return;
    }
    if (obj->data) {
        free(obj->data);
        obj->data = NULL;
        obj->dlen = 0;
        ATOMIC_INCREMENT(cache->cnt[SHARDCACHE_COUNTER_EVICTS].value);
    }
    obj->flags = COBJ_FLAG_EVICTED; // reset all flags but leave the EVICTED bit on
    list_clear(obj->listeners);
}

typedef struct
{
    cached_object_t *obj;
    shardcache_t *cache;
    char *peer_addr;
    int fd;
} shc_fetch_async_arg_t;

static int
arc_ops_fetch_from_peer_async_cb(char *peer,
                                 void *key,
                                 size_t klen,
                                 void *data,
                                 size_t len,
                                 int error,
                                 void *priv)
{
    shc_fetch_async_arg_t *arg = (shc_fetch_async_arg_t *)priv;
    cached_object_t *obj = arg->obj;
    shardcache_t *cache = arg->cache;
    char *peer_addr = arg->peer_addr;
    int fd = arg->fd;
    int complete = 0;
    int total_len = 0;
    int drop = 0;

    MUTEX_LOCK(&obj->lock);
    if (!obj->res) {
        list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener_error, obj);
        COBJ_UNSET_FLAG(obj, COBJ_FLAG_FETCHING);
        MUTEX_UNLOCK(&obj->lock);
        return -1;
    }
    arc_retain_resource(cache->arc, obj->res);
    if (!obj->listeners) {
        list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener_error, obj);
        COBJ_UNSET_FLAG(obj, COBJ_FLAG_FETCHING);
        MUTEX_UNLOCK(&obj->lock);
        arc_release_resource(cache->arc, obj->res);
        return -1;
    }
    if (error) {
        list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener_error, obj);
        COBJ_UNSET_FLAG(obj, COBJ_FLAG_FETCHING);
        close(fd);
        free(arg);
        if (COBJ_CHECK_FLAGS(obj, COBJ_FLAG_EVICT))
            arc_ops_evict_object(cache, obj);
        else
            drop = 1;
    } else if (len) {
        obj->data = realloc(obj->data, obj->dlen + len);
        memcpy(obj->data + obj->dlen, data, len);
        obj->dlen += len;
        shardcache_fetch_from_peer_notify_arg arg = {
            .obj = obj,
            .data = data,
            .len = len
        };
        list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener, &arg);
    } else {
        list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener_complete, obj);
        COBJ_SET_FLAG(obj, COBJ_FLAG_COMPLETE);
        COBJ_UNSET_FLAG(obj, COBJ_FLAG_FETCHING);
        complete = 1;
        total_len = obj->dlen;
        // XXX - in theory we shuould let read_message_async() and its input data handler
        //       (who is calling us) take care of calling iomux_close() on this fd and hence
        //       removing it from the async mux. We need to do it now (earlier) only because
        //       we want to put back the filedescriptor into the connections pool (for further usage)
        //       which would create issues if we do it before removing it from the async mux.
        //       This means that when our caller will try using iomux_close() on this fd, the attempt
        //       will fail but it will stil need to take care of releasing the async_read context. 
        //       (check commit 792760ada8e655d7b87996788b490c9718177bc7)
        if (arg->fd > 0)
            iomux_remove(cache->async_mux, arg->fd);
        shardcache_release_connection_for_peer(cache, peer_addr, fd);
        free(arg);

        if (COBJ_CHECK_FLAGS(obj, COBJ_FLAG_EVICT))
            arc_ops_evict_object(cache, obj);
        else if (COBJ_CHECK_FLAGS(obj, COBJ_FLAG_DROP))
            drop = 1;
    }

    if (complete) {
        if (total_len && !drop) {
            arc_update_size(cache->arc, key, klen, total_len);
            int evicted = COBJ_CHECK_FLAGS(obj, COBJ_FLAG_EVICT) ||
                          COBJ_CHECK_FLAGS(obj, COBJ_FLAG_EVICTED);
            if (cache->expire_time > 0 && !evicted && !cache->lazy_expiration) {
                shardcache_schedule_expiration(cache, key, klen, cache->expire_time, 0);
            }
        } else if (error || drop) {
            arc_remove(cache->arc, key, klen);
        }
    }

    MUTEX_UNLOCK(&obj->lock);
    arc_release_resource(cache->arc, obj->res);

    return !error ? 0 : -1;
}


static int
arc_ops_fetch_from_peer(shardcache_t *cache, cached_object_t *obj, char *peer)
{
    int rc = -1;
    if (shardcache_log_level() >= LOG_DEBUG) {
        char keystr[1024];
        KEY2STR(obj->key, obj->klen, keystr, sizeof(keystr));
        SHC_DEBUG2("Fetching data for key %s from peer %s", keystr, peer); 
    }

    shardcache_node_t *node = shardcache_node_select(cache, peer);
    if (!peer) {
        SHC_ERROR("Can't find address for node %s\n", peer);
        return rc;
    }
    char *peer_addr = shardcache_node_get_address(node);

    // another peer is responsible for this item, let's get the value from there

    int fd = shardcache_get_connection_for_peer(cache, peer_addr);
    if (COBJ_CHECK_FLAGS(obj, COBJ_FLAG_ASYNC)) {
        shc_fetch_async_arg_t *arg = malloc(sizeof(shc_fetch_async_arg_t));
        arg->obj = obj;
        arg->cache = cache;
        arg->peer_addr = peer_addr;
        arg->fd = fd;
        async_read_wrk_t *wrk = NULL;
        COBJ_SET_FLAG(obj, COBJ_FLAG_FETCHING);
        rc = fetch_from_peer_async(peer_addr,
                                   (char *)cache->auth,
                                   SHC_HDR_CSIGNATURE_SIP,
                                   obj->key,
                                   obj->klen,
                                   0,
                                   0,
                                   arc_ops_fetch_from_peer_async_cb,
                                   arg,
                                   fd,
                                   &wrk);
        if (rc == 0) {
            queue_push_right(cache->async_queue, wrk);

            // Keep the remote object in the cache only 10% of the time.
            // This is the same logic applied by groupcache to determine hot keys.
            // Better approaches are possible but maybe unnecessary.
            if (!cache->force_caching && rand() % 10 != 0)
                COBJ_SET_FLAG(obj, COBJ_FLAG_DROP);
            else
                COBJ_UNSET_FLAG(obj, COBJ_FLAG_DROP);

        } else {
            if (obj->listeners) {
                list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener_error, obj);
                list_clear(obj->listeners);
            }

            COBJ_UNSET_FLAG(obj, COBJ_FLAG_FETCHING);
            COBJ_SET_FLAG(obj, COBJ_FLAG_EVICTED);

            shardcache_release_connection_for_peer(cache, peer_addr, fd);

            free(arg);
        }
    } else { 
        fbuf_t value = FBUF_STATIC_INITIALIZER;
        COBJ_SET_FLAG(obj, COBJ_FLAG_FETCHING);
        rc = fetch_from_peer(peer_addr, (char *)cache->auth, SHC_HDR_SIGNATURE_SIP, obj->key, obj->klen, &value, fd);
        COBJ_UNSET_FLAG(obj, COBJ_FLAG_FETCHING);
        if (rc == 0 && fbuf_used(&value)) {
            obj->data = fbuf_data(&value);
            obj->dlen = fbuf_used(&value);
            COBJ_SET_FLAG(obj, COBJ_FLAG_COMPLETE);
            if (!cache->force_caching && rand() % 10 != 0)
                COBJ_SET_FLAG(obj, COBJ_FLAG_DROP);
            else
                COBJ_UNSET_FLAG(obj, COBJ_FLAG_DROP);
        } else {
            // if succeded the fbuf buffer has been moved to the obj structure
            // but otherwise we have to release it
            fbuf_destroy(&value);
        }
        shardcache_release_connection_for_peer(cache, peer_addr, fd);
    }

    return rc;
}

void *
arc_ops_create(const void *key, size_t len, int async, arc_resource_t *res, void *priv)
{
    shardcache_t *cache = (shardcache_t *)priv;
    cached_object_t *obj = calloc(1, sizeof(cached_object_t));

    obj->klen = len;
    obj->key = malloc(len);
    memcpy(obj->key, key, len);
    obj->data = NULL;
    COBJ_UNSET_FLAG(obj, COBJ_FLAG_COMPLETE);
    obj->res = res;
    if (async) {
        COBJ_SET_FLAG(obj, COBJ_FLAG_ASYNC);
        obj->listeners = list_create();
        list_set_free_value_callback(obj->listeners, free);
    }
    MUTEX_INIT(&obj->lock);
    obj->arc = cache->arc;

    return obj;
}

static void *
arc_ops_fetch_copy_volatile_object_cb(void *ptr, size_t len, void *user)
{
    volatile_object_t *item = (volatile_object_t *)ptr;
    volatile_object_t *copy = malloc(sizeof(volatile_object_t));
    copy->data = malloc(item->dlen);
    memcpy(copy->data, item->data, item->dlen);
    copy->dlen = item->dlen;
    copy->expire = item->expire;
    return copy;
}

int
arc_ops_fetch(void *item, size_t *size, void * priv)
{
    cached_object_t *obj = (cached_object_t *)item;
    shardcache_t *cache = (shardcache_t *)priv;

    ATOMIC_INCREMENT(cache->cnt[SHARDCACHE_COUNTER_CACHE_MISSES].value);

    MUTEX_LOCK(&obj->lock);
    if (obj->data || COBJ_CHECK_FLAGS(obj, COBJ_FLAG_ASYNC|COBJ_FLAG_FETCHING))
    { // the value is already loaded or being downloaded, we don't need to fetch
        *size = obj->dlen;
        MUTEX_UNLOCK(&obj->lock);
        return 0;
    }

    // this object is not evicted anymore (if it eventually was)
    COBJ_UNSET_FLAG(obj, COBJ_FLAG_EVICTED);
    char node_name[1024];
    size_t node_len = sizeof(node_name);
    memset(node_name, 0, node_len);
    // if we are not the owner try asking to the peer responsible for this data
    if (!shardcache_test_ownership(cache, obj->key, obj->klen, node_name, &node_len))
    {
        int done = 1;
        int ret = arc_ops_fetch_from_peer(cache, obj, node_name);
        if (ret == -1) {
            int check = shardcache_test_migration_ownership(cache,
                                                            obj->key,
                                                            obj->klen,
                                                            node_name,
                                                            &node_len);
            if (check == 0) {
                ret = arc_ops_fetch_from_peer(cache, obj, node_name);
            }

            if (check == 1 || (ret == -1 && cache->storage.global)) {
                // if it's a global storage or we are responsible in the
                // migration context, we don't want to return earlier
                done = 0;
                COBJ_UNSET_FLAG(obj, COBJ_FLAG_EVICTED);
            }
        }
        if (done) {
            ATOMIC_INCREMENT(cache->cnt[SHARDCACHE_COUNTER_FETCH_REMOTE].value);
            if (ret == 0) {
                gettimeofday(&obj->ts, NULL);
                *size = obj->dlen;
                MUTEX_UNLOCK(&obj->lock);
                return COBJ_CHECK_FLAGS(obj, COBJ_FLAG_DROP|COBJ_FLAG_COMPLETE) ? 1 : 0;
            }
            MUTEX_UNLOCK(&obj->lock);
            ATOMIC_INCREMENT(cache->cnt[SHARDCACHE_COUNTER_ERRORS].value);
            return -1;
        }
    }

    char keystr[1024];
    if (shardcache_log_level() >= LOG_DEBUG)
        KEY2STR(obj->key, obj->klen, keystr, sizeof(keystr));

    ATOMIC_INCREMENT(cache->cnt[SHARDCACHE_COUNTER_FETCH_LOCAL].value);

    // we are responsible for this item ... 
    // let's first check if it's among the volatile keys otherwise
    // fetch it from the storage
    volatile_object_t *vobj = ht_get_deep_copy(cache->volatile_storage,
                                               obj->key,
                                               obj->klen,
                                               NULL,
                                               arc_ops_fetch_copy_volatile_object_cb,
                                               NULL);
    if (vobj) {
        obj->data = vobj->data; 
        obj->dlen = vobj->dlen;
        free(vobj);
        if (obj->data && obj->dlen) {
            SHC_DEBUG3("Found volatile value %s (%lu) for key %s",
                   shardcache_hex_escape(obj->data, obj->dlen, DEBUG_DUMP_MAXSIZE),
                   (unsigned long)obj->dlen, keystr);
        }
    } else if (cache->use_persistent_storage && cache->storage.fetch) {
        int rc = cache->storage.fetch(obj->key, obj->klen, &obj->data, &obj->dlen, cache->storage.priv);
        if (rc == -1) {
            SHC_ERROR("Fetch storage callback retunrned an error (%d)", rc);
            ATOMIC_INCREMENT(cache->cnt[SHARDCACHE_COUNTER_ERRORS].value);
            MUTEX_UNLOCK(&obj->lock);
            return -1;
        }
        if (obj->data && obj->dlen) {
            SHC_DEBUG3("Fetch storage callback returned value %s (%lu) for key %s",
                   shardcache_hex_escape(obj->data, obj->dlen, DEBUG_DUMP_MAXSIZE),
                   (unsigned long)obj->dlen, keystr);
        } else {
            SHC_DEBUG3("Fetch storage callback returned an empty value for key %s", keystr);
        }
    }

    gettimeofday(&obj->ts, NULL);

    COBJ_SET_FLAG(obj, COBJ_FLAG_COMPLETE);
    COBJ_UNSET_FLAG(obj, COBJ_FLAG_FETCHING);

    if (!obj->data) {
        if (COBJ_CHECK_FLAGS(obj, COBJ_FLAG_ASYNC) && obj->listeners)
            list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener_complete, obj);

        MUTEX_UNLOCK(&obj->lock);
        SHC_DEBUG("Item not found for key %s", keystr);
        ATOMIC_INCREMENT(cache->cnt[SHARDCACHE_COUNTER_NOT_FOUND].value);
        return 1;
    }

    if (COBJ_CHECK_FLAGS(obj, COBJ_FLAG_ASYNC)) {
        shardcache_fetch_from_peer_notify_arg arg = {
            .obj = obj,
            .data = obj->data,
            .len = obj->dlen
        };
        list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener, &arg);
        list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener_complete, obj);
        if (COBJ_CHECK_FLAGS(obj, COBJ_FLAG_EVICT))
            arc_ops_evict_object(cache, obj);
    }

    *size = obj->dlen;

    int evicted = COBJ_CHECK_FLAGS(obj, COBJ_FLAG_EVICT) ||
                  COBJ_CHECK_FLAGS(obj, COBJ_FLAG_EVICTED);
    if (cache->expire_time > 0 && !evicted && !cache->lazy_expiration) {
        shardcache_schedule_expiration(cache, obj->key, obj->klen, cache->expire_time, 0);
    }

    MUTEX_UNLOCK(&obj->lock);

    return 0;
}

void
arc_ops_evict(void *item, void *priv)
{
    cached_object_t *obj = (cached_object_t *)item;
    shardcache_t *cache = (shardcache_t *)priv;
    MUTEX_LOCK(&obj->lock);
    if (!cache->lazy_expiration)
        shardcache_unschedule_expiration(cache, obj->key, obj->klen, 0);
    arc_ops_evict_object(cache, obj);
    MUTEX_UNLOCK(&obj->lock);
}

void
arc_ops_destroy(void *item, void *priv)
{
    cached_object_t *obj = (cached_object_t *)item;

    MUTEX_LOCK(&obj->lock); // XXX - this is not really necessary
    if (obj->listeners) {
        // safety belts, just to ensure not leaking listeners by notifying them an error
        // before relasing the object completely
        // NOTE: there shouldn't ever be listeners here if the object is being released,
        //       but in case of race conditions or bugs which would end up registering
        //       listeners when they shouldn't, at least we notify back an error instead
        //       of making them wait forever
        list_foreach_value(obj->listeners, arc_ops_fetch_from_peer_notify_listener_error, obj);
        list_destroy(obj->listeners);
    }
    MUTEX_UNLOCK(&obj->lock);

    // no lock is necessary here ... if we are here
    // nobody is referencing us anymore
    if (obj->data) {
        free(obj->data);
    }
    if (obj->key)
        free(obj->key);

    MUTEX_DESTROY(&obj->lock);
    free(obj);
}


