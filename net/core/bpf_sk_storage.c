// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 Facebook  */
#include <linux/rculist.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <uapi/linux/btf.h>
#include <net/bpf_sk_storage.h>
#include <net/sock.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/socket.h>

static inline bool hlist_is_singular_node(struct hlist_node *n, struct hlist_head *h)
{
	return !n->next && n->pprev == &h->first;
}

static inline void *__compat_kvcalloc(size_t n, size_t size, gfp_t flags)
{
	if (size != 0 && n > SIZE_MAX / size)
		return NULL;
	return kmalloc_array(n, size, flags | __GFP_ZERO);
}
#define kvcalloc __compat_kvcalloc

static atomic_t cache_idx;

struct bucket {
	struct hlist_head list;
	raw_spinlock_t lock;
};

/* The map is not the primary owner of a bpf_sk_storage_elem.
 * Instead, the sk->sk_bpf_storage is.
 *
 * The map (bpf_sk_storage_map) is for two purposes:
 * 1. Define the size of the "sk local storage". It is
 *    the map's value_size.
 *
 * 2. Maintain a list to keep track of all elems such
 *    that they can be cleaned up during the map destruction.
 *
 * When a bpf local storage is being looked up for a
 * particular sk, the "bpf_map" pointer is actually used
 * as the "key" to search in the list of elem in
 * sk->sk_bpf_storage.
 */
struct bpf_sk_storage_map {
	struct bpf_map map;
	struct bucket *buckets;
	u32 bucket_log;
	u16 elem_size;
	u16 cache_idx;
};

struct bpf_sk_storage_data {
	struct bpf_sk_storage_map __rcu *smap;
	u8 data[0] __aligned(8);
};

/* Linked to bpf_sk_storage and bpf_sk_storage_map */
struct bpf_sk_storage_elem {
	struct hlist_node map_node;	/* Linked to bpf_sk_storage_map */
	struct hlist_node snode;	/* Linked to bpf_sk_storage */
	struct bpf_sk_storage __rcu *sk_storage;
	struct rcu_head rcu;
	struct bpf_sk_storage_data sdata ____cacheline_aligned;
};

#define SELEM(_SDATA) container_of((_SDATA), struct bpf_sk_storage_elem, sdata)
#define SDATA(_SELEM) (&(_SELEM)->sdata)
#define BPF_SK_STORAGE_CACHE_SIZE	16

struct bpf_sk_storage {
	struct bpf_sk_storage_data __rcu *cache[BPF_SK_STORAGE_CACHE_SIZE];
	struct hlist_head list;	/* List of bpf_sk_storage_elem */
	struct sock *sk;	/* The sk that owns the above "list" */
	struct rcu_head rcu;
	raw_spinlock_t lock;	/* Protect adding/removing from the "list" */
};

static struct bucket *select_bucket(struct bpf_sk_storage_map *smap,
				    struct bpf_sk_storage_elem *selem)
{
	return &smap->buckets[hash_ptr(selem, smap->bucket_log)];
}

static int omem_charge(struct sock *sk, unsigned int size)
{
	/* same check as in sock_kmalloc() */
	if (size <= sysctl_optmem_max &&
	    atomic_read(&sk->sk_omem_alloc) + size < sysctl_optmem_max) {
		atomic_add(size, &sk->sk_omem_alloc);
		return 0;
	}

	return -ENOMEM;
}

static bool selem_linked_to_sk(const struct bpf_sk_storage_elem *selem)
{
	return !hlist_unhashed(&selem->snode);
}

static bool selem_linked_to_map(const struct bpf_sk_storage_elem *selem)
{
	return !hlist_unhashed(&selem->map_node);
}

static struct bpf_sk_storage_elem *selem_alloc(struct bpf_sk_storage_map *smap,
					       struct sock *sk, void *value,
					       bool charge_omem)
{
	struct bpf_sk_storage_elem *selem;

	if (charge_omem && omem_charge(sk, smap->elem_size))
		return NULL;

	selem = kzalloc(smap->elem_size, GFP_ATOMIC | __GFP_NOWARN);
	if (selem) {
		if (value)
			memcpy(SDATA(selem)->data, value, smap->map.value_size);
		return selem;
	}

	if (charge_omem)
		atomic_sub(smap->elem_size, &sk->sk_omem_alloc);

	return NULL;
}

