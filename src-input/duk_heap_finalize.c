/*
 *  Finalizer handling
 */

#include "duk_internal.h"

#if defined(DUK_USE_FINALIZER_SUPPORT)

/*
 *  Fake torture finalizer.
 */

#if defined(DUK_USE_FINALIZER_TORTURE)
DUK_LOCAL duk_ret_t duk__fake_global_finalizer(duk_context *ctx) {
	DUK_DD(DUK_DDPRINT("fake global torture finalizer executed"));

	/* Require a lot of stack to force a value stack grow/shrink. */
	duk_require_stack(ctx, 100000);

	/* XXX: do something to force a callstack grow/shrink, perhaps
	 * just a manual forced resize or a forced relocating realloc?
	 */

	/* Inner function call, error throw. */
	duk_eval_string_noresult(ctx,
		"(function dummy() {\n"
		"    dummy.prototype = null;  /* break reference loop */\n"
		"    try {\n"
		"        throw 'fake-finalizer-dummy-error';\n"
		"    } catch (e) {\n"
		"        void e;\n"
		"    }\n"
		"})()");

	/* The above creates garbage (e.g. a function instance).  Because
	 * the function/prototype reference loop is broken, it gets collected
	 * immediately by DECREF.  If Function.prototype has a _Finalizer
	 * property (happens in some test cases), the garbage gets queued to
	 * finalize_list.  This still won't cause an infinite loop because
	 * the torture finalizer is called once per finalize_list run and
	 * the garbage gets handled in the same run.  (If the garbage needs
	 * mark-and-sweep collection, an infinite loop might ensue.)
	 */
	return 0;
}

DUK_LOCAL void duk__run_global_torture_finalizer(duk_hthread *thr) {
	DUK_ASSERT(thr != NULL);

	/* Avoid fake finalization when callstack limit has been reached.
	 * Otherwise a callstack limit error will be created, then refzero'ed.
	 */
	if (thr->heap->call_recursion_depth >= thr->heap->call_recursion_limit ||
	    thr->callstack_size + 2 * DUK_CALLSTACK_GROW_STEP >= thr->callstack_max /*approximate*/) {
		DUK_D(DUK_DPRINT("skip global torture finalizer because of call recursion or call stack size limit"));
		return;
	}

	/* Run fake finalizer.  Avoid creating unnecessary garbage. */
	duk_push_c_function((duk_context *) thr, duk__fake_global_finalizer, 0 /*nargs*/);
	(void) duk_pcall((duk_context *) thr, 0 /*nargs*/);
	duk_pop((duk_context *) thr);
}
#endif  /* DUK_USE_FINALIZER_TORTURE */

