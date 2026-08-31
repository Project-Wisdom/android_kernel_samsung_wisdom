/*
 * BPF DEVMAP_HASH Map Implementation
 *
 * Supports BPF_MAP_TYPE_DEVMAP_HASH with real net_device lifetime tracking,
 * RCU-protected hash lookup, netdevice notification unregister handling,
 * and safe read-only value access from BPF.
 */

#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/rculist.h>

#define DEV_CREATE_FLAG_MASK \
	(BPF_F_NO_PREALLOC | BPF_F_RDONLY | BPF_F_WRONLY | \
	 BPF_F_RDONLY_PROG | BPF_F_WRONLY_PROG)

struct bpf_dtab_netdev {
	struct net_device *dev;
	struct hlist_node index_hlist;
	struct bpf_dtab *dtab;
	struct rcu_head rcu;
	u32 idx;
	u32 ifindex;
};

struct bpf_dtab {
	struct bpf_map map;
	struct hlist_head *dev_index_head;
	spinlock_t index_lock;
	unsigned int n_buckets;
	u32 items;
	struct list_head list;
};

static DEFINE_SPINLOCK(dev_map_lock);
static LIST_HEAD(dev_map_list);

static inline struct hlist_head *dev_map_index_hash(struct bpf_dtab *dtab,
						    u32 idx)
{
	return &dtab->dev_index_head[idx & (dtab->n_buckets - 1)];
}

static struct bpf_dtab_netdev *
__dev_map_hash_lookup_elem(struct bpf_dtab *dtab, u32 key)
{
	struct hlist_head *head = dev_map_index_hash(dtab, key);
	struct bpf_dtab_netdev *dev;

	hlist_for_each_entry_rcu(dev, head, index_hlist) {
		if (dev->idx == key)
			return dev;
	}

	return NULL;
}

static void *dev_map_hash_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_dtab *dtab = container_of(map, struct bpf_dtab, map);
	struct bpf_dtab_netdev *dev = __dev_map_hash_lookup_elem(dtab, *(u32 *)key);

	return dev ? &dev->ifindex : NULL;
}

static void __dev_map_entry_free(struct rcu_head *rcu)
{
	struct bpf_dtab_netdev *dev =
		container_of(rcu, struct bpf_dtab_netdev, rcu);

	if (dev->dev)
		dev_put(dev->dev);
	kfree(dev);
}

static int dev_map_hash_get_next_key(struct bpf_map *map, void *key,
				     void *next_key)
{
	struct bpf_dtab *dtab = container_of(map, struct bpf_dtab, map);
	u32 idx = key ? *(u32 *)key : 0;
	struct bpf_dtab_netdev *dev, *next_dev = NULL;
	int i = 0;

	if (key) {
		dev = __dev_map_hash_lookup_elem(dtab, idx);
		if (dev) {
			next_dev = hlist_entry_safe(
				rcu_dereference_raw(hlist_next_rcu(&dev->index_hlist)),
				struct bpf_dtab_netdev, index_hlist);
			if (next_dev) {
				*(u32 *)next_key = next_dev->idx;
				return 0;
			}
			i = (idx & (dtab->n_buckets - 1)) + 1;
		}
	}

	for (; i < dtab->n_buckets; i++) {
		struct hlist_head *head = &dtab->dev_index_head[i];

		next_dev = hlist_entry_safe(
			rcu_dereference_raw(hlist_first_rcu(head)),
			struct bpf_dtab_netdev, index_hlist);
		if (next_dev) {
			*(u32 *)next_key = next_dev->idx;
			return 0;
		}
	}

	return -ENOENT;
}

static int dev_map_hash_update_elem(struct bpf_map *map, void *key, void *value,
				    u64 map_flags)
{
	struct bpf_dtab *dtab = container_of(map, struct bpf_dtab, map);
	struct net *net = current->nsproxy->net_ns;
	struct bpf_dtab_netdev *dev, *old_dev;
	struct hlist_head *head;
	struct net_device *netdev;
	u32 idx = *(u32 *)key;
	u32 ifindex = *(u32 *)value;

	if (unlikely(map_flags > BPF_EXIST))
		return -EINVAL;

	if (!ifindex)
		return -EINVAL;

	spin_lock_bh(&dtab->index_lock);

	head = dev_map_index_hash(dtab, idx);
	old_dev = __dev_map_hash_lookup_elem(dtab, idx);

	if (old_dev && (map_flags == BPF_NOEXIST)) {
		spin_unlock_bh(&dtab->index_lock);
		return -EEXIST;
	}

	if (!old_dev && (map_flags == BPF_EXIST)) {
		spin_unlock_bh(&dtab->index_lock);
		return -ENOENT;
	}

	if (!old_dev && (dtab->items >= dtab->map.max_entries)) {
		spin_unlock_bh(&dtab->index_lock);
		return -E2BIG;
	}

	netdev = dev_get_by_index(net, ifindex);
	if (!netdev) {
		spin_unlock_bh(&dtab->index_lock);
		return -EINVAL;
	}

	dev = kzalloc(sizeof(*dev), GFP_ATOMIC | __GFP_NOWARN);
	if (!dev) {
		dev_put(netdev);
		spin_unlock_bh(&dtab->index_lock);
		return -ENOMEM;
	}

	dev->dev = netdev;
	dev->idx = idx;
	dev->ifindex = ifindex;
	dev->dtab = dtab;

	if (old_dev) {
		hlist_del_rcu(&old_dev->index_hlist);
		hlist_add_head_rcu(&dev->index_hlist, head);
		spin_unlock_bh(&dtab->index_lock);

		call_rcu(&old_dev->rcu, __dev_map_entry_free);
		return 0;
	}

	hlist_add_head_rcu(&dev->index_hlist, head);
	dtab->items++;
	spin_unlock_bh(&dtab->index_lock);

	return 0;
}