/* sk_storage->lock must be held and selem->sk_storage == sk_storage.
 * The caller must ensure selem->smap is still valid to be
 * dereferenced for its smap->elem_size and smap->cache_idx.
 */
static bool __selem_unlink_sk(struct bpf_sk_storage *sk_storage,
			      struct bpf_sk_storage_elem *selem,
			      bool uncharge_omem)
{
	struct bpf_sk_storage_map *smap;
	bool free_sk_storage;
	struct sock *sk;

	smap = rcu_dereference(SDATA(selem)->smap);
	sk = sk_storage->sk;

	/* All uncharging on sk->sk_omem_alloc must be done first.
	 * sk may be freed once the last selem is unlinked from sk_storage.
	 */
	if (uncharge_omem)
		atomic_sub(smap->elem_size, &sk->sk_omem_alloc);

	free_sk_storage = hlist_is_singular_node(&selem->snode,
						 &sk_storage->list);
	if (free_sk_storage) {
		atomic_sub(sizeof(struct bpf_sk_storage), &sk->sk_omem_alloc);
		sk_storage->sk = NULL;
		/* After this RCU_INIT, sk may be freed and cannot be used */
		RCU_INIT_POINTER(sk->sk_bpf_storage, NULL);
	}
	hlist_del_init_rcu(&selem->snode);
	if (rcu_access_pointer(sk_storage->cache[smap->cache_idx]) ==
	    SDATA(selem))
		RCU_INIT_POINTER(sk_storage->cache[smap->cache_idx], NULL);

	kfree_rcu(selem, rcu);

	return free_sk_storage;
}

static void selem_unlink_sk(struct bpf_sk_storage_elem *selem)
{
	struct bpf_sk_storage *sk_storage;
	bool free_sk_storage = false;

	if (unlikely(!selem_linked_to_sk(selem)))
		return;

	sk_storage = rcu_dereference(selem->sk_storage);
	raw_spin_lock_bh(&sk_storage->lock);
	if (likely(selem_linked_to_sk(selem)))
		free_sk_storage = __selem_unlink_sk(sk_storage, selem, true);
	raw_spin_unlock_bh(&sk_storage->lock);

	if (free_sk_storage)
		kfree_rcu(sk_storage, rcu);
}

/* sk_storage->lock must be held and sk_storage->list cannot be empty */
static void __selem_link_sk(struct bpf_sk_storage *sk_storage,
			    struct bpf_sk_storage_elem *selem)
{
	RCU_INIT_POINTER(selem->sk_storage, sk_storage);
	hlist_add_head(&selem->snode, &sk_storage->list);
}

static void selem_unlink_map(struct bpf_sk_storage_elem *selem)
{
	struct bpf_sk_storage_map *smap;
	struct bucket *b;

	if (unlikely(!selem_linked_to_map(selem)))
		return;

	smap = rcu_dereference(SDATA(selem)->smap);
	b = select_bucket(smap, selem);
	raw_spin_lock_bh(&b->lock);
	if (likely(selem_linked_to_map(selem)))
		hlist_del_init_rcu(&selem->map_node);
	raw_spin_unlock_bh(&b->lock);
}

static void selem_link_map(struct bpf_sk_storage_map *smap,
			   struct bpf_sk_storage_elem *selem)
{
	struct bucket *b = select_bucket(smap, selem);

	raw_spin_lock_bh(&b->lock);
	RCU_INIT_POINTER(SDATA(selem)->smap, smap);
	hlist_add_head_rcu(&selem->map_node, &b->list);
	raw_spin_unlock_bh(&b->lock);
}

static void selem_unlink(struct bpf_sk_storage_elem *selem)
{
	selem_unlink_map(selem);
	selem_unlink_sk(selem);
}