/*
 *  Process the finalize_list to completion.
 *
 *  An object may be placed on finalize_list by either refcounting or
 *  mark-and-sweep.  The refcount of objects placed by refcounting will be
 *  zero; the refcount of objects placed by mark-and-sweep is > 0.
 *
 *  List processing assumes refcounts are kept up-to-date at all times, so
 *  that once the finalizer returns, a zero refcount is a reliable reason to
 *  free the object immediately rather than place it back to the heap.  This
 *  is the case because we run outside of refzero_list processing so that
 *  DECREF cascades are handled fully inline.
 *
 *  For mark-and-sweep queued objects (had_zero_refcount false) the object
 *  may be freed immediately if its refcount is zero after the finalizer call
 *  (i.e. finalizer removed the reference loop for the object).  If not, the
 *  next mark-and-sweep will collect the object unless it has become reachable
 *  (i.e. rescued) by that time and its refcount hasn't fallen to zero before
 *  that.  Mark-and-sweep detects these objects because their FINALIZED flag
 *  is set.
 *
 *  There's an inherent limitation for mark-and-sweep finalizer rescuing: an
 *  object won't get refinalized if (1) it's rescued, but (2) becomes
 *  unreachable before mark-and-sweep has had time to notice it.  The next
 *  mark-and-sweep round simply doesn't have any information of whether the
 *  object has been unreachable the whole time or not (the only way to get
 *  that information would be a mark-and-sweep pass for *every finalized
 *  object*).  This is awkward for the application because the mark-and-sweep
 *  round is not generally visible or under full application control.
 *
 *  For refcount queued objects (had_zero_refcount true) the object is either
 *  immediately freed or rescued, and waiting for a mark-and-sweep round is not
 *  necessary (or desirable); FINALIZED is cleared when a rescued object is
 *  queued back to heap_allocated.  The object is eligible for finalization
 *  again (either via refcounting or mark-and-sweep) immediately after being
 *  rescued.  If a refcount finalized object is placed into an unreachable
 *  reference loop by its finalizer, it will get collected by mark-and-sweep
 *  and currently the finalizer will execute again.
 *
 *  There's a special case where:
 *
 *    - Mark-and-sweep queues an object to finalize_list for finalization.
 *    - The finalizer is executed, FINALIZED is set, and object is queued
 *      back to heap_allocated, waiting for a new mark-and-sweep round.
 *    - The object's refcount drops to zero before mark-and-sweep has a
 *      chance to run another round and make a rescue/free decision.
 *
 *  This is now handled by refzero code: if an object has a finalizer but
 *  FINALIZED is already set, the object is freed without finalizer processing.
 *  The outcome is the same as if mark-and-sweep was executed at that point;
 *  mark-and-sweep would also free the object without another finalizer run.
 *  This could also be changed so that the refzero-triggered finalizer *IS*
 *  executed: being refzero collected implies someone has operated on the
 *  object so it hasn't been totally unreachable the whole time.  This would
 *  risk a finalizer loop however.
 */

