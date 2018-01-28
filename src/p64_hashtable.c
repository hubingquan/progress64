#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "p64_hashtable.h"
#include "p64_hazardptr.h"
#include "build_config.h"

#include "common.h"
#include "lockfree.h"

#define MARK_REMOVE 1UL
#define HAS_MARK(ptr) (((uintptr_t)(ptr) & MARK_REMOVE) != 0)
#define SET_MARK(ptr) (void *)((uintptr_t)(ptr) |  MARK_REMOVE)
#define REM_MARK(ptr) (void *)((uintptr_t)(ptr) & ~MARK_REMOVE)

//CACHE_LINE == 64, __SIZEOF_POINTER__ == 8 => BKT_SIZE == 4
#define BKT_SIZE (CACHE_LINE / (2 * __SIZEOF_POINTER__))

struct hash_bucket
{
    struct p64_hashentry entries[BKT_SIZE];
} ALIGNED(CACHE_LINE);

#if __SIZEOF_POINTER__ == 8
typedef __int128 uintptr_pair_t;
#define lockfree_compare_exchange_pair lockfree_compare_exchange_16
#endif

union heui
{
    struct p64_hashentry he;
    uintptr_pair_t ui;
};

struct p64_hashtable
{
    uint32_t nbkts;
    uint32_t nused;//entries
    struct hash_bucket buckets[0];
};

#ifndef NDEBUG
static uint32_t list_check(struct p64_hashentry *prnt,
			   uint64_t (*f)(struct p64_hashentry *))
{
    uint32_t num = 0;
    struct p64_hashentry *he;
    while ((he = REM_MARK(prnt->next)) != NULL)
    {
	printf(" <h=%lx,k=%lu>", prnt->hash, f(he));
	num++;
	prnt = he;
    }
    return num;
}

static uint32_t bucket_check(uint32_t bix,
			     struct hash_bucket *bkt,
			     uint64_t (*f)(struct p64_hashentry *))
{
    uint32_t num = 0;
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	printf("%u.%u:", bix, i);
	num += list_check(&bkt->entries[i], f);
	printf("\n");
    }
    return num;
}
#endif

uint32_t hash_table_check(struct p64_hashtable *ht,
			  uint64_t (*f)(struct p64_hashentry *))
{
    uint32_t num = 0;
#ifndef NDEBUG
    for (uint32_t i = 0; i < ht->nbkts; i++)
    {
	num += bucket_check(i, &ht->buckets[i], f);
    }
    printf("Found %u entries (%u)\n", num, ht->nused);
#endif
    return num;
}

struct p64_hashtable *p64_hashtable_alloc(uint32_t nentries)
{
    size_t nbkts = (nentries + BKT_SIZE - 1) / BKT_SIZE;
    size_t sz = sizeof(struct p64_hashtable) +
		sizeof(struct hash_bucket) * nbkts;
    struct p64_hashtable *ht = calloc(1, sz);
    if (ht != NULL)
    {
	ht->nbkts = nbkts;
	ht->nused = 0;
	//All buckets already cleared (NULL pointers)
    }
    return ht;
}

UNROLL_LOOPS ALWAYS_INLINE
static inline struct p64_hashentry *bucket_lookup(struct hash_bucket *bkt,
						  p64_hashtable_compare cf,
						  const void *key,
						  p64_hashvalue_t hash,
						  p64_hazardptr_t *hazpp)
{
    uint32_t mask = 0;
    //We want this loop unrolled
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (bkt->entries[i].hash == hash)
	{
	    mask |= 1U << i;
	}
    }
    while (mask != 0)
    {
	uint32_t i = __builtin_ffs(mask) - 1;
	struct p64_hashentry *he;
	he = hp_acquire((void**)&bkt->entries[i].next, hazpp);
	//The head entry pointers cannot be marked for REMOVAL
	assert(REM_MARK(he) == he);
	if (he != NULL)
	{
	    if (cf(he, key) == 0)
	    {
		//Found our object
		return he;
	    }
	}
	mask &= ~(1U << i);
    }
    return NULL;
}

static struct p64_hashentry *list_lookup(struct p64_hashentry *prnt,
					 p64_hashtable_compare cf,
					 const void *key,
					 p64_hazardptr_t *hazpp)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    for (;;)
    {
	struct p64_hashentry *this = hp_acquire((void**)&prnt->next, hazpp);
	this = REM_MARK(this);
	if (this == NULL)
	{
	    hp_release_ro(&hpprnt);
	    return NULL;
	}
	if (cf(this, key) == 0)
	{
	    //Found our object
	    hp_release_ro(&hpprnt);
	    return this;
	}
	//Continue search
	prnt = this;
	SWAP(hpprnt, *hazpp);
    }
}