static struct bpf_sk_storage_data *
__sk_storage_lookup(struct bpf_sk_storage *sk_storage,
		    struct bpf_sk_storage_map *smap,
		    bool cacheit_lockit)
{
	struct bpf_sk_storage_data *sdata;
	struct bpf_sk_storage_elem *selem;

	/* Fast path (cache hit) */
	sdata = rcu_dereference(sk_storage->cache[smap->cache_idx]);
	if (sdata && rcu_access_pointer(sdata->smap) == smap)
		return sdata;

	/* Slow path (cache miss) */
	hlist_for_each_entry_rcu(selem, &sk_storage->list, snode)
		if (rcu_access_pointer(SDATA(selem)->smap) == smap)
			break;

	if (!selem)
		return NULL;

	sdata = SDATA(selem);
	if (cacheit_lockit) {
		raw_spin_lock_bh(&sk_storage->lock);
		if (selem_linked_to_sk(selem))
			rcu_assign_pointer(sk_storage->cache[smap->cache_idx],
					   sdata);
		raw_spin_unlock_bh(&sk_storage->lock);
	}

	return sdata;
}

static struct bpf_sk_storage_data *
sk_storage_lookup(struct sock *sk, struct bpf_map *map, bool cacheit_lockit)
{
	struct bpf_sk_storage *sk_storage;
	struct bpf_sk_storage_map *smap;

	sk_storage = rcu_dereference(sk->sk_bpf_storage);
	if (!sk_storage)
		return NULL;

	smap = (struct bpf_sk_storage_map *)map;
	return __sk_storage_lookup(sk_storage, smap, cacheit_lockit);
}

static int check_flags(const struct bpf_sk_storage_data *old_sdata,
		       u64 map_flags)
{
	if (map_flags != BPF_ANY &&
	    map_flags != BPF_NOEXIST &&
	    map_flags != BPF_EXIST)
		return -EINVAL;

	if (old_sdata && (map_flags & BPF_NOEXIST))
		return -EEXIST;

	if (!old_sdata && (map_flags & BPF_EXIST))
		return -ENOENT;

	return 0;
}

static int sk_storage_alloc(struct sock *sk,
			    struct bpf_sk_storage_map *smap,
			    struct bpf_sk_storage_elem *first_selem)
{
	struct bpf_sk_storage *prev_sk_storage, *sk_storage;
	int err;

	err = omem_charge(sk, sizeof(*sk_storage));
	if (err)
		return err;

	sk_storage = kzalloc(sizeof(*sk_storage), GFP_ATOMIC | __GFP_NOWARN);
	if (!sk_storage) {
		err = -ENOMEM;
		goto uncharge;
	}
	INIT_HLIST_HEAD(&sk_storage->list);
	raw_spin_lock_init(&sk_storage->lock);
	sk_storage->sk = sk;

	__selem_link_sk(sk_storage, first_selem);
	selem_link_map(smap, first_selem);

	prev_sk_storage = cmpxchg((struct bpf_sk_storage **)&sk->sk_bpf_storage,
				  NULL, sk_storage);
	if (unlikely(prev_sk_storage)) {
		selem_unlink_map(first_selem);
		err = -EAGAIN;
		goto uncharge;
	}

	return 0;

uncharge:
	kfree(sk_storage);
	atomic_sub(sizeof(*sk_storage), &sk->sk_omem_alloc);
	return err;
}

