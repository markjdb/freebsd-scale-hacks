/*-
 * Copyright (c) 2002-2006 Rice University
 * Copyright (c) 2007-2011 Alan L. Cox <alc@cs.rice.edu>
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Alan L. Cox,
 * Olivier Crameri, Peter Druschel, Sitaram Iyer, and Juan Navarro.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *	Superpage reservation management module
 *
 * Any external functions defined by this module are only to be used by the
 * virtual memory system.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_ddb.h"
#include "opt_vm.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/seq.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_phys.h>
#include <vm/vm_radix.h>
#include <vm/vm_reserv.h>

#include <ddb/ddb.h>

/*
 * The reservation system supports the speculative allocation of large physical
 * pages ("superpages").  Speculative allocation enables the fully automatic
 * utilization of superpages by the virtual memory system.  In other words, no
 * programmatic directives are required to use superpages.
 */

#if VM_NRESERVLEVEL > 0

/*
 * The number of small pages that are contained in a level 0 reservation
 */
#define	VM_LEVEL_0_NPAGES	(1 << VM_LEVEL_0_ORDER)

/*
 * The number of bits by which a physical address is shifted to obtain the
 * reservation number
 */
#define	VM_LEVEL_0_SHIFT	(VM_LEVEL_0_ORDER + PAGE_SHIFT)

/*
 * The size of a level 0 reservation in bytes
 */
#define	VM_LEVEL_0_SIZE		(1 << VM_LEVEL_0_SHIFT)

/*
 * Computes the index of the small page underlying the given (object, pindex)
 * within the reservation's array of small pages.
 */
#define	VM_RESERV_INDEX(object, pindex)	\
    (((object)->pg_color + (pindex)) & (VM_LEVEL_0_NPAGES - 1))

/*
 * The size of a population map entry
 */
typedef	u_long		popmap_t;

/*
 * The number of bits in a population map entry
 */
#define	NBPOPMAP	(NBBY * sizeof(popmap_t))

/*
 * The number of population map entries in a reservation
 */
#define	NPOPMAP		howmany(VM_LEVEL_0_NPAGES, NBPOPMAP)

/*
 * Clear a bit in the population map.
 */
static __inline void
popmap_clear(popmap_t popmap[], int i)
{

	popmap[i / NBPOPMAP] &= ~(1UL << (i % NBPOPMAP));
}

/*
 * Set a bit in the population map.
 */
static __inline void
popmap_set(popmap_t popmap[], int i)
{

	popmap[i / NBPOPMAP] |= 1UL << (i % NBPOPMAP);
}

/*
 * Is a bit in the population map clear?
 */
static __inline boolean_t
popmap_is_clear(popmap_t popmap[], int i)
{

	return ((popmap[i / NBPOPMAP] & (1UL << (i % NBPOPMAP))) == 0);
}

/*
 * Is a bit in the population map set?
 */
static __inline boolean_t
popmap_is_set(popmap_t popmap[], int i)
{

	return ((popmap[i / NBPOPMAP] & (1UL << (i % NBPOPMAP))) != 0);
}

/*
 * The reservation structure
 *
 * A reservation structure is constructed whenever a large physical page is
 * speculatively allocated to an object.  The reservation provides the small
 * physical pages for the range [pindex, pindex + VM_LEVEL_0_NPAGES) of offsets
 * within that object.  The reservation's "popcnt" tracks the number of these
 * small physical pages that are in use at any given time.  When and if the
 * reservation is not fully utilized, it appears in the queue of partially
 * populated reservations.  The reservation always appears on the containing
 * object's list of reservations.
 *
 * A partially populated reservation can be broken and reclaimed at any time.
 */
struct vm_reserv {
	TAILQ_ENTRY(vm_reserv) partpopq;	/* prot. by free page lock */
	LIST_ENTRY(vm_reserv) objq;		/* prot. by free page lock */
	vm_object_t	object;			/* containing object */
	vm_pindex_t	pindex;			/* offset within object */
	vm_page_t	pages;			/* first page of a superpage */
	seq_t		seq;			/* sequence counter for obj */
	uint16_t	popcnt;			/* # of pages in use */
	int8_t		actcnt;			/* activation count */
	uint8_t		flags;			/* state flags */
	popmap_t	popmap[NPOPMAP];	/* bit vector of used pages */
};
CTASSERT(VM_LEVEL_0_ORDER >= sizeof(((struct vm_reserv *)NULL)->popcnt));

#define	VM_RESERV_F_ACTIVE	0x01
#define	VM_RESERV_F_INACTIVE	0x02
#define	VM_RESERV_F_PARTPOP	(VM_RESERV_F_ACTIVE | VM_RESERV_F_INACTIVE)
#define	VM_RESERV_F_MARKER	0x04

#define	RV_INIT		2
#define	RV_POP_STEP	1
#define	RV_DEPOP_STEP	1
#define	RV_DEC		1
#define	RV_ACT_MAX	64

/*
 * The reservation array
 *
 * This array is analoguous in function to vm_page_array.  It differs in the
 * respect that it may contain a greater number of useful reservation
 * structures than there are (physical) superpages.  These "invalid"
 * reservation structures exist to trade-off space for time in the
 * implementation of vm_reserv_from_page().  Invalid reservation structures are
 * distinguishable from "valid" reservation structures by inspecting the
 * reservation's "pages" field.  Invalid reservation structures have a NULL
 * "pages" field.
 *
 * vm_reserv_from_page() maps a small (physical) page to an element of this
 * array by computing a physical reservation number from the page's physical
 * address.  The physical reservation number is used as the array index.
 *
 * An "active" reservation is a valid reservation structure that has a non-NULL
 * "object" field and a non-zero "popcnt" field.  In other words, every active
 * reservation belongs to a particular object.  Moreover, every active
 * reservation has an entry in the containing object's list of reservations.  
 */
static vm_reserv_t vm_reserv_array;

/*
 * Reservation locking
 *
 * Reservations are locked by a combination of the rv_lock array, the free page
 * queue mutex, and VM object write locks. Each reservation also contains a
 * sequence counter used to perform lock-free checks of the "object" and
 * "pindex" fields.
 *
 * Each reservation maps to an element of the rv_lock array; this is the
 * reservation lock. The reservation lock protects most of the reservation's
 * fields, including the object linkage ("object" and "pindex"), the population
 * map, and the flags. When associating a reservation with an object, the
 * object's write lock must also be held. The object lock is not required when a
 * reservation is freed from its object or moved into another object. Active
 * reservations are linked into a list in their corresponding object.
 * Partially-populated active reservations also belong to one of two LRU queues.
 * Access to these lists is synchronized by the free page queue mutex.
 *
 * XXX more
 */