struct p64_hashentry *p64_hashtable_lookup(struct p64_hashtable *ht,
					   p64_hashtable_compare cf,
					   const void *key,
					   p64_hashvalue_t hash,
					   p64_hazardptr_t *hazpp)
{
    uint32_t bix = (hash / BKT_SIZE) % ht->nbkts;
    struct hash_bucket *bkt = &ht->buckets[bix];
    struct p64_hashentry *he;
    *hazpp = P64_HAZARDPTR_NULL;
    he = bucket_lookup(bkt, cf, key, hash, hazpp);
    if (he != NULL)
    {
	return he;
    }
    he = list_lookup(&bkt->entries[hash % BKT_SIZE], cf, key, hazpp);
    if (he != NULL)
    {
	return he;
    }
    hp_release_ro(hazpp);
    return NULL;
}

//Remove node, return true if hashentry removed by us or some other thread
//Returns fails if hashentry cannot be removed due to parent marked for removal
static inline bool remove_node(struct p64_hashentry *prnt,
			       struct p64_hashentry *this,
			       p64_hashvalue_t hash,
			       int32_t *removed)
{
    assert(this == REM_MARK(this));
    //Set our REMOVE mark (it may already be set)
    __atomic_fetch_or(&this->next, MARK_REMOVE, __ATOMIC_RELAXED);
    //Now nobody may update our next pointer
    //And other threads may help to remove us
    //Swing our parent's next pointer

    //Expect prnt->this to be unmarked or parent is also marked for removal
    union heui old = {.he.hash = hash, .he.next = this };
    //New prnt->next should not have REMOVAL mark
    union heui neu = {.he.hash = this->hash, .he.next = REM_MARK(this->next) };
    if (lockfree_compare_exchange_pair((uintptr_pair_t *)prnt,
				       &old.ui,
				       neu.ui,
				       /*weak=*/false,
				       __ATOMIC_RELAXED,
				       __ATOMIC_RELAXED))
    {
	(*removed)++;
	return true;
    }
    else if (REM_MARK(old.he.next) != this)
    {
	//prnt->next doesn't point to 'this', 'this' already removed
	return true;
    }
    //Else prnt->next does point to 'this' but parent marked for removal
    assert(old.he.next == SET_MARK(this));
    return false;
}

static inline struct p64_hashentry *insert_node(struct p64_hashentry *prnt,
						struct p64_hashentry *he,
						p64_hashvalue_t hash)
{
    assert(he->hash == 0);
    assert(he->next == NULL);
    union heui old = {.he.hash = 0, .he.next = NULL };
    union heui neu = {.he.hash = hash, .he.next = he };
    if (lockfree_compare_exchange_pair((uintptr_pair_t *)prnt,
				       &old.ui,
				       neu.ui,
				       /*weak=*/false,
				       __ATOMIC_RELEASE,
				       __ATOMIC_RELAXED))
    {
	//CAS succeded, node inserted
	return NULL;
    }
    //CAS failed, unexpected value returned
    return old.he.next;
}

UNROLL_LOOPS ALWAYS_INLINE
static inline bool bucket_insert(struct hash_bucket *bkt,
				 struct p64_hashentry *he,
				 p64_hashvalue_t hash)
{
    uint32_t mask = 0;
    //We want this loop unrolled
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (bkt->entries[i].next == NULL)
	{
	    mask |= 1U << i;
	}
    }
    while (mask != 0)
    {
	uint32_t i = __builtin_ffs(mask) - 1;
	if (insert_node(&bkt->entries[i], he, hash) == NULL)
	{
	    //Success
	    return true;
	}
	mask &= ~(1U << i);
    }
    return false;
}

static void list_insert(struct p64_hashentry *prnt,
			struct p64_hashentry *he,
			p64_hashvalue_t hash,
			int32_t *removed)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    struct p64_hashentry *const org = prnt;
    for (;;)
    {
	struct p64_hashentry *this = hp_acquire((void**)&prnt->next, &hpthis);
	this = REM_MARK(this);
	if (this == NULL)
	{
	    //Next pointer is NULL => end of list, try to swap in our entry
	    struct p64_hashentry *old = insert_node(prnt, he, hash);
	    if (old == NULL)
	    {
		//CAS succeeded, our entry added to end of list
		hp_release(&hpprnt);
		hp_release_ro(&hpthis);
		return;//Entry inserted
	    }
	    //Else CAS failed, next pointer unexpectedly changed
	    if (HAS_MARK(old))
	    {
		//Parent marked for removal and must be removed before we
		//remove 'this'
		//Restart from beginning
		prnt = org;
		continue;
	    }
	    //Other node inserted at this place, try again
	    continue;
	}
	else if (UNLIKELY(this == he))
	{
	    fprintf(stderr, "Object already in hash table\n"), abort();
	}
	else if (UNLIKELY(HAS_MARK(this->next)))
	{
	    //Found other object ('this' != 'he') marked for removal
	    //Let's give a helping hand
	    //FIXME prnt->hash not read atomically with prnt->next, problem?
	    if (remove_node(prnt, this, prnt->hash, removed))
	    {
		//'this' node removed, '*prnt' points to 'next'
		//Continue from current position
		continue;
	    }
	    //Else parent node is also marked for removal
	    //Parent must be removed before we remove 'this'
	    //Restart from beginning
	    prnt = org;
	    continue;
	}
	//Continue search
	prnt = this;
	SWAP(hpprnt, hpthis);
    }
}