static struct bpf_sk_storage_data *sk_storage_update(struct sock *sk,
						     struct bpf_map *map,
						     void *value,
						     u64 map_flags)
{
	struct bpf_sk_storage_data *old_sdata = NULL;
	struct bpf_sk_storage_elem *selem;
	struct bpf_sk_storage *sk_storage;
	struct bpf_sk_storage_map *smap;
	int err;

	smap = (struct bpf_sk_storage_map *)map;
	sk_storage = rcu_dereference(sk->sk_bpf_storage);
	if (!sk_storage || hlist_empty(&sk_storage->list)) {
		/* Very first elem for this sk */
		err = check_flags(NULL, map_flags);
		if (err)
			return ERR_PTR(err);

		selem = selem_alloc(smap, sk, value, true);
		if (!selem)
			return ERR_PTR(-ENOMEM);

		err = sk_storage_alloc(sk, smap, selem);
		if (err) {
			kfree(selem);
			atomic_sub(smap->elem_size, &sk->sk_omem_alloc);
			return ERR_PTR(err);
		}

		return SDATA(selem);
	}

	raw_spin_lock_bh(&sk_storage->lock);

	/* Recheck sk_storage->list under sk_storage->lock */
	if (unlikely(hlist_empty(&sk_storage->list))) {
		err = -EAGAIN;
		goto unlock_err;
	}

	old_sdata = __sk_storage_lookup(sk_storage, smap, false);
	err = check_flags(old_sdata, map_flags);
	if (err)
		goto unlock_err;

	selem = selem_alloc(smap, sk, value, !old_sdata);
	if (!selem) {
		err = -ENOMEM;
		goto unlock_err;
	}

	/* First, link the new selem to the map */
	selem_link_map(smap, selem);

	/* Second, link (and publish) the new selem to sk_storage */
	__selem_link_sk(sk_storage, selem);

	/* Third, remove old selem */
	if (old_sdata) {
		selem_unlink_map(SELEM(old_sdata));
		__selem_unlink_sk(sk_storage, SELEM(old_sdata), false);
	}

	raw_spin_unlock_bh(&sk_storage->lock);
	return SDATA(selem);

unlock_err:
	raw_spin_unlock_bh(&sk_storage->lock);
	return ERR_PTR(err);
}

static int sk_storage_delete(struct sock *sk, struct bpf_map *map)
{
	struct bpf_sk_storage_data *sdata;

	sdata = sk_storage_lookup(sk, map, false);
	if (!sdata)
		return -ENOENT;

	selem_unlink(SELEM(sdata));

	return 0;
}

/* Called by sk_destruct() */
void bpf_sk_storage_free(struct sock *sk)
{
	struct bpf_sk_storage_elem *selem;
	struct bpf_sk_storage *sk_storage;
	bool free_sk_storage = false;
	struct hlist_node *n;

	rcu_read_lock();
	sk_storage = rcu_dereference(sk->sk_bpf_storage);
	if (!sk_storage) {
		rcu_read_unlock();
		return;
	}

	raw_spin_lock_bh(&sk_storage->lock);
	hlist_for_each_entry_safe(selem, n, &sk_storage->list, snode) {
		selem_unlink_map(selem);
		free_sk_storage = __selem_unlink_sk(sk_storage, selem, true);
	}
	raw_spin_unlock_bh(&sk_storage->lock);
	rcu_read_unlock();

	if (free_sk_storage)
		kfree_rcu(sk_storage, rcu);
}

static void bpf_sk_storage_map_free(struct bpf_map *map)
{
	struct bpf_sk_storage_elem *selem;
	struct bpf_sk_storage_map *smap;
	struct bucket *b;
	unsigned int i;

	smap = (struct bpf_sk_storage_map *)map;

	synchronize_rcu();

	for (i = 0; i < (1U << smap->bucket_log); i++) {
		b = &smap->buckets[i];

		rcu_read_lock();
		while ((selem = hlist_entry_safe(rcu_dereference_raw(hlist_first_rcu(&b->list)),
						 struct bpf_sk_storage_elem,
						 map_node))) {
			selem_unlink(selem);
			cond_resched_rcu();
		}
		rcu_read_unlock();
	}

	synchronize_rcu();

	kvfree(smap->buckets);
	kfree(map);
}

static int bpf_sk_storage_map_alloc_check(union bpf_attr *attr)
{
	if (attr->map_flags != BPF_F_NO_PREALLOC || attr->max_entries ||
	    attr->key_size != sizeof(int) || !attr->value_size)
		return -EINVAL;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (attr->value_size >= KMALLOC_MAX_SIZE -
	    MAX_BPF_STACK - sizeof(struct bpf_sk_storage_elem) ||
	    attr->value_size > U16_MAX - sizeof(struct bpf_sk_storage_elem))
		return -E2BIG;

	return 0;
}