#define	RV_LOCK_COUNT	256
#define	RV_LOCKPTR(rv)	(&rv_lock[(rv - vm_reserv_array) % RV_LOCK_COUNT])

static struct mtx_padalign rv_lock[RV_LOCK_COUNT];

static inline void
vm_reserv_lock(vm_reserv_t rv)
{

	mtx_lock(RV_LOCKPTR(rv));
}

static inline int
vm_reserv_trylock(vm_reserv_t rv)
{

	return (mtx_trylock(RV_LOCKPTR(rv)));
}

static inline void
vm_reserv_unlock(vm_reserv_t rv)
{

	mtx_unlock(RV_LOCKPTR(rv));
}

static inline void
vm_reserv_assert_locked(vm_reserv_t rv)
{

	mtx_assert(RV_LOCKPTR(rv), MA_OWNED);
}

/*
 * The partially populated reservation queue
 *
 * This queue enables the fast recovery of an unused free small page from a
 * partially populated reservation.  The reservation at the head of this queue
 * is the least recently changed, partially populated reservation.
 *
 * XXX locking
 */
static TAILQ_HEAD(, vm_reserv) vm_rvlru_active =
    TAILQ_HEAD_INITIALIZER(vm_rvlru_active);
static TAILQ_HEAD(, vm_reserv) vm_rvlru_inactive =
    TAILQ_HEAD_INITIALIZER(vm_rvlru_inactive);

/* XXX comment */
static struct vm_reserv _scan_marker;
static vm_reserv_t scan_marker;

static SYSCTL_NODE(_vm, OID_AUTO, reserv, CTLFLAG_RD, 0, "Reservation Info");

static long vm_reserv_broken;
SYSCTL_LONG(_vm_reserv, OID_AUTO, broken, CTLFLAG_RD,
    &vm_reserv_broken, 0, "Cumulative number of broken reservations");

static long vm_reserv_freed;
SYSCTL_LONG(_vm_reserv, OID_AUTO, freed, CTLFLAG_RD,
    &vm_reserv_freed, 0, "Cumulative number of freed reservations");

static int sysctl_vm_reserv_fullpop(SYSCTL_HANDLER_ARGS);

SYSCTL_PROC(_vm_reserv, OID_AUTO, fullpop, CTLTYPE_INT | CTLFLAG_RD, NULL, 0,
    sysctl_vm_reserv_fullpop, "I", "Current number of full reservations");

static int sysctl_vm_reserv_partpopq(SYSCTL_HANDLER_ARGS);

SYSCTL_OID(_vm_reserv, OID_AUTO, partpopq, CTLTYPE_STRING | CTLFLAG_RD, NULL, 0,
    sysctl_vm_reserv_partpopq, "A", "Partially populated reservation queues");

static long vm_reserv_reclaimed;
SYSCTL_LONG(_vm_reserv, OID_AUTO, reclaimed, CTLFLAG_RD,
    &vm_reserv_reclaimed, 0, "Cumulative number of reclaimed reservations");

static void		vm_reserv_break(vm_reserv_t rv, vm_page_t m);
static void		vm_reserv_depopulate(vm_reserv_t rv, int index);
static vm_reserv_t	vm_reserv_from_page(vm_page_t m);
static boolean_t	vm_reserv_has_pindex(vm_reserv_t rv,
			    vm_pindex_t pindex);
static void		vm_reserv_populate(vm_reserv_t rv, int index);
static void		vm_reserv_reclaim(vm_reserv_t rv);

/*
 * Returns the current number of full reservations.
 *
 * Since the number of full reservations is computed without acquiring the
 * free page queue lock, the returned value may be inexact.
 */
static int
sysctl_vm_reserv_fullpop(SYSCTL_HANDLER_ARGS)
{
	vm_paddr_t paddr;
	struct vm_phys_seg *seg;
	vm_reserv_t rv;
	int fullpop, segind;

	fullpop = 0;
	for (segind = 0; segind < vm_phys_nsegs; segind++) {
		seg = &vm_phys_segs[segind];
		paddr = roundup2(seg->start, VM_LEVEL_0_SIZE);
		while (paddr + VM_LEVEL_0_SIZE <= seg->end) {
			rv = &vm_reserv_array[paddr >> VM_LEVEL_0_SHIFT];
			fullpop += rv->popcnt == VM_LEVEL_0_NPAGES;
			paddr += VM_LEVEL_0_SIZE;
		}
	}
	return (sysctl_handle_int(oidp, &fullpop, 0, req));
}

/*
 * Describes the current state of the partially populated reservation queue.
 */
static int
sysctl_vm_reserv_partpopq(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sbuf;
	vm_reserv_t rv;
	int counter, error, level, unused_pages;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sbuf, NULL, 128, req);
	sbuf_printf(&sbuf, "\nLEVEL     SIZE  NUMBER\n\n");
	for (level = -1; level <= VM_NRESERVLEVEL - 2; level++) {
		counter = 0;
		unused_pages = 0;
		mtx_lock(&vm_page_queue_free_mtx);
		TAILQ_FOREACH(rv, &vm_rvlru_active/*[level]*/, partpopq) {
			if ((rv->flags & VM_RESERV_F_MARKER) != 0)
				continue;
			counter++;
			unused_pages += VM_LEVEL_0_NPAGES - rv->popcnt;
		}
		mtx_unlock(&vm_page_queue_free_mtx);
		/* XXX this should be humanized */
		sbuf_printf(&sbuf, "ACT %3d: %6dK, %6d\n", level,
		    unused_pages * ((int)PAGE_SIZE / 1024), counter);
		counter = 0;
		unused_pages = 0;
		mtx_lock(&vm_page_queue_free_mtx);
		TAILQ_FOREACH(rv, &vm_rvlru_inactive/*[level]*/, partpopq) {
			counter++;
			unused_pages += VM_LEVEL_0_NPAGES - rv->popcnt;
		}
		mtx_unlock(&vm_page_queue_free_mtx);
		/* XXX this should be humanized */
		sbuf_printf(&sbuf, "INACT %2d: %6dK, %6d\n", level,
		    unused_pages * ((int)PAGE_SIZE / 1024), counter);
	}
	error = sbuf_finish(&sbuf);
	sbuf_delete(&sbuf);
	return (error);
}