void p64_hashtable_insert(struct p64_hashtable *ht,
			  struct p64_hashentry *he,
			  p64_hashvalue_t hash)
{
    int32_t removed = -1;//Assume one node inserted
    uint32_t bix = (hash / BKT_SIZE) % ht->nbkts;
    struct hash_bucket *bkt = &ht->buckets[bix];
    he->hash = 0;
    he->next = NULL;
    bool success = bucket_insert(bkt, he, hash);
    if (!success)
    {
	struct p64_hashentry *prnt = &bkt->entries[hash % BKT_SIZE];
	list_insert(prnt, he, hash, &removed);
    }
#ifndef NDEBUG
    if (removed != 0)
    {
	__atomic_fetch_sub(&ht->nused, removed, __ATOMIC_RELAXED);
    }
#endif
}

UNROLL_LOOPS ALWAYS_INLINE
static inline bool bucket_remove(struct hash_bucket *bkt,
				 struct p64_hashentry *he,
				 p64_hashvalue_t hash,
				 int32_t *removed)
{
    uint32_t mask = 0;
    //We want this loop unrolled
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (bkt->entries[i].next == he)
	{
	    mask |= 1U << i;
	}
    }
    if (mask != 0)
    {
	uint32_t i = __builtin_ffs(mask) - 1;
	struct p64_hashentry *prnt = &bkt->entries[i];
	//No need to hp_acquire(), we already have a reference
	(void)remove_node(prnt, he, hash, removed);
	//Cannot fail due to parent marked for removal
	return true;
    }
    return false;
}

static bool list_remove(struct p64_hashentry *prnt,
			struct p64_hashentry *he,
			p64_hashvalue_t hash,
			int32_t *removed)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpnext = P64_HAZARDPTR_NULL;
    struct p64_hashentry *const org = prnt;
    for (;;)
    {
	struct p64_hashentry *this = hp_acquire((void**)&prnt->next, &hpthis);
	this = REM_MARK(this);
	if (UNLIKELY(this == NULL))
	{
	    //End of list
	    hp_release(&hpprnt);
	    hp_release(&hpthis);
	    hp_release(&hpnext);
	    return false;//Object not found
	}
	else if (this == he)
	{
	    //Found our object, now remove it
	    if (remove_node(prnt, this, hash, removed))
	    {
		//Success, 'this' node is removed
		hp_release(&hpprnt);
		hp_release(&hpthis);
		hp_release(&hpnext);
		return true;
	    }
	    //Else parent node is also marked for removal
	    //Parent must be removed before we remove 'this'
	    //Restart from beginning
	    prnt = org;
	    continue;
	}
	else if (UNLIKELY(HAS_MARK(this->next)))
	{
	    //Found other object ('this' != 'he') marked for removal
	    //Let's give a helping hand
	    //FIXME prnt->hash not read atomically with prnt->next, problem?
	    if (remove_node(prnt, this, prnt->hash, removed))
	    {
		//'this' node removed, '*prnt' points to 'next'
		//Continue from current position
		continue;
	    }
	    //Else parent node is also marked for removal
	    //Parent must be removed before we remove 'this'
	    //Restart from beginning
	    prnt = org;
	    continue;
	}
	//Continue search
	prnt = this;
	SWAP(hpprnt, hpthis);
    }
}

bool p64_hashtable_remove(struct p64_hashtable *ht,
			  struct p64_hashentry *he,
			  p64_hashvalue_t hash)
{
    int32_t removed = 0;
    uint32_t bix = (hash / BKT_SIZE) % ht->nbkts;
    struct hash_bucket *bkt = &ht->buckets[bix];
    bool success = bucket_remove(bkt, he, hash, &removed);
    if (!success)
    {
	struct p64_hashentry *prnt = &bkt->entries[hash % BKT_SIZE];
	success = list_remove(prnt, he, hash, &removed);
    }
#ifndef NDEBUG
    if (removed != 0)
    {
	__atomic_fetch_sub(&ht->nused, removed, __ATOMIC_RELAXED);
    }
#endif
    return success;
}