static void bpf_map_init_from_attr(struct bpf_map *map, union bpf_attr *attr)
{
	map->map_type = attr->map_type;
	map->key_size = attr->key_size;
	map->value_size = attr->value_size;
	map->max_entries = attr->max_entries;
	map->map_flags = attr->map_flags;
}

static struct bpf_map *bpf_sk_storage_map_alloc(union bpf_attr *attr)
{
	struct bpf_sk_storage_map *smap;
	unsigned int i;
	u32 nbuckets;
	u64 cost;
	int err;

	err = bpf_sk_storage_map_alloc_check(attr);
	if (err)
		return ERR_PTR(err);

	smap = kzalloc(sizeof(*smap), GFP_USER | __GFP_NOWARN);
	if (!smap)
		return ERR_PTR(-ENOMEM);
	bpf_map_init_from_attr(&smap->map, attr);

	smap->bucket_log = ilog2(roundup_pow_of_two(num_possible_cpus()));
	nbuckets = 1U << smap->bucket_log;
	smap->buckets = kvcalloc(sizeof(*smap->buckets), nbuckets,
				 GFP_USER | __GFP_NOWARN);
	if (!smap->buckets) {
		kfree(smap);
		return ERR_PTR(-ENOMEM);
	}
	cost = sizeof(*smap->buckets) * nbuckets + sizeof(*smap);

	for (i = 0; i < nbuckets; i++) {
		INIT_HLIST_HEAD(&smap->buckets[i].list);
		raw_spin_lock_init(&smap->buckets[i].lock);
	}

	smap->elem_size = sizeof(struct bpf_sk_storage_elem) + attr->value_size;
	smap->cache_idx = (unsigned int)atomic_inc_return(&cache_idx) %
		BPF_SK_STORAGE_CACHE_SIZE;
	smap->map.pages = round_up(cost, PAGE_SIZE) >> PAGE_SHIFT;

	return &smap->map;
}

static int notsupp_get_next_key(struct bpf_map *map, void *key,
				void *next_key)
{
	return -ENOTSUPP;
}

static void *bpf_fd_sk_storage_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_sk_storage_data *sdata;
	struct socket *sock;
	int fd, err;

	fd = *(int *)key;
	sock = sockfd_lookup(fd, &err);
	if (sock) {
		sdata = sk_storage_lookup(sock->sk, map, true);
		sockfd_put(sock);
		return sdata ? sdata->data : NULL;
	}

	return ERR_PTR(err);
}

static int bpf_fd_sk_storage_update_elem(struct bpf_map *map, void *key,
					 void *value, u64 map_flags)
{
	struct bpf_sk_storage_data *sdata;
	struct socket *sock;
	int fd, err;

	if (map_flags != BPF_ANY &&
	    map_flags != BPF_NOEXIST &&
	    map_flags != BPF_EXIST)
		return -EINVAL;

	fd = *(int *)key;
	sock = sockfd_lookup(fd, &err);
	if (sock) {
		sdata = sk_storage_update(sock->sk, map, value, map_flags);
		sockfd_put(sock);
		return IS_ERR(sdata) ? PTR_ERR(sdata) : 0;
	}

	return err;
}

static int bpf_fd_sk_storage_delete_elem(struct bpf_map *map, void *key)
{
	struct socket *sock;
	int fd, err;

	fd = *(int *)key;
	sock = sockfd_lookup(fd, &err);
	if (sock) {
		err = sk_storage_delete(sock->sk, map);
		sockfd_put(sock);
		return err;
	}

	return err;
}