/* XXX */
static inline void
vm_reserv_set_object(vm_reserv_t rv, vm_object_t object, vm_pindex_t pindex)
{

	vm_reserv_assert_locked(rv);
	seq_write_begin(&rv->seq);
	if (object != NULL) {
		VM_OBJECT_ASSERT_WLOCKED(object);
		rv->pindex = pindex;
	}
	rv->object = object;
	seq_write_end(&rv->seq);
}

static inline void
vm_reserv_lru_dequeue(vm_reserv_t rv)
{

	KASSERT((rv->flags & VM_RESERV_F_PARTPOP) != 0 &&
	    (rv->flags & VM_RESERV_F_PARTPOP) != VM_RESERV_F_PARTPOP,
	    ("reserv %p not in partpop queues", rv));
	KASSERT((rv->flags & VM_RESERV_F_MARKER) == 0,
	    ("dequeuing marker reservation"));
	if ((rv->flags & VM_RESERV_F_INACTIVE) != 0)
		TAILQ_REMOVE(&vm_rvlru_inactive, rv, partpopq);
	else
		TAILQ_REMOVE(&vm_rvlru_active, rv, partpopq);
	rv->flags &= ~VM_RESERV_F_PARTPOP;
}

static inline void
vm_reserv_update_lru(vm_reserv_t rv, uint8_t advance)
{

	vm_reserv_assert_locked(rv);
	if (rv->popcnt == VM_LEVEL_0_NPAGES) {
		KASSERT((rv->flags & VM_RESERV_F_PARTPOP) != 0 &&
		    (rv->flags & VM_RESERV_F_PARTPOP) != VM_RESERV_F_PARTPOP,
		    ("corrupt reservation flags in %p", rv));
		mtx_lock(&vm_page_queue_free_mtx);
		vm_reserv_lru_dequeue(rv);
		mtx_unlock(&vm_page_queue_free_mtx);
	} else if (rv->popcnt == 0) {
		vm_reserv_set_object(rv, NULL, rv->pindex);
		mtx_lock(&vm_page_queue_free_mtx);
		LIST_REMOVE(rv, objq);
		vm_reserv_lru_dequeue(rv);
		vm_phys_free_pages(rv->pages, VM_LEVEL_0_ORDER);
		mtx_unlock(&vm_page_queue_free_mtx);
		atomic_add_long(&vm_reserv_freed, 1);
	} else if ((rv->flags & VM_RESERV_F_ACTIVE) == 0) {
		rv->actcnt = RV_INIT;
		mtx_lock(&vm_page_queue_free_mtx);
		if ((rv->flags & VM_RESERV_F_INACTIVE) != 0)
			vm_reserv_lru_dequeue(rv);
		TAILQ_INSERT_TAIL(&vm_rvlru_active, rv, partpopq);
		rv->flags |= VM_RESERV_F_ACTIVE;
		mtx_unlock(&vm_page_queue_free_mtx);
	} else {
		rv->actcnt += advance;
		if (rv->actcnt > RV_ACT_MAX)
			rv->actcnt = RV_ACT_MAX;
	}
}

/*
 * Reduces the given reservation's population count.  If the population count
 * becomes zero, the reservation is destroyed.  Additionally, moves the
 * reservation to the tail of the partially populated reservation queue if the
 * population count is non-zero.
 *
 * The free page queue lock must be held.
 */
static void
vm_reserv_depopulate(vm_reserv_t rv, int index)
{

	vm_reserv_assert_locked(rv);
	KASSERT(rv->object != NULL,
	    ("vm_reserv_depopulate: reserv %p is free", rv));
	KASSERT(popmap_is_set(rv->popmap, index),
	    ("vm_reserv_depopulate: reserv %p's popmap[%d] is clear", rv,
	    index));
	KASSERT(rv->popcnt > 0,
	    ("vm_reserv_depopulate: reserv %p's popcnt is corrupted", rv));

	popmap_clear(rv->popmap, index);
	if (rv->popcnt-- == VM_LEVEL_0_NPAGES) {
		KASSERT(rv->pages->psind == 1 &&
		    (rv->flags & VM_RESERV_F_PARTPOP) == 0,
		    ("vm_reserv_depopulate: reserv %p is already demoted", rv));
		rv->pages->psind = 0;
	}
	vm_reserv_update_lru(rv, RV_DEPOP_STEP);
}

/*
 * Increases the given reservation's population count.  Moves the reservation
 * to the tail of the partially populated reservation queue.
 *
 * The free page queue must be locked.
 */
static void
vm_reserv_populate(vm_reserv_t rv, int index)
{

	vm_reserv_assert_locked(rv);
	KASSERT(rv->object != NULL,
	    ("vm_reserv_populate: reserv %p is free", rv));
	KASSERT(popmap_is_clear(rv->popmap, index),
	    ("vm_reserv_populate: reserv %p's popmap[%d] is set", rv,
	    index));
	KASSERT(rv->popcnt < VM_LEVEL_0_NPAGES,
	    ("vm_reserv_populate: reserv %p is already full", rv));
	KASSERT(rv->pages->psind == 0,
	    ("vm_reserv_populate: reserv %p is already promoted", rv));

	popmap_set(rv->popmap, index);
	if (++rv->popcnt == VM_LEVEL_0_NPAGES)
		rv->pages->psind = 1;
	vm_reserv_update_lru(rv, RV_POP_STEP);
}

/*
 * Returns the reservation to which the given page might belong.
 */
static __inline vm_reserv_t
vm_reserv_from_page(vm_page_t m)
{

	return (&vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT]);
}

/*
 * Returns TRUE if the given reservation contains the given page index and
 * FALSE otherwise.
 */
static __inline boolean_t
vm_reserv_has_pindex(vm_reserv_t rv, vm_pindex_t pindex)
{

	return (((pindex - rv->pindex) & ~(VM_LEVEL_0_NPAGES - 1)) == 0);
}