DUK_INTERNAL void duk_heap_process_finalize_list(duk_heap *heap) {
	duk_heaphdr *curr;
#if defined(DUK_USE_DEBUG)
	duk_size_t count = 0;
#endif

	DUK_DDD(DUK_DDDPRINT("duk_heap_process_finalize_list: %p", (void *) heap));

	if (heap->pf_prevent_count != 0) {
		DUK_DDD(DUK_DDDPRINT("skip finalize_list processing: pf_prevent_count != 0"));
		return;
	}

	/* Heap alloc prevents mark-and-sweep before heap_thread is ready. */
	DUK_ASSERT(heap != NULL);
	DUK_ASSERT(heap->heap_thread != NULL);
	DUK_ASSERT(heap->heap_thread->valstack != NULL);
	DUK_ASSERT(heap->heap_thread->callstack != NULL);
	DUK_ASSERT(heap->heap_thread->catchstack != NULL);
#if defined(DUK_USE_REFERENCE_COUNTING)
	DUK_ASSERT(heap->refzero_list == NULL);
#endif

	DUK_ASSERT(heap->pf_prevent_count == 0);
	heap->pf_prevent_count = 1;

	/* Bump ms_prevent_count to prevent mark-and-sweep while we execute
	 * finalizers.  It's important for no mark-and-sweep passes to happen
	 * while we process the finalize_list.  If a part of the finalize_list
	 * has been processed and mark-and-sweep runs, it will incorrectly
	 * consider the processed objects rescued if they are in a reference
	 * relationship with objects still in finalize_list.  This happens
	 * because mark-and-sweep treats the whole finalize_list as being
	 * "reachable".
	 */
	heap->ms_prevent_count++;
	DUK_ASSERT(heap->ms_prevent_count != 0);  /* Wrap. */

	/* When finalizer torture is enabled, make a fake finalizer call with
	 * maximum side effects regardless of whether finalize_list is empty.
	 */
#if defined(DUK_USE_FINALIZER_TORTURE)
	duk__run_global_torture_finalizer(heap->heap_thread);
#endif

	/* Process finalize_list until it becomes empty.  There's currently no
	 * protection against a finalizer always creating more garbage.
	 */
	while ((curr = heap->finalize_list) != NULL) {
#if defined(DUK_USE_REFERENCE_COUNTING)
		duk_bool_t queue_back;
#endif

		DUK_DD(DUK_DDPRINT("processing finalize_list entry: %p -> %!iO", (void *) curr, curr));

		DUK_ASSERT(DUK_HEAPHDR_GET_TYPE(curr) == DUK_HTYPE_OBJECT);  /* Only objects have finalizers. */
		DUK_ASSERT(!DUK_HEAPHDR_HAS_REACHABLE(curr));
		DUK_ASSERT(!DUK_HEAPHDR_HAS_TEMPROOT(curr));
		DUK_ASSERT(DUK_HEAPHDR_HAS_FINALIZABLE(curr));  /* All objects on finalize_list will have this flag. */
		DUK_ASSERT(!DUK_HEAPHDR_HAS_FINALIZED(curr));   /* Queueing code ensures. */
		DUK_ASSERT(!DUK_HEAPHDR_HAS_READONLY(curr));  /* ROM objects never get freed (or finalized). */

		/* Clear FINALIZABLE for object being finalized, so that
		 * duk_push_heapptr() can properly ignore the object.
		 */
		DUK_HEAPHDR_CLEAR_FINALIZABLE(curr);

		/* XXX: Instead of an artificial refcount bump, could also
		 * push/pop the object for reachability.  This doesn't really
		 * matter much because the object is still on finalize_list
		 * and treated as reachable by mark-and-sweep.
		 */

		if (DUK_LIKELY(!heap->pf_skip_finalizers)) {
			/* Run the finalizer, duk_heap_run_finalizer() sets
			 * and checks for FINALIZED to prevent the finalizer
			 * from executing multiple times per finalization cycle.
			 * (This safeguard shouldn't be actually needed anymore).
			 */

#if defined(DUK_USE_REFERENCE_COUNTING)
			duk_bool_t had_zero_refcount;
#endif

			/* Ensure object's refcount is >0 throughout so it
			 * won't be refzero processed prematurely.
			 */
#if defined(DUK_USE_REFERENCE_COUNTING)
			had_zero_refcount = (DUK_HEAPHDR_GET_REFCOUNT(curr) == 0);
			DUK_HEAPHDR_PREINC_REFCOUNT(curr);
#endif

			DUK_ASSERT(!DUK_HEAPHDR_HAS_FINALIZED(curr));
			duk_heap_run_finalizer(heap, (duk_hobject *) curr);  /* must never longjmp */
			DUK_ASSERT(DUK_HEAPHDR_HAS_FINALIZED(curr));
			/* XXX: assert that object is still in finalize_list
			 * when duk_push_heapptr() allows automatic rescue.
			 */

#if defined(DUK_USE_REFERENCE_COUNTING)
			DUK_ASSERT(DUK_HEAPHDR_GET_REFCOUNT(curr) >= 1);  /* ref on value stack at least */
			DUK_HEAPHDR_PREDEC_REFCOUNT(curr);
			DUK_DD(DUK_DDPRINT("refcount after finalizer: %ld", (long) DUK_HEAPHDR_GET_REFCOUNT(curr)));
			if (DUK_HEAPHDR_GET_REFCOUNT(curr) == 0) {
#if defined(DUK_USE_DEBUG)
				if (had_zero_refcount) {
					DUK_DD(DUK_DDPRINT("finalized object's refcount is zero -> free immediately (refcount queued)"));
				} else {
					DUK_DD(DUK_DDPRINT("finalized object's refcount is zero -> free immediately (mark-and-sweep queued)"));
				}
#endif
				queue_back = 0;
			} else
#endif
			{
#if defined(DUK_USE_REFERENCE_COUNTING)
				queue_back = 1;
				if (had_zero_refcount) {
					/* When finalization is triggered
					 * by refzero and we queue the object
					 * back, clear FINALIZED right away
					 * so that the object can be refinalized
					 * immediately if necessary.
					 */
					DUK_HEAPHDR_CLEAR_FINALIZED(curr);
				}
#endif
			}
		} else {
			/* Used during heap destruction: don't actually run finalizers
			 * because we're heading into forced finalization.  Instead,
			 * queue finalizable objects back to the heap_allocated list.
			 */
			DUK_D(DUK_DPRINT("skip finalizers flag set, queue object to heap_allocated without finalizing"));
			DUK_ASSERT(!DUK_HEAPHDR_HAS_FINALIZED(curr));
#if defined(DUK_USE_REFERENCE_COUNTING)
			queue_back = 1;
#endif
		}

		/* Dequeue object from finalize_list.  Note that 'curr' may no
		 * longer be finalize_list head because new objects may have
		 * been queued to the list.  As a result we can't optimize for
		 * the single-linked heap case and must scan the list for
		 * removal, typically the scan is very short however.
		 */
		DUK_HEAP_REMOVE_FROM_FINALIZE_LIST(heap, curr);

		/* Queue back to heap_allocated or free immediately. */
#if defined(DUK_USE_REFERENCE_COUNTING)
		if (queue_back) {
			/* FINALIZED is only cleared if object originally
			 * queued for finalization by refcounting.  For
			 * mark-and-sweep FINALIZED is left set, so that
			 * next mark-and-sweep round can make a rescue/free
			 * decision.
			 */
			DUK_HEAPHDR_CLEAR_FINALIZABLE(curr);
			DUK_HEAP_INSERT_INTO_HEAP_ALLOCATED(heap, curr);
		} else {
			DUK_ASSERT(DUK_HEAPHDR_GET_TYPE(curr) == DUK_HTYPE_OBJECT);  /* currently, always the case */
			DUK_DD(DUK_DDPRINT("refcount finalize after finalizer call: %!O", curr));
			duk_hobject_refcount_finalize_norz(heap, (duk_hobject *) curr);
			duk_free_hobject(heap, (duk_hobject *) curr);
			DUK_DD(DUK_DDPRINT("freed hobject after finalization: %p", (void *) curr));
		}
#else
		DUK_HEAPHDR_CLEAR_FINALIZABLE(curr);
		DUK_HEAP_INSERT_INTO_HEAP_ALLOCATED(heap, curr);
#endif

#if defined(DUK_USE_DEBUG)
		count++;
#endif
	}

	/* finalize_list will always be processed completely. */
	DUK_ASSERT(heap->finalize_list == NULL);

#if 0
	/* While NORZ macros are used above, this is unnecessary because the
	 * only pending side effects are now finalizers, and finalize_list is
	 * empty.
	 */
	DUK_REFZERO_CHECK_SLOW(heap->heap_thread);
#endif

	DUK_ASSERT(heap->ms_prevent_count > 0);
	heap->ms_prevent_count--;

	/* Prevent count may be bumped while finalizers run, but should always
	 * be reliably unbumped by the time we get here.
	 */
	DUK_ASSERT(heap->pf_prevent_count == 1);
	heap->pf_prevent_count = 0;

#if defined(DUK_USE_DEBUG)
	DUK_DD(DUK_DDPRINT("duk_heap_process_finalize_list: %ld finalizers called", (long) count));
#endif
}

