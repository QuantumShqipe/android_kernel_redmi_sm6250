/*
 * Copyright (c) 2019, Tyler Nijmeh <tylernij@gmail.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

/* For this many sync requests, perform one async request */
#define	DEFAULT_SYNC_RATIO	(4)

enum {
	SYNC,
	ASYNC
};

struct anxiety_data {
	struct list_head queue[2];
	uint16_t contig_syncs;

	/* Tunables */
	uint8_t sync_ratio;
};

static inline bool anxiety_can_dispatch(struct anxiety_data *adata)
{
	return !list_empty(&adata->queue[SYNC]) ||
			!list_empty(&adata->queue[ASYNC]);
}

static void anxiety_merged_requests(struct request_queue *q, struct request *rq,
		struct request *next)
{
	list_del_init(&next->queuelist);
}

static int __anxiety_dispatch(struct request_queue *q, struct request *rq)
{
	if (!rq)
		return -EINVAL;

	list_del_init(&rq->queuelist);
	elv_dispatch_sort(q, rq);

	return 0;
}

static int anxiety_dispatch(struct request_queue *q, int force)
{
	struct anxiety_data *adata = q->elevator->elevator_data;
	int batched;

	/* Make sure we can even process any requests at all */
	if (list_empty(&adata->queue[READ]) &&
			list_empty(&adata->queue[WRITE]))
		return 0;

	/* Batch sync requests according to tunables */
	for (batched = 0; batched < adata->sync_ratio; batched++) {
		if (!list_empty(&adata->queue[SYNC]))
			__anxiety_dispatch(q,
					rq_entry_fifo(adata->queue[SYNC].next));
	}

	/* Submit one async request after the sync batch to avoid starvation */
	if (!list_empty(&adata->queue[ASYNC]))
		__anxiety_dispatch(q,
			rq_entry_fifo(adata->queue[ASYNC].next));

	return 1;
}

static void anxiety_add_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = rq_data_dir(rq);

	list_add_tail(&rq->queuelist, &((struct anxiety_data *) q->elevator->elevator_data)->queue[dir]);
}

static struct request *anxiety_former_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = rq_data_dir(rq);

	if (rq->queuelist.prev == &((struct anxiety_data *) q->elevator->elevator_data)->queue[dir])
		return NULL;

	return list_prev_entry(rq, queuelist);
}

static struct request *anxiety_latter_request(struct request_queue *q, struct request *rq)
{
	const uint8_t dir = rq_data_dir(rq);

	if (rq->queuelist.next == &((struct anxiety_data *) q->elevator->elevator_data)->queue[dir])
		return NULL;

	return list_next_entry(rq, queuelist);
}

static int anxiety_init_queue(struct request_queue *q,
		struct elevator_type *elv)
{
	struct anxiety_data *adata;
	struct elevator_queue *eq = elevator_alloc(q, elv);

	if (!eq)
		return -ENOMEM;

	/* Allocate the data */
	adata = kmalloc_node(sizeof(*adata), GFP_KERNEL, q->node);
	if (!adata) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	/* Set the elevator data */
	eq->elevator_data = adata;

	/* Initialize */
	INIT_LIST_HEAD(&adata->queue[SYNC]);
	INIT_LIST_HEAD(&adata->queue[ASYNC]);
	adata->contig_syncs = 0;
	adata->sync_ratio = DEFAULT_SYNC_RATIO;

	/* Set elevator to Anxiety */
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return 0;
}

/* Sysfs access */
static ssize_t anxiety_sync_ratio_show(struct elevator_queue *e, char *page)
{
	struct anxiety_data *adata = e->elevator_data;

	return snprintf(page, PAGE_SIZE, "%u\n", adata->sync_ratio);
}

static ssize_t anxiety_sync_ratio_store(struct elevator_queue *e,
		const char *page, size_t count)
{
	struct anxiety_data *adata = e->elevator_data;
	int ret;

	ret = kstrtou8(page, 0, &adata->sync_ratio);
	if (ret < 0)
		return ret;

	return count;
}

static struct elv_fs_entry anxiety_attrs[] = {
	__ATTR(sync_ratio, 0644, anxiety_sync_ratio_show,
			anxiety_sync_ratio_store),
	__ATTR_NULL
};

static struct elevator_type elevator_anxiety = {
	.ops.sq = {
		.elevator_merge_req_fn	= anxiety_merged_requests,
		.elevator_dispatch_fn	= anxiety_dispatch,
		.elevator_add_req_fn	= anxiety_add_request,
		.elevator_former_req_fn	= anxiety_former_request,
		.elevator_latter_req_fn	= anxiety_latter_request,
		.elevator_init_fn	= anxiety_init_queue,
	},
	.elevator_name = "anxiety",
	.elevator_attrs = anxiety_attrs,
	.elevator_owner = THIS_MODULE,
};

static int __init anxiety_init(void)
{
	return elv_register(&elevator_anxiety);
}

static void __exit anxiety_exit(void)
{
	elv_unregister(&elevator_anxiety);
}

module_init(anxiety_init);
module_exit(anxiety_exit);

MODULE_AUTHOR("Tyler Nijmeh");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Anxiety IO scheduler");