/*
 * Allocates a contiguous set of physical pages of the given size "npages"
 * from existing or newly created reservations.  All of the physical pages
 * must be at or above the given physical address "low" and below the given
 * physical address "high".  The given value "alignment" determines the
 * alignment of the first physical page in the set.  If the given value
 * "boundary" is non-zero, then the set of physical pages cannot cross any
 * physical address boundary that is a multiple of that value.  Both
 * "alignment" and "boundary" must be a power of two.
 *
 * The page "mpred" must immediately precede the offset "pindex" within the
 * specified object.
 *
 * The object and free page queue must be locked.
 */
vm_page_t
vm_reserv_alloc_contig(vm_object_t object, vm_pindex_t pindex, u_long npages,
    vm_paddr_t low, vm_paddr_t high, u_long alignment, vm_paddr_t boundary,
    vm_page_t mpred)
{
	vm_paddr_t pa, size;
	vm_page_t m, m_ret, msucc;
	vm_pindex_t first, leftcap, rightcap;
	vm_reserv_t rv;
	u_long allocpages, maxpages, minpages;
	int i, index, n;
	seq_t seq;

	VM_OBJECT_ASSERT_WLOCKED(object);
	KASSERT(npages != 0, ("vm_reserv_alloc_contig: npages is 0"));

	/*
	 * Is a reservation fundamentally impossible?
	 */
	if (pindex < VM_RESERV_INDEX(object, pindex) ||
	    pindex + npages > object->size)
		return (NULL);

	/*
	 * All reservations of a particular size have the same alignment.
	 * Assuming that the first page is allocated from a reservation, the
	 * least significant bits of its physical address can be determined
	 * from its offset from the beginning of the reservation and the size
	 * of the reservation.
	 *
	 * Could the specified index within a reservation of the smallest
	 * possible size satisfy the alignment and boundary requirements?
	 */
	pa = VM_RESERV_INDEX(object, pindex) << PAGE_SHIFT;
	if ((pa & (alignment - 1)) != 0)
		return (NULL);
	size = npages << PAGE_SHIFT;
	if (((pa ^ (pa + size - 1)) & ~(boundary - 1)) != 0)
		return (NULL);

	first = pindex - VM_RESERV_INDEX(object, pindex);

	/*
	 * Look for an existing reservation.
	 */
	if (mpred != NULL) {
		KASSERT(mpred->object == object,
		    ("vm_reserv_alloc_contig: object doesn't contain mpred"));
		KASSERT(mpred->pindex < pindex,
		    ("vm_reserv_alloc_contig: mpred doesn't precede pindex"));
		rv = vm_reserv_from_page(mpred);
		for (;;) {
			seq = seq_read(&rv->seq);
			if (rv->object == object) {
				if (vm_reserv_has_pindex(rv, pindex)) {
					vm_reserv_lock(rv);
					if (seq_consistent(&rv->seq, seq))
						goto found;
					vm_reserv_unlock(rv);
					continue;
				}
				leftcap = rv->pindex + VM_LEVEL_0_NPAGES;
			} else
				leftcap = mpred->pindex + 1;
			if (seq_consistent(&rv->seq, seq)) {
				if (leftcap > first)
					return (NULL);
				break;
			}
		}

		msucc = TAILQ_NEXT(mpred, listq);
	} else
		msucc = TAILQ_FIRST(&object->memq);

	minpages = VM_RESERV_INDEX(object, pindex) + npages;
	maxpages = roundup2(minpages, VM_LEVEL_0_NPAGES);
	allocpages = maxpages;
	if (msucc != NULL) {
		KASSERT(msucc->pindex > pindex,
		    ("vm_reserv_alloc_contig: pindex already allocated"));
		rv = vm_reserv_from_page(msucc);
		for (;;) {
			seq = seq_read(&rv->seq);
			if (rv->object == object) {
				if (vm_reserv_has_pindex(rv, pindex)) {
					vm_reserv_lock(rv);
					if (seq_consistent(&rv->seq, seq))
						goto found;
					vm_reserv_unlock(rv);
					continue;
				}
				rightcap = rv->pindex;
			} else
				rightcap = msucc->pindex;
			if (seq_consistent(&rv->seq, seq)) {
				if (first + maxpages > rightcap) {
					if (maxpages == VM_LEVEL_0_NPAGES)
						return (NULL);
					/*
					 * At least one reservation will fit
					 * between "leftcap" and "rightcap".
					 * However, a reservation for the last
					 * of the requested pages will not fit.
					 * Reduce the size of the upcoming
					 * allocation accordingly.
					 */
					allocpages = minpages;
				}
				break;
			}
		}
	}

	/*
	 * Would the last new reservation extend past the end of the object?
	 */
	if (first + maxpages > object->size) {
		/*
		 * Don't allocate the last new reservation if the object is a
		 * vnode or backed by another object that is a vnode. 
		 */
		if (object->type == OBJT_VNODE ||
		    (object->backing_object != NULL &&
		    object->backing_object->type == OBJT_VNODE)) {
			if (maxpages == VM_LEVEL_0_NPAGES)
				return (NULL);
			allocpages = minpages;
		}
		/* Speculate that the object may grow. */
	}

	/*
	 * Allocate the physical pages.  The alignment and boundary specified
	 * for this allocation may be different from the alignment and
	 * boundary specified for the requested pages.  For instance, the
	 * specified index may not be the first page within the first new
	 * reservation.
	 */
	mtx_lock(&vm_page_queue_free_mtx);
	m = vm_phys_alloc_contig(allocpages, low, high, ulmax(alignment,
	    VM_LEVEL_0_SIZE), boundary > VM_LEVEL_0_SIZE ? boundary : 0);
	mtx_unlock(&vm_page_queue_free_mtx);
	if (m == NULL)
		return (NULL);

	/*
	 * The allocated physical pages always begin at a reservation
	 * boundary, but they do not always end at a reservation boundary.
	 * Initialize every reservation that is completely covered by the
	 * allocated physical pages.
	 */
	m_ret = NULL;
	index = VM_RESERV_INDEX(object, pindex);
	do {
		rv = vm_reserv_from_page(m);
		vm_reserv_lock(rv);
		KASSERT(rv->pages == m,
		    ("vm_reserv_alloc_contig: reserv %p's pages is corrupted",
		    rv));
		KASSERT(rv->object == NULL,
		    ("vm_reserv_alloc_contig: reserv %p isn't free", rv));
		mtx_lock(&vm_page_queue_free_mtx);
		LIST_INSERT_HEAD(&object->rvq, rv, objq);
		mtx_unlock(&vm_page_queue_free_mtx);
		vm_reserv_set_object(rv, object, first);
		KASSERT(rv->popcnt == 0,
		    ("vm_reserv_alloc_contig: reserv %p's popcnt is corrupted",
		    rv));
		KASSERT((rv->flags & VM_RESERV_F_PARTPOP) == 0,
		    ("vm_reserv_alloc_contig: reserv %p in partpop queues",
		    rv));
		for (i = 0; i < NPOPMAP; i++)
			KASSERT(rv->popmap[i] == 0,
		    ("vm_reserv_alloc_contig: reserv %p's popmap is corrupted",
			    rv));
		n = ulmin(VM_LEVEL_0_NPAGES - index, npages);
		for (i = 0; i < n; i++)
			vm_reserv_populate(rv, index + i);
		vm_reserv_unlock(rv);
		npages -= n;
		if (m_ret == NULL) {
			m_ret = &rv->pages[index];
			index = 0;
		}
		m += VM_LEVEL_0_NPAGES;
		first += VM_LEVEL_0_NPAGES;
		allocpages -= VM_LEVEL_0_NPAGES;
	} while (allocpages >= VM_LEVEL_0_NPAGES);
	return (m_ret);

	/*
	 * Found a matching reservation.
	 */
found:
	index = VM_RESERV_INDEX(object, pindex);
	/* Does the allocation fit within the reservation? */
	if (index + npages > VM_LEVEL_0_NPAGES)
		goto lost;
	m = &rv->pages[index];
	pa = VM_PAGE_TO_PHYS(m);
	if (pa < low || pa + size > high || (pa & (alignment - 1)) != 0 ||
	    ((pa ^ (pa + size - 1)) & ~(boundary - 1)) != 0)
		goto lost;
	/* Handle vm_page_rename(m, new_object, ...). */
	for (i = 0; i < npages; i++)
		if (popmap_is_set(rv->popmap, index + i))
			goto lost;
	for (i = 0; i < npages; i++)
		vm_reserv_populate(rv, index + i);
	vm_reserv_unlock(rv);
	return (m);

lost:
	vm_reserv_unlock(rv);
	return (NULL);
}

