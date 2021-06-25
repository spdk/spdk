/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __OCF_LIST_H__
#define __OCF_LIST_H__

#define LIST_POISON1  ((void *) 0x00100100)
#define LIST_POISON2  ((void *) 0x00200200)

/**
 * List entry structure mimicking linux kernel based one.
 */
struct list_head {
	struct list_head *next;
	struct list_head *prev;
} __attribute__((aligned(64)));

/**
 * start an empty list
 */
#define INIT_LIST_HEAD(l) { (l)->prev = l; (l)->next = l; }

/**
 * Add item to list head.
 * @param it list entry to be added
 * @param l1 list main node (head)
 */
static inline void list_add(struct list_head *it, struct list_head *l1)
{
	it->prev = l1;
	it->next = l1->next;

	l1->next->prev = it;
	l1->next = it;
}

/**
 * Add item it to tail.
 * @param it list entry to be added
 * @param l1 list main node (head)
 */
static inline void list_add_tail(struct list_head *it, struct list_head *l1)
{
	it->prev = l1->prev;
	it->next = l1;

	l1->prev->next = it;
	l1->prev = it;
}

/**
 * check if a list is empty (return true)
 */
static inline int list_empty(struct list_head *it)
{
	return it->next == it;
}

/**
 * delete an entry from a list
 */
static inline void list_del(struct list_head *it)
{
	it->next->prev = it->prev;
	it->prev->next = it->next;
}

static inline void list_move_tail(struct list_head *list,
				  struct list_head *head)
{
	list_del(list);
	list_add_tail(list, head);
}

static inline void list_move(struct list_head *list,
			     struct list_head *head)
{
	list_del(list);
	list_add(list, head);
}

/**
 * Extract an entry.
 * @param list_head_i list head item, from which entry is extracted
 * @param item_type type (struct) of list entry
 * @param field_name name of list_head field within item_type
 */
#define list_entry(list_head_i, item_type, field_name) \
	(item_type *)(((void*)(list_head_i)) - offsetof(item_type, field_name))

#define list_first_entry(list_head_i, item_type, field_name) \
	list_entry((list_head_i)->next, item_type, field_name)

/**
 * @param iterator uninitialized list_head pointer, to be used as iterator
 * @param plist list head (main node)
 */
#define list_for_each(iterator, plist) \
	for (iterator = (plist)->next; \
	     (iterator)->next != (plist)->next; \
	     iterator = (iterator)->next)

/**
 * Safe version of list_for_each which works even if entries are deleted during
 * loop.
 * @param iterator uninitialized list_head pointer, to be used as iterator
 * @param q another uninitialized list_head, used as helper
 * @param plist list head (main node)
 */
/*
 * Algorithm handles situation, where q is deleted.
 * consider in example 3 element list with header h:
 *
 *   h -> 1 -> 2 -> 3 ->
 *1.      i    q
 *
 *2.           i    q
 *
 *3. q              i
 */
#define list_for_each_safe(iterator, q, plist) \
	for (iterator = (q = (plist)->next->next)->prev; \
	     (q) != (plist)->next; \
	     iterator = (q = (q)->next)->prev)

#define _list_entry_helper(item, head, field_name) list_entry(head, typeof(*item), field_name)

/**
 * Iterate over list entries.
 * @param list pointer to list item (iterator)
 * @param plist pointer to list_head item
 * @param field_name name of list_head field in list entry
 */
#define list_for_each_entry(item, plist, field_name) \
	for (item = _list_entry_helper(item, (plist)->next, field_name); \
	     _list_entry_helper(item, (item)->field_name.next, field_name) !=\
		     _list_entry_helper(item, (plist)->next, field_name); \
	     item = _list_entry_helper(item, (item)->field_name.next, field_name))

/**
 * Safe version of list_for_each_entry which works even if entries are deleted
 * during loop.
 * @param list pointer to list item (iterator)
 * @param q another pointer to list item, used as helper
 * @param plist pointer to list_head item
 * @param field_name name of list_head field in list entry
 */
#define list_for_each_entry_safe(item, q, plist, field_name)		\
	for (item = _list_entry_helper(item, (plist)->next, field_name), \
	     q = _list_entry_helper(item, (item)->field_name.next, field_name); \
	     _list_entry_helper(item, (item)->field_name.next, field_name) != \
		     _list_entry_helper(item, (plist)->next, field_name); \
	     item = q, q = _list_entry_helper(q, (q)->field_name.next, field_name))

#endif