static int dev_map_hash_delete_elem(struct bpf_map *map, void *key)
{
	struct bpf_dtab *dtab = container_of(map, struct bpf_dtab, map);
	struct bpf_dtab_netdev *old_dev;
	u32 idx = *(u32 *)key;

	spin_lock_bh(&dtab->index_lock);
	old_dev = __dev_map_hash_lookup_elem(dtab, idx);
	if (!old_dev) {
		spin_unlock_bh(&dtab->index_lock);
		return -ENOENT;
	}

	hlist_del_rcu(&old_dev->index_hlist);
	dtab->items--;
	spin_unlock_bh(&dtab->index_lock);

	call_rcu(&old_dev->rcu, __dev_map_entry_free);
	return 0;
}

static struct bpf_map *dev_map_hash_alloc(union bpf_attr *attr)
{
	struct bpf_dtab *dtab;
	u64 cost;
	int i, ret;

	if (!capable(CAP_NET_ADMIN))
		return ERR_PTR(-EPERM);

	/* check sanity of attributes */
	if (attr->max_entries == 0 || attr->key_size != 4 ||
	    attr->value_size != 4 || attr->map_flags & ~DEV_CREATE_FLAG_MASK)
		return ERR_PTR(-EINVAL);

	dtab = kzalloc(sizeof(*dtab), GFP_USER);
	if (!dtab)
		return ERR_PTR(-ENOMEM);

	/* copy mandatory map attributes */
	dtab->map.map_type = attr->map_type;
	dtab->map.key_size = attr->key_size;
	dtab->map.value_size = attr->value_size;
	dtab->map.max_entries = attr->max_entries;
	dtab->map.map_flags = attr->map_flags;

	dtab->n_buckets = roundup_pow_of_two(attr->max_entries);
	if (!dtab->n_buckets) {
		kfree(dtab);
		return ERR_PTR(-EINVAL);
	}

	cost = (u64)sizeof(struct hlist_head) * dtab->n_buckets;
	cost += sizeof(*dtab);
	cost = round_up(cost, PAGE_SIZE) >> PAGE_SHIFT;

	ret = bpf_map_precharge_memlock(cost);
	if (ret < 0) {
		kfree(dtab);
		return ERR_PTR(ret);
	}
	dtab->map.pages = cost;

	dtab->dev_index_head = bpf_map_area_alloc(sizeof(struct hlist_head) * dtab->n_buckets);
	if (!dtab->dev_index_head) {
		kfree(dtab);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < dtab->n_buckets; i++)
		INIT_HLIST_HEAD(&dtab->dev_index_head[i]);

	spin_lock_init(&dtab->index_lock);
	INIT_LIST_HEAD(&dtab->list);

	spin_lock_bh(&dev_map_lock);
	list_add_tail(&dtab->list, &dev_map_list);
	spin_unlock_bh(&dev_map_lock);

	return &dtab->map;
}

static void dev_map_hash_free(struct bpf_map *map)
{
	struct bpf_dtab *dtab = container_of(map, struct bpf_dtab, map);
	int i;

	spin_lock_bh(&dev_map_lock);
	list_del(&dtab->list);
	spin_unlock_bh(&dev_map_lock);

	synchronize_rcu();

	for (i = 0; i < dtab->n_buckets; i++) {
		struct bpf_dtab_netdev *dev;
		struct hlist_node *tmp;

		hlist_for_each_entry_safe(dev, tmp, &dtab->dev_index_head[i], index_hlist) {
			hlist_del_rcu(&dev->index_hlist);
			if (dev->dev)
				dev_put(dev->dev);
			kfree(dev);
		}
	}

	bpf_map_area_free(dtab->dev_index_head);
	kfree(dtab);
}

static int dev_map_notification(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);
	struct bpf_dtab *dtab;

	if (event != NETDEV_UNREGISTER)
		return NOTIFY_OK;

	rcu_read_lock();
	spin_lock_bh(&dev_map_lock);
	list_for_each_entry(dtab, &dev_map_list, list) {
		int i;

		spin_lock_bh(&dtab->index_lock);
		for (i = 0; i < dtab->n_buckets; i++) {
			struct bpf_dtab_netdev *dev;
			struct hlist_node *tmp;

			hlist_for_each_entry_safe(dev, tmp, &dtab->dev_index_head[i], index_hlist) {
				if (dev->dev == netdev) {
					hlist_del_rcu(&dev->index_hlist);
					dtab->items--;
					call_rcu(&dev->rcu, __dev_map_entry_free);
				}
			}
		}
		spin_unlock_bh(&dtab->index_lock);
	}
	spin_unlock_bh(&dev_map_lock);
	rcu_read_unlock();

	return NOTIFY_OK;
}

static struct notifier_block dev_map_notifier = {
	.notifier_call = dev_map_notification,
};

const struct bpf_map_ops dev_map_hash_ops = {
	.map_alloc = dev_map_hash_alloc,
	.map_free = dev_map_hash_free,
	.map_get_next_key = dev_map_hash_get_next_key,
	.map_lookup_elem = dev_map_hash_lookup_elem,
	.map_update_elem = dev_map_hash_update_elem,
	.map_delete_elem = dev_map_hash_delete_elem,
};

static struct bpf_map_type_list devmap_hash_type __read_mostly = {
	.ops = &dev_map_hash_ops,
	.type = BPF_MAP_TYPE_DEVMAP_HASH,
};

static int __init dev_map_init(void)
{
	bpf_register_map_type(&devmap_hash_type);
	return register_netdevice_notifier(&dev_map_notifier);
}
late_initcall(dev_map_init);