/*
 * Allocates a page from an existing or newly created reservation.
 *
 * The page "mpred" must immediately precede the offset "pindex" within the
 * specified object.
 *
 * The object and free page queue must be locked.
 */
vm_page_t
vm_reserv_alloc_page(vm_object_t object, vm_pindex_t pindex, vm_page_t mpred)
{
	vm_page_t m, msucc;
	vm_pindex_t first, leftcap, rightcap;
	vm_reserv_t rv;
	int i, index;
	seq_t seq;

	VM_OBJECT_ASSERT_WLOCKED(object);

	/*
	 * Is a reservation fundamentally impossible?
	 */
	if (pindex < VM_RESERV_INDEX(object, pindex) ||
	    pindex >= object->size)
		return (NULL);

	first = pindex - VM_RESERV_INDEX(object, pindex);

	/*
	 * Look for an existing reservation.
	 * XXX comment
	 */
	if (mpred != NULL) {
		KASSERT(mpred->object == object,
		    ("vm_reserv_alloc_page: object doesn't contain mpred"));
		KASSERT(mpred->pindex < pindex,
		    ("vm_reserv_alloc_page: mpred doesn't precede pindex"));
		rv = vm_reserv_from_page(mpred);
		for (;;) {
			seq = seq_read(&rv->seq);
			if (rv->object == object) {
				if (vm_reserv_has_pindex(rv, pindex)) {
					vm_reserv_lock(rv);
					if (seq_consistent(&rv->seq, seq))
						goto found;
					vm_reserv_unlock(rv);
					continue;
				}
				leftcap = rv->pindex + VM_LEVEL_0_NPAGES;
			} else
				leftcap = mpred->pindex + 1;
			if (seq_consistent(&rv->seq, seq)) {
				if (leftcap > first)
					return (NULL);
				break;
			}
		}

		msucc = TAILQ_NEXT(mpred, listq);
	} else
		msucc = TAILQ_FIRST(&object->memq);
	if (msucc != NULL) {
		KASSERT(msucc->pindex > pindex,
		    ("vm_reserv_alloc_page: msucc doesn't succeed pindex"));
		rv = vm_reserv_from_page(msucc);
		for (;;) {
			seq = seq_read(&rv->seq);
			if (rv->object == object) {
				if (vm_reserv_has_pindex(rv, pindex)) {
					vm_reserv_lock(rv);
					if (seq_consistent(&rv->seq, seq))
						goto found;
					vm_reserv_unlock(rv);
					continue;
				}
				rightcap = rv->pindex;
			} else
				rightcap = msucc->pindex;
			if (seq_consistent(&rv->seq, seq)) {
				if (first + VM_LEVEL_0_NPAGES > rightcap)
					return (NULL);
				break;
			}
		}
	}

	/*
	 * Would a new reservation extend past the end of the object? 
	 */
	if (first + VM_LEVEL_0_NPAGES > object->size) {
		/*
		 * Don't allocate a new reservation if the object is a vnode or
		 * backed by another object that is a vnode. 
		 */
		if (object->type == OBJT_VNODE ||
		    (object->backing_object != NULL &&
		    object->backing_object->type == OBJT_VNODE))
			return (NULL);
		/* Speculate that the object may grow. */
	}

	/*
	 * Allocate and populate the new reservation.
	 */
	mtx_lock(&vm_page_queue_free_mtx);
	m = vm_phys_alloc_pages(VM_FREEPOOL_DEFAULT, VM_LEVEL_0_ORDER);
	mtx_unlock(&vm_page_queue_free_mtx);
	if (m == NULL)
		return (NULL);
	rv = vm_reserv_from_page(m);
	vm_reserv_lock(rv);
	KASSERT(rv->pages == m,
	    ("vm_reserv_alloc_page: reserv %p's pages is corrupted", rv));
	KASSERT(rv->object == NULL,
	    ("vm_reserv_alloc_page: reserv %p isn't free", rv));
	mtx_lock(&vm_page_queue_free_mtx);
	LIST_INSERT_HEAD(&object->rvq, rv, objq);
	mtx_unlock(&vm_page_queue_free_mtx);
	vm_reserv_set_object(rv, object, first);
	KASSERT(rv->popcnt == 0,
	    ("vm_reserv_alloc_page: reserv %p's popcnt is corrupted", rv));
	KASSERT((rv->flags & VM_RESERV_F_PARTPOP) == 0,
	    ("vm_reserv_alloc_contig: reserv %p in partpop queues", rv));
	for (i = 0; i < NPOPMAP; i++)
		KASSERT(rv->popmap[i] == 0,
		    ("vm_reserv_alloc_page: reserv %p's popmap is corrupted",
		    rv));
	index = VM_RESERV_INDEX(object, pindex);
	vm_reserv_populate(rv, index);
	vm_reserv_unlock(rv);
	return (&rv->pages[index]);

	/*
	 * Found a matching reservation.
	 */
found:
	index = VM_RESERV_INDEX(object, pindex);
	m = &rv->pages[index];
	/* Handle vm_page_rename(m, new_object, ...). */
	if (popmap_is_set(rv->popmap, index))
		goto lost;
	vm_reserv_populate(rv, index);
	vm_reserv_unlock(rv);
	return (m);

lost:
	vm_reserv_unlock(rv);
	return (NULL);
}