static int bpf_sk_storage_map_check_btf(const struct bpf_map *map,
					const struct btf *btf,
					u32 key_type_id,
					u32 value_type_id)
{
	const struct btf_type *key_type, *val_type;
	u32 key_size = 0, val_size = 0;
	u32 int_data;

	key_type = btf_type_id_size(btf, &key_type_id, &key_size);
	if (!key_type || key_size != sizeof(int))
		return -EINVAL;

	if (BTF_INFO_KIND(key_type->info) != BTF_KIND_INT)
		return -EINVAL;

	int_data = *(u32 *)(key_type + 1);
	if (BTF_INT_BITS(int_data) != 32 || BTF_INT_OFFSET(int_data))
		return -EINVAL;

	val_type = btf_type_id_size(btf, &value_type_id, &val_size);
	if (!val_type || val_size != map->value_size)
		return -EINVAL;

	return 0;
}

BPF_CALL_4(bpf_sk_storage_get, struct bpf_map *, map, struct sock *, sk,
	   void *, value, u64, flags)
{
	struct bpf_sk_storage_data *sdata;

	if (!sk || !sk_fullsock(sk) ||
	    (flags & ~BPF_SK_STORAGE_GET_F_CREATE))
		return (unsigned long)NULL;

	sdata = sk_storage_lookup(sk, map, true);
	if (sdata)
		return (unsigned long)sdata->data;

	if (flags == BPF_SK_STORAGE_GET_F_CREATE &&
	    atomic_inc_not_zero(&sk->sk_refcnt)) {
		sdata = sk_storage_update(sk, map, value, BPF_NOEXIST);
		sock_put(sk);
		return IS_ERR(sdata) ?
			(unsigned long)NULL : (unsigned long)sdata->data;
	}

	return (unsigned long)NULL;
}

BPF_CALL_2(bpf_sk_storage_delete, struct bpf_map *, map, struct sock *, sk)
{
	if (!sk || !sk_fullsock(sk))
		return -EINVAL;

	if (atomic_inc_not_zero(&sk->sk_refcnt)) {
		int err;

		err = sk_storage_delete(sk, map);
		sock_put(sk);
		return err;
	}

	return -ENOENT;
}

const struct bpf_map_ops sk_storage_map_ops = {
	.map_alloc = bpf_sk_storage_map_alloc,
	.map_free = bpf_sk_storage_map_free,
	.map_get_next_key = notsupp_get_next_key,
	.map_lookup_elem = bpf_fd_sk_storage_lookup_elem,
	.map_update_elem = bpf_fd_sk_storage_update_elem,
	.map_delete_elem = bpf_fd_sk_storage_delete_elem,
	.map_check_btf = bpf_sk_storage_map_check_btf,
};

const struct bpf_func_proto bpf_sk_storage_get_proto = {
	.func		= bpf_sk_storage_get,
	.gpl_only	= false,
	.ret_type	= RET_PTR_TO_MAP_VALUE_OR_NULL,
	.arg1_type	= ARG_CONST_MAP_PTR,
	.arg2_type	= ARG_PTR_TO_SOCK_COMMON,
	.arg3_type	= ARG_PTR_TO_MAP_VALUE_OR_NULL,
	.arg4_type	= ARG_ANYTHING,
};

const struct bpf_func_proto bpf_sk_storage_delete_proto = {
	.func		= bpf_sk_storage_delete,
	.gpl_only	= false,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_CONST_MAP_PTR,
	.arg2_type	= ARG_PTR_TO_SOCK_COMMON,
};

const struct bpf_func_proto bpf_sk_storage_get_cg_sock_proto = {
	.func		= bpf_sk_storage_get,
	.gpl_only	= false,
	.ret_type	= RET_PTR_TO_MAP_VALUE_OR_NULL,
	.arg1_type	= ARG_CONST_MAP_PTR,
	.arg2_type	= ARG_PTR_TO_CTX,
	.arg3_type	= ARG_PTR_TO_MAP_VALUE_OR_NULL,
	.arg4_type	= ARG_ANYTHING,
};

static struct bpf_map_type_list sk_storage_map_type __read_mostly = {
	.ops = &sk_storage_map_ops,
	.type = BPF_MAP_TYPE_SK_STORAGE,
};

static int __init register_sk_storage_map(void)
{
	bpf_register_map_type(&sk_storage_map_type);
	return 0;
}
late_initcall(register_sk_storage_map);
