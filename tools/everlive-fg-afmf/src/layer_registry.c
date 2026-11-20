/* The instances and devices that passed through the layer, keyed by the loader's dispatch
 * pointer. */

#include "layer_internal.h"

/* Readers on every submit and present from every thread of the application; writers only at
 * instance and device creation and destruction. */
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct afmf_instance *g_instances;
static struct afmf_device *g_devices;

struct afmf_instance *afmf_instance_find(void *key)
{
    pthread_rwlock_rdlock(&g_lock);
    struct afmf_instance *inst = g_instances;
    while (inst != NULL && inst->key != key)
        inst = inst->next;
    pthread_rwlock_unlock(&g_lock);
    return inst;
}

struct afmf_instance *afmf_instance_take(void *key)
{
    pthread_rwlock_wrlock(&g_lock);
    struct afmf_instance **link = &g_instances;
    while (*link != NULL && (*link)->key != key)
        link = &(*link)->next;
    struct afmf_instance *inst = *link;
    if (inst != NULL)
        *link = inst->next;
    pthread_rwlock_unlock(&g_lock);
    return inst;
}

void afmf_instance_register(struct afmf_instance *inst)
{
    pthread_rwlock_wrlock(&g_lock);
    inst->next = g_instances;
    g_instances = inst;
    pthread_rwlock_unlock(&g_lock);
}

struct afmf_device *afmf_device_find(void *key)
{
    pthread_rwlock_rdlock(&g_lock);
    struct afmf_device *dev = g_devices;
    while (dev != NULL && dev->key != key)
        dev = dev->next;
    pthread_rwlock_unlock(&g_lock);
    return dev;
}

struct afmf_device *afmf_device_take(void *key)
{
    pthread_rwlock_wrlock(&g_lock);
    struct afmf_device **link = &g_devices;
    while (*link != NULL && (*link)->key != key)
        link = &(*link)->next;
    struct afmf_device *dev = *link;
    if (dev != NULL)
        *link = dev->next;
    pthread_rwlock_unlock(&g_lock);
    return dev;
}

void afmf_device_register(struct afmf_device *dev)
{
    pthread_rwlock_wrlock(&g_lock);
    dev->next = g_devices;
    g_devices = dev;
    pthread_rwlock_unlock(&g_lock);
}

void afmf_device_free(struct afmf_device *dev)
{
    pthread_mutex_destroy(&dev->lock);
    pthread_mutex_destroy(&dev->async_lock);
    free(dev->queues);
    free(dev->queue_families);
    free(dev->app_families);
    free(dev);
}