/*
 * Breaks the given reservation.  Except for the specified free page, all free
 * pages in the reservation are returned to the physical memory allocator.
 * The reservation's population count and map are reset to their initial
 * state.
 *
 * The given reservation must not be in the partially populated reservation
 * queue.  The free page queue lock must be held.
 */
static void
vm_reserv_break(vm_reserv_t rv, vm_page_t m)
{
	int begin_zeroes, hi, i, lo;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	vm_reserv_assert_locked(rv);
	KASSERT(rv->object != NULL,
	    ("vm_reserv_break: reserv %p is free", rv));
	KASSERT((rv->flags & VM_RESERV_F_PARTPOP) == 0,
	    ("vm_reserv_alloc_contig: reserv %p in partpop queues", rv));
	LIST_REMOVE(rv, objq);
	vm_reserv_set_object(rv, NULL, rv->pindex);
	if (m != NULL) {
		/*
		 * Since the reservation is being broken, there is no harm in
		 * abusing the population map to stop "m" from being returned
		 * to the physical memory allocator.
		 */
		i = m - rv->pages;
		KASSERT(popmap_is_clear(rv->popmap, i),
		    ("vm_reserv_break: reserv %p's popmap is corrupted", rv));
		popmap_set(rv->popmap, i);
		rv->popcnt++;
	}
	i = hi = 0;
	do {
		/* Find the next 0 bit.  Any previous 0 bits are < "hi". */
		lo = ffsl(~(((1UL << hi) - 1) | rv->popmap[i]));
		if (lo == 0) {
			/* Redundantly clears bits < "hi". */
			rv->popmap[i] = 0;
			rv->popcnt -= NBPOPMAP - hi;
			while (++i < NPOPMAP) {
				lo = ffsl(~rv->popmap[i]);
				if (lo == 0) {
					rv->popmap[i] = 0;
					rv->popcnt -= NBPOPMAP;
				} else
					break;
			}
			if (i == NPOPMAP)
				break;
			hi = 0;
		}
		KASSERT(lo > 0, ("vm_reserv_break: lo is %d", lo));
		/* Convert from ffsl() to ordinary bit numbering. */
		lo--;
		if (lo > 0) {
			/* Redundantly clears bits < "hi". */
			rv->popmap[i] &= ~((1UL << lo) - 1);
			rv->popcnt -= lo - hi;
		}
		begin_zeroes = NBPOPMAP * i + lo;
		/* Find the next 1 bit. */
		do
			hi = ffsl(rv->popmap[i]);
		while (hi == 0 && ++i < NPOPMAP);
		if (i != NPOPMAP)
			/* Convert from ffsl() to ordinary bit numbering. */
			hi--;
		vm_phys_free_contig(&rv->pages[begin_zeroes], NBPOPMAP * i +
		    hi - begin_zeroes);
	} while (i < NPOPMAP);
	KASSERT(rv->popcnt == 0,
	    ("vm_reserv_break: reserv %p's popcnt is corrupted", rv));
	atomic_add_long(&vm_reserv_broken, 1);
}

/*
 * Breaks all reservations belonging to the given object.
 */
void
vm_reserv_break_all(vm_object_t object)
{
	vm_reserv_t rv, next;

	VM_OBJECT_ASSERT_WLOCKED(object);

	mtx_lock(&vm_page_queue_free_mtx);
	while ((rv = LIST_FIRST(&object->rvq)) != NULL) {
		if (!vm_reserv_trylock(rv)) {
			mtx_unlock(&vm_page_queue_free_mtx);
			vm_reserv_lock(rv);
			mtx_lock(&vm_page_queue_free_mtx);
			next = LIST_FIRST(&object->rvq);
			if (rv != next) {
				if (RV_LOCKPTR(next) != RV_LOCKPTR(rv)) {
					vm_reserv_unlock(rv);
					continue;
				}
				rv = next;
			}
		}
quick:
		KASSERT(rv->object == object,
		    ("vm_reserv_break_all: reserv %p is corrupted", rv));
		if ((rv->flags & VM_RESERV_F_PARTPOP) != 0)
			vm_reserv_lru_dequeue(rv);
		vm_reserv_break(rv, NULL);
		if ((next = LIST_FIRST(&object->rvq)) != NULL &&
		    RV_LOCKPTR(next) == RV_LOCKPTR(rv)) {
			rv = next;
			goto quick;
		}
		vm_reserv_unlock(rv);
	}
	mtx_unlock(&vm_page_queue_free_mtx);
}

/*
 * Frees the given page if it belongs to a reservation.  Returns TRUE if the
 * page is freed and FALSE otherwise.
 *
 * The free page queue lock must be held.
 */
boolean_t
vm_reserv_free_page(vm_page_t m)
{
	vm_reserv_t rv;

	if (m->object != NULL)
		VM_OBJECT_ASSERT_WLOCKED(m->object);

	rv = vm_reserv_from_page(m);
	/*
	 * Since we hold the object write lock, we know that a thread isn't
	 * concurrently setting rv->object to object.
	 */
	if (rv->object == NULL)
		return (FALSE);
	vm_reserv_lock(rv);
	vm_reserv_depopulate(rv, m - rv->pages);
	vm_reserv_unlock(rv);
	return (TRUE);
}