/*
 *  Run an duk_hobject finalizer.  Must never throw an uncaught error
 *  (but may throw caught errors).
 *
 *  There is no return value.  Any return value or error thrown by
 *  the finalizer is ignored (although errors are debug logged).
 *
 *  Notes:
 *
 *    - The finalizer thread 'top' assertions are there because it is
 *      critical that strict stack policy is observed (i.e. no cruft
 *      left on the finalizer stack).
 */

DUK_LOCAL duk_ret_t duk__finalize_helper(duk_context *ctx, void *udata) {
	duk_hthread *thr;

	DUK_ASSERT(ctx != NULL);
	thr = (duk_hthread *) ctx;
	DUK_UNREF(udata);

	DUK_DDD(DUK_DDDPRINT("protected finalization helper running"));

	/* [... obj] */

	/* _Finalizer property is read without checking if the value is
	 * callable or even exists.  This is intentional, and handled
	 * by throwing an error which is caught by the safe call wrapper.
	 *
	 * XXX: Finalizer lookup should traverse the prototype chain (to allow
	 * inherited finalizers) but should not invoke accessors or proxy object
	 * behavior.  At the moment this lookup will invoke proxy behavior, so
	 * caller must ensure that this function is not called if the target is
	 * a Proxy.
	 */
	duk_get_prop_stridx_short(ctx, -1, DUK_STRIDX_INT_FINALIZER);  /* -> [... obj finalizer] */
	duk_dup_m2(ctx);
	duk_push_boolean(ctx, DUK_HEAP_HAS_FINALIZER_NORESCUE(thr->heap));
	DUK_DDD(DUK_DDDPRINT("calling finalizer"));
	duk_call(ctx, 2);  /* [ ... obj finalizer obj heapDestruct ]  -> [ ... obj retval ] */
	DUK_DDD(DUK_DDDPRINT("finalizer returned successfully"));
	return 0;

	/* Note: we rely on duk_safe_call() to fix up the stack for the caller,
	 * so we don't need to pop stuff here.  There is no return value;
	 * caller determines rescued status based on object refcount.
	 */
}

