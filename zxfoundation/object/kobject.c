// SPDX-License-Identifier: Apache-2.0
// zxfoundation/object/kobject.c

#include <zxfoundation/object/kobject.h>
#include <zxfoundation/memory/slab.h>
#include <lib/string.h>

/// @brief Internal bridge between kref and kobject lifecycle.
static void kobject_release_internal(kref_t *ref) {
    kobject_t *obj = kobject_container(ref, kobject_t, ref);
    obj->state = KOBJECT_DEAD;
    if (obj->ops && obj->ops->release) {
        obj->ops->release(obj);
    }
}

/// @brief Initialize a kobject.
void kobject_init(kobject_t *obj, const kobject_ops_t *ops, const char *name) {
    kref_init(&obj->ref);
    obj->ops = ops;
    obj->name = name;
    obj->state = KOBJECT_ALIVE;
}

/// @brief Get a reference.
kobject_t *kobject_get(kobject_t *obj) {
    if (obj) kref_get(&obj->ref);
    return obj;
}

/// @brief Release a reference.
void kobject_put(kobject_t *obj) {
    if (obj) {
        kref_put(&obj->ref, kobject_release_internal);
    }
}

/// @brief Increment only if the object is still alive.
bool kobject_get_unless_dead(kobject_t *obj) {
    if (!obj) return false;
    return kref_get_unless_zero(&obj->ref);
}