/*
 * Initializes the reservation management system.  Specifically, initializes
 * the reservation array.
 *
 * Requires that vm_page_array and first_page are initialized!
 */
void
vm_reserv_init(void)
{
	vm_paddr_t paddr;
	struct vm_phys_seg *seg;
	int i, segind;

	/*
	 * Initialize the reservation array.  Specifically, initialize the
	 * "pages" field for every element that has an underlying superpage.
	 */
	for (segind = 0; segind < vm_phys_nsegs; segind++) {
		seg = &vm_phys_segs[segind];
		paddr = roundup2(seg->start, VM_LEVEL_0_SIZE);
		while (paddr + VM_LEVEL_0_SIZE <= seg->end) {
			vm_reserv_array[paddr >> VM_LEVEL_0_SHIFT].pages =
			    PHYS_TO_VM_PAGE(paddr);
			paddr += VM_LEVEL_0_SIZE;
		}
	}
	for (i = 0; i < RV_LOCK_COUNT; i++)
		mtx_init(&rv_lock[i], "reserv", NULL, MTX_DEF);

	scan_marker = &_scan_marker;
	scan_marker->object = NULL;
	scan_marker->flags = VM_RESERV_F_MARKER | VM_RESERV_F_ACTIVE;
	TAILQ_INSERT_HEAD(&vm_rvlru_active, scan_marker, partpopq);
}

/*
 * Returns true if the given page belongs to a reservation and that page is
 * free.  Otherwise, returns false.
 */
bool
vm_reserv_is_page_free(vm_page_t m)
{
	vm_reserv_t rv;

	rv = vm_reserv_from_page(m);
	if (rv->object == NULL)
		return (false);
	return (popmap_is_clear(rv->popmap, m - rv->pages));
}

/*
 * If the given page belongs to a reservation, returns the level of that
 * reservation.  Otherwise, returns -1.
 */
int
vm_reserv_level(vm_page_t m)
{
	vm_reserv_t rv;

	rv = vm_reserv_from_page(m);
	return (rv->object != NULL ? 0 : -1);
}

/*
 * Returns a reservation level if the given page belongs to a fully populated
 * reservation and -1 otherwise.
 */
int
vm_reserv_level_iffullpop(vm_page_t m)
{
	vm_reserv_t rv;

	rv = vm_reserv_from_page(m);
	return (rv->popcnt == VM_LEVEL_0_NPAGES ? 0 : -1);
}

/*
 * Breaks the given partially populated reservation, releasing its free pages
 * to the physical memory allocator.
 *
 * The free page queue lock must be held.
 */
static void
vm_reserv_reclaim(vm_reserv_t rv)
{

	vm_reserv_assert_locked(rv);
	KASSERT((rv->flags & VM_RESERV_F_PARTPOP) != 0,
	    ("vm_reserv_reclaim: reserv %p not in partpop queues", rv));

	vm_reserv_lru_dequeue(rv);
	vm_reserv_break(rv, NULL);
	vm_reserv_reclaimed++;
}

/*
 * Breaks the reservation at the head of the partially populated reservation
 * queue, releasing its free pages to the physical memory allocator.  Returns
 * TRUE if a reservation is broken and FALSE otherwise.
 *
 * The free page queue lock must be held, and may be dropped before returning.
 */
boolean_t
vm_reserv_reclaim_inactive(void)
{
	vm_reserv_t rv, tmp;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
restart:
	TAILQ_FOREACH_SAFE(rv, &vm_rvlru_inactive, partpopq, tmp) {
		if (!vm_reserv_trylock(rv)) {
			mtx_unlock(&vm_page_queue_free_mtx);
			vm_reserv_lock(rv);
			if ((rv->flags & VM_RESERV_F_INACTIVE) == 0) {
				vm_reserv_unlock(rv);
				goto restart;
			}
			mtx_lock(&vm_page_queue_free_mtx);
		}
		vm_reserv_reclaim(rv);
		vm_reserv_unlock(rv);
		return (TRUE);
	}
	/* XXX duplication, and incorrect for the active queue */
	TAILQ_FOREACH_SAFE(rv, &vm_rvlru_active, partpopq, tmp) {
		if ((rv->flags & VM_RESERV_F_MARKER) != 0)
			continue;
		if (!vm_reserv_trylock(rv)) {
			mtx_unlock(&vm_page_queue_free_mtx);
			vm_reserv_lock(rv);
			if ((rv->flags & VM_RESERV_F_ACTIVE) == 0) {
				vm_reserv_unlock(rv);
				goto restart;
			}
			mtx_lock(&vm_page_queue_free_mtx);
		}
		vm_reserv_reclaim(rv);
		vm_reserv_unlock(rv);
		return (TRUE);
	}
	return (FALSE);
}

/*
 * Searches the partially populated reservation queue for the least recently
 * changed reservation with free pages that satisfy the given request for
 * contiguous physical memory.  If a satisfactory reservation is found, it is
 * broken.  Returns TRUE if a reservation is broken and FALSE otherwise.
 *
 * The free page queue lock must be held.
 *
 * XXX don't break the LRU
 */