DUK_INTERNAL void duk_heap_run_finalizer(duk_heap *heap, duk_hobject *obj) {
	duk_context *ctx;
	duk_ret_t rc;
#if defined(DUK_USE_ASSERTIONS)
	duk_idx_t entry_top;
#endif

	DUK_DD(DUK_DDPRINT("running duk_hobject finalizer for object: %p", (void *) obj));

	DUK_ASSERT(heap != NULL);
	DUK_ASSERT(heap->heap_thread != NULL);
	ctx = (duk_context *) heap->heap_thread;
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT_VALSTACK_SPACE(heap->heap_thread, 1);

#if defined(DUK_USE_ASSERTIONS)
	entry_top = duk_get_top(ctx);
#endif
	/*
	 *  Get and call the finalizer.  All of this must be wrapped
	 *  in a protected call, because even getting the finalizer
	 *  may trigger an error (getter may throw one, for instance).
	 */

	/* ROM objects could inherit a finalizer, but they are never deemed
	 * unreachable by mark-and-sweep, and their refcount never falls to 0.
	 */
	DUK_ASSERT(!DUK_HEAPHDR_HAS_READONLY((duk_heaphdr *) obj));

	/* Duktape 2.1: finalize_list never contains objects with FINALIZED
	 * set, so no need to check here.
	 */
	DUK_ASSERT(!DUK_HEAPHDR_HAS_FINALIZED((duk_heaphdr *) obj));
#if 0
	if (DUK_HEAPHDR_HAS_FINALIZED((duk_heaphdr *) obj)) {
		DUK_D(DUK_DPRINT("object already finalized, avoid running finalizer twice: %!O", obj));
		return;
	}
#endif
	DUK_HEAPHDR_SET_FINALIZED((duk_heaphdr *) obj);  /* ensure never re-entered until rescue cycle complete */

	if (DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(obj)) {
		/* This may happen if duk_set_finalizer() or Duktape.fin() is
		 * called for a Proxy object.  In such cases the fast finalizer
		 * flag will be set on the Proxy, not the target, and neither
		 * will be finalized.
		 */
		DUK_D(DUK_DPRINT("object is a proxy, skip finalizer call"));
		return;
	}

	duk_push_hobject(ctx, obj);  /* this also increases refcount by one */
	rc = duk_safe_call(ctx, duk__finalize_helper, NULL /*udata*/, 0 /*nargs*/, 1 /*nrets*/);  /* -> [... obj retval/error] */
	DUK_ASSERT_TOP(ctx, entry_top + 2);  /* duk_safe_call discipline */

	if (rc != DUK_EXEC_SUCCESS) {
		/* Note: we ask for one return value from duk_safe_call to get this
		 * error debugging here.
		 */
		DUK_D(DUK_DPRINT("wrapped finalizer call failed for object %p (ignored); error: %!T",
		                 (void *) obj, (duk_tval *) duk_get_tval(ctx, -1)));
	}
	duk_pop_2(ctx);  /* -> [...] */

	DUK_ASSERT_TOP(ctx, entry_top);
}

#else  /* DUK_USE_FINALIZER_SUPPORT */

/* nothing */

#endif  /* DUK_USE_FINALIZER_SUPPORT */