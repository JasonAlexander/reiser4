/* Copyright 2001, 2002 by Hans Reiser, licensing governed by reiser4/README */

/* plugin-sets */

#include "../debug.h"

#include "plugin_set.h"

#include <linux/slab.h>
#include <linux/stddef.h>

/* slab for plugin sets */
static kmem_cache_t *plugin_set_slab;

static spinlock_t plugin_set_lock = SPIN_LOCK_UNLOCKED;

/* hash table support */

#define PS_TABLE_SIZE (32)

static inline plugin_set *
cast_to(const atomic_t * a)
{
	return container_of(a, plugin_set, ref);
}

static inline int
pseq(const atomic_t * a1, const atomic_t * a2)
{
	plugin_set *set1;
	plugin_set *set2;

	set1 = cast_to(a1);
	set2 = cast_to(a2);
	return 
		set1->file == set2->file &&
		set1->dir == set2->dir &&
		set1->perm == set2->perm &&
		set1->tail == set2->tail &&
		set1->hash == set2->hash &&
		set1->sd == set2->sd &&
		set1->dir_item == set2->dir_item &&
		set1->crypto == set2->crypto &&
		set1->compression == set2->compression;
}

#define HASH_FIELD(hash, set, field)		\
({						\
	__u32 top;				\
						\
	top = (hash) >> (32 - 4);		\
	(hash) <<= 4;				\
	(hash) |= top;				\
	(hash) ^= (__u32)(set)->field;		\
})

static inline __u32
pshash(const atomic_t * a)
{
	plugin_set *set;
	__u32 result;

	set = cast_to(a);

	result = 0;
	HASH_FIELD(result, set, file);
	HASH_FIELD(result, set, dir);
	HASH_FIELD(result, set, perm);
	HASH_FIELD(result, set, tail);
	HASH_FIELD(result, set, hash);
	HASH_FIELD(result, set, sd);
	HASH_FIELD(result, set, dir_item);
	HASH_FIELD(result, set, crypto);
	HASH_FIELD(result, set, compression);
	return result & (PS_TABLE_SIZE - 1);
}

/* The hash table definition */
#define KMALLOC(size) kmalloc((size), GFP_KERNEL)
#define KFREE(ptr, size) kfree(ptr)
TS_HASH_DEFINE(ps, plugin_set, atomic_t, ref, link, pshash, pseq);
#undef KFREE
#undef KMALLOC

static ps_hash_table ps_table;
static plugin_set empty_set = {
	.ref                = ATOMIC_INIT(1),
	.file               = NULL,
	.dir                = NULL,
	.perm               = NULL,
	.tail               = NULL,
	.hash               = NULL,
	.sd                 = NULL,
	.dir_item           = NULL,
	.crypto             = NULL,
	.compression        = NULL,
	.link               = { NULL }
};

plugin_set *plugin_set_get_empty(void)
{
	return plugin_set_clone(&empty_set);
}

plugin_set *plugin_set_clone(plugin_set *set)
{
	assert("nikita-2901", set != NULL);

	atomic_inc(&set->ref);
	return set;
}

void plugin_set_put(plugin_set *set)
{
	assert("nikita-2900", set != NULL);

	if (atomic_dec_and_lock(&set->ref, &plugin_set_lock)) {
		assert("nikita-2899", set != &empty_set);
		ps_hash_remove(&ps_table, set);
		kmem_cache_free(plugin_set_slab, set);
	}
}

int plugin_set_field(plugin_set **set, void *val, int offset, int len)
{
	int result;

	assert("nikita-2902", set != NULL);
	assert("nikita-2904", *set != NULL);
	assert("nikita-2903", val != NULL);

	result = 0;
	if (memcmp(((char *)*set) + offset, val, len)) {
		plugin_set replica;
		plugin_set *twin;
		plugin_set *psal;
		plugin_set *orig;

		replica = *(orig = *set);
		xmemcpy(((char *)&replica) + offset, val, len);
		psal = NULL;
		do {
			spin_lock(&plugin_set_lock);
			twin = ps_hash_find(&ps_table, &replica.ref);
			if (twin == NULL) {
				if (psal == NULL) {
					spin_unlock(&plugin_set_lock);
					psal = kmem_cache_alloc(plugin_set_slab,
								GFP_KERNEL);
					if (psal == NULL)
						result = -ENOMEM;
					continue;
				}
				*(*set = psal) = replica;
				atomic_set(&psal->ref, 1);
				ps_hash_insert(&ps_table, psal);
				psal = NULL;
			} else
				*set = plugin_set_clone(twin);
			spin_unlock(&plugin_set_lock);
			plugin_set_put(orig);
			break;
		} while (result == 0);
		if (psal != NULL)
			kmem_cache_free(plugin_set_slab, psal);
	}
	return result;
}

#define DEFINE_PLUGIN_SET(type, field)						\
int plugin_set_ ## field(plugin_set **set, type *val)				\
{										\
	return plugin_set_field(set, &val, 					\
				offsetof(plugin_set, field), sizeof(val));	\
}

DEFINE_PLUGIN_SET(file_plugin, file)
DEFINE_PLUGIN_SET(dir_plugin, dir)
DEFINE_PLUGIN_SET(perm_plugin, perm)
DEFINE_PLUGIN_SET(tail_plugin, tail)
DEFINE_PLUGIN_SET(hash_plugin, hash)
DEFINE_PLUGIN_SET(item_plugin, sd)
DEFINE_PLUGIN_SET(item_plugin, dir_item)
DEFINE_PLUGIN_SET(crypto_plugin, crypto)
DEFINE_PLUGIN_SET(compression_plugin, compression)

int plugin_set_init(void)
{
	int result;

	result = ps_hash_init(&ps_table, PS_TABLE_SIZE);
	if (result == 0) {
		plugin_set_slab = kmem_cache_create("plugin_set", 
						    sizeof (plugin_set), 0, 
						    SLAB_HWCACHE_ALIGN, 
						    NULL, NULL);
		if (plugin_set_slab == NULL)
			result = -ENOMEM;
	}
	return result;
}

void plugin_set_done(void)
{
	kmem_cache_destroy(plugin_set_slab);
	ps_hash_done(&ps_table);
}


/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