boolean_t
vm_reserv_reclaim_contig(u_long npages, vm_paddr_t low, vm_paddr_t high,
    u_long alignment, vm_paddr_t boundary)
{
	vm_paddr_t pa, size;
	vm_reserv_t rv;
	int hi, i, lo, low_index, next_free;

	MPASS(0);

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	if (npages > VM_LEVEL_0_NPAGES - 1)
		return (FALSE);
	size = npages << PAGE_SHIFT;
	TAILQ_FOREACH(rv, &vm_rvlru_inactive, partpopq) {
		pa = VM_PAGE_TO_PHYS(&rv->pages[VM_LEVEL_0_NPAGES - 1]);
		if (pa + PAGE_SIZE - size < low) {
			/* This entire reservation is too low; go to next. */
			continue;
		}
		pa = VM_PAGE_TO_PHYS(&rv->pages[0]);
		if (pa + size > high) {
			/* This entire reservation is too high; go to next. */
			continue;
		}
		if (pa < low) {
			/* Start the search for free pages at "low". */
			low_index = (low + PAGE_MASK - pa) >> PAGE_SHIFT;
			i = low_index / NBPOPMAP;
			hi = low_index % NBPOPMAP;
		} else
			i = hi = 0;
		do {
			/* Find the next free page. */
			lo = ffsl(~(((1UL << hi) - 1) | rv->popmap[i]));
			while (lo == 0 && ++i < NPOPMAP)
				lo = ffsl(~rv->popmap[i]);
			if (i == NPOPMAP)
				break;
			/* Convert from ffsl() to ordinary bit numbering. */
			lo--;
			next_free = NBPOPMAP * i + lo;
			pa = VM_PAGE_TO_PHYS(&rv->pages[next_free]);
			KASSERT(pa >= low,
			    ("vm_reserv_reclaim_contig: pa is too low"));
			if (pa + size > high) {
				/* The rest of this reservation is too high. */
				break;
			} else if ((pa & (alignment - 1)) != 0 ||
			    ((pa ^ (pa + size - 1)) & ~(boundary - 1)) != 0) {
				/*
				 * The current page doesn't meet the alignment
				 * and/or boundary requirements.  Continue
				 * searching this reservation until the rest
				 * of its free pages are either excluded or
				 * exhausted.
				 */
				hi = lo + 1;
				if (hi >= NBPOPMAP) {
					hi = 0;
					i++;
				}
				continue;
			}
			/* Find the next used page. */
			hi = ffsl(rv->popmap[i] & ~((1UL << lo) - 1));
			while (hi == 0 && ++i < NPOPMAP) {
				if ((NBPOPMAP * i - next_free) * PAGE_SIZE >=
				    size) {
					vm_reserv_reclaim(rv);
					return (TRUE);
				}
				hi = ffsl(rv->popmap[i]);
			}
			/* Convert from ffsl() to ordinary bit numbering. */
			if (i != NPOPMAP)
				hi--;
			if ((NBPOPMAP * i + hi - next_free) * PAGE_SIZE >=
			    size) {
				vm_reserv_reclaim(rv);
				return (TRUE);
			}
		} while (i < NPOPMAP);
	}
	return (FALSE);
}

/*
 * Transfers the reservation underlying the given page to a new object.
 *
 * The object must be locked.
 */
void
vm_reserv_rename(vm_page_t m, vm_object_t new_object, vm_object_t old_object,
    vm_pindex_t old_object_offset)
{
	vm_reserv_t rv;

	VM_OBJECT_ASSERT_WLOCKED(new_object);
	rv = vm_reserv_from_page(m);
	if (rv->object == old_object) {
		vm_reserv_lock(rv);
		if (rv->object == old_object) {
			mtx_lock(&vm_page_queue_free_mtx);
			LIST_REMOVE(rv, objq);
			LIST_INSERT_HEAD(&new_object->rvq, rv, objq);
			mtx_unlock(&vm_page_queue_free_mtx);
			vm_reserv_set_object(rv, new_object,
			    rv->pindex - old_object_offset);
		}
		vm_reserv_unlock(rv);
	}
}

/*
 * Returns the size (in bytes) of a reservation of the specified level.
 */
int
vm_reserv_size(int level)
{

	switch (level) {
	case 0:
		return (VM_LEVEL_0_SIZE);
	case -1:
		return (PAGE_SIZE);
	default:
		return (0);
	}
}

/*
 * XXX
 */
void
vm_reserv_scan(struct vm_domain *vmd, int target)
{
	vm_reserv_t rv, marker, tmp;

	marker = scan_marker;

	mtx_lock(&vm_page_queue_free_mtx);
	rv = TAILQ_NEXT(marker, partpopq);
	/* If rv == NULL, we'll start from the beginning. */
	TAILQ_FOREACH_FROM_SAFE(rv, &vm_rvlru_active, partpopq, tmp) {
		if (target == 0)
			break;
		if ((rv->flags & VM_RESERV_F_MARKER) != 0)
			continue;
		if (!vm_reserv_trylock(rv))
			continue;
		if (rv->actcnt - RV_DEC <= 0) {
			vm_reserv_lru_dequeue(rv);
			TAILQ_INSERT_TAIL(&vm_rvlru_inactive, rv, partpopq);
			rv->flags |= VM_RESERV_F_INACTIVE;
			rv->actcnt = 0;
			target--;
		} else
			rv->actcnt -= RV_DEC;
		vm_reserv_unlock(rv);
	}

	/* Update the placeholder. */
	TAILQ_REMOVE(&vm_rvlru_active, marker, partpopq);
	if (rv != NULL)
		TAILQ_INSERT_BEFORE(rv, marker, partpopq);
	else
		TAILQ_INSERT_HEAD(&vm_rvlru_active, marker, partpopq);
	mtx_unlock(&vm_page_queue_free_mtx);
}

/*
 * Allocates the virtual and physical memory required by the reservation
 * management system's data structures, in particular, the reservation array.
 */
vm_paddr_t
vm_reserv_startup(vm_offset_t *vaddr, vm_paddr_t end, vm_paddr_t high_water)
{
	vm_paddr_t new_end;
	size_t n, size;

	/*
	 * Calculate the size (in bytes) of the reservation array.  Round up
	 * from "high_water" because every small page is mapped to an element
	 * in the reservation array based on its physical address.  Thus, the
	 * number of elements in the reservation array can be greater than the
	 * number of superpages. 
	 */
	n = howmany(high_water, VM_LEVEL_0_SIZE);
	size = n * sizeof(struct vm_reserv);
	if (bootverbose)
		printf("vm_reserv_startup: allocating %zd reservations\n", n);

	/*
	 * Allocate and map the physical memory for the reservation array.  The
	 * next available virtual address is returned by reference.
	 */
	new_end = end - round_page(size);
	vm_reserv_array = (void *)(uintptr_t)pmap_map(vaddr, new_end, end,
	    VM_PROT_READ | VM_PROT_WRITE);
	bzero(vm_reserv_array, size);

	/*
	 * Return the next available physical address.
	 */
	return (new_end);
}

DB_SHOW_COMMAND(reserv, vm_reserv_print)
{
	vm_reserv_t rv;

	rv = (vm_reserv_t)addr;

	db_printf("object: %p, popcnt: %d, actcnt: %d, flags: %#x\n",
	    rv->object, rv->popcnt, rv->actcnt, rv->flags);
	db_printf("first page: %p\n", rv->pages);
}

#endif	/* VM_NRESERVLEVEL > 0 */
