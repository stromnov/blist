/* Copyright 2007 Stutzbach Enterprises, LLC (daniel@stutzbachenterprises.com)
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer. 
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution. 
 *    3. The name of the author may not be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <Python.h>

#ifndef Py_BUILD_CORE
#include "listobject.h"
#endif

typedef struct {
        PyBList *lst;
        int i;
} point_t;

typedef struct {
        int depth;
        PyBList *leaf;
        int i;
        Py_ssize_t remaining;
        point_t stack[MAX_HEIGHT];
} iter_t;

typedef struct {
        PyObject_HEAD
        iter_t iter;
} blistiterobject;

/* Empty BList reuse scheme to save calls to malloc and free */
#define MAXFREELISTS 80
static PyBList *free_lists[MAXFREELISTS];
static int num_free_lists = 0;

static PyBList *free_ulists[MAXFREELISTS];
static int num_free_ulists = 0;

static blistiterobject *free_iters[MAXFREELISTS];
static int num_free_iters = 0;

#define PyBList_Check(op) (PyObject_TypeCheck((op), &PyBList_Type) || (PyObject_TypeCheck((op), &PyUserBList_Type)))
#define PyUserBList_Check(op) (PyObject_TypeCheck((op), &PyUserBList_Type))
#define PyBList_CheckExact(op) ((op)->ob_type == &PyBList_Type || (op)->ob_type == &PyUserBList_Type)
#define PyBListIter_Check(op) (PyObject_TypeCheck((op), &PyBListIter_Type) || (PyObject_TypeCheck((op), &PyBListReverseIter_Type)))

static void
copy(PyBList *self, int k, PyBList *other, int k2, int n)
{
        PyObject **src = &other->children[k2];
        PyObject **dst = &self->children[k];
        PyObject **stop = &other->children[k2+n];

        assert(self != other);

        while (src < stop)
                *dst++ = *src++;
}

static void
copyref(PyBList *self, int k, PyBList *other, int k2, int n) {
        PyObject **src = &other->children[k2];
        PyObject **dst = &self->children[k];
        PyObject **stop = &src[n];

        while (src < stop) {
                Py_INCREF(*src);
                *dst++ = *src++;
        }
}

static void
xcopyref(PyBList *self, int k, PyBList *other, int k2, int n) {
        PyObject **src = &other->children[k2];
        PyObject **dst = &self->children[k];
        PyObject **stop = &src[n];

        while (src < stop) {
                Py_XINCREF(*src);
                *dst++ = *src++;
        }
}

static void
shift_right(PyBList *self, int k, int n)
{
        PyObject **src = &self->children[self->num_children-1];
        PyObject **dst = &self->children[self->num_children-1 + n];
        PyObject **stop = &self->children[k];

        if (self->num_children == 0)
                return;

        assert(k >= 0);
        assert(k <= LIMIT);
        assert(n + self->num_children <= LIMIT);

        while (src >= stop)
                *dst-- = *src--;
}

static void
shift_left(PyBList *self, int k, int n)
{
        PyObject **src = &self->children[k];
        PyObject **dst = &self->children[k - n];
        PyObject **stop = &self->children[self->num_children];

        assert(k - n >= 0);
        assert(k >= 0);
        assert(k <= LIMIT);
        assert(self->num_children -n >= 0);

        while (src < stop)
                *dst++ = *src++;

#ifdef Py_DEBUG
        while (dst < stop)
                *dst++ = NULL;
#endif
}

#define ITER2(lst, item, start, stop, block) {\
        iter_t _it; int _use_iter;\
        if (lst->leaf) { \
                int _i; _use_iter = 0; \
                for (_i = (start); _i < lst->num_children && _i < (stop); _i++) { \
                        item = lst->children[_i]; \
                        block; \
                } \
        } else { \
                PyBList *_p; _use_iter = 1;\
                iter_init2(&_it, (lst), (start), (stop)); \
                _p = _it.leaf; \
                while (1) { \
                        if (_it.i < _p->num_children) { \
                                if (_it.remaining == 0) break; \
                                _it.remaining--; \
                                item = _p->children[_it.i++]; \
                        } else { \
                                item = iter_next(&_it); \
                                _p = _it.leaf; \
                                if (item == NULL) break; \
                        } \
                        block; \
                } \
                iter_cleanup(&_it); \
        } \
}

#define ITER(lst, item, block) {\
        iter_t _it; int _use_iter;\
        if ((lst)->leaf) { \
                int _i; _use_iter = 0;\
                for (_i = 0; _i < (lst)->num_children; _i++) { \
                        item = (lst)->children[_i]; \
                        block; \
                } \
        } else { \
                PyBList *_p; \
                _use_iter = 1; \
                iter_init(&_it, (lst)); \
                _p = _it.leaf; \
                while (1) { \
                        if (_it.i < _p->num_children) { \
                                item = _p->children[_it.i++]; \
                        } else { \
                                item = iter_next(&_it); \
                                _p = _it.leaf; \
                                if (item == NULL) break; \
                        } \
                        block; \
                } \
                iter_cleanup(&_it); \
        } \
}
#define ITER_CLEANUP() if (_use_iter) iter_cleanup(&_it)

/* Forward declarations */
PyTypeObject PyBList_Type;
PyTypeObject PyUserBList_Type;
PyTypeObject PyBListIter_Type;
PyTypeObject PyBListReverseIter_Type;
static void ext_init(PyBListRoot *root);
static void ext_mark(PyBList *broot, int offset, int value);
#define CLEAN (-1) /* also hard-coded in listobject.h */
#define DIRTY (-2)

static PyObject *_indexerr = NULL;
void set_index_error(void)
{
        if (_indexerr == NULL) 
                _indexerr = PyString_FromString("list index out of range");
        PyErr_SetObject(PyExc_IndexError, _indexerr);
}

/************************************************************************
 * Debugging forward declarations
 */

#ifdef Py_DEBUG

static int blist_unstable = 0;
static int blist_in_code = 0;
static int blist_danger = 0;
#define DANGER_BEGIN { int _blist_unstable = blist_unstable, _blist_in_code = blist_in_code; blist_unstable = 0; blist_in_code = 0; blist_danger++;
#define DANGER_END assert(!blist_unstable); assert(!blist_in_code); blist_unstable = _blist_unstable; blist_in_code = _blist_in_code; assert(blist_danger); blist_danger--; }

#else

#define DANGER_BEGIN
#define DANGER_END

#endif

/************************************************************************
 * Utility functions for removal items from a BList
 *
 * Objects in Python can execute arbitrary code when garbage
 * collected, which means they may make calls that modify the BList
 * that we're deleting items from.  Yuck.
 *
 * To avoid this in the general case, any function that removes items
 * from a BList calls decref_later() on the object instead of
 * Py_DECREF().  The objects are accumulated in a global list of
 * objects pending for deletion.  Just before the function returns to
 * the interpreter, decref_flush() is called to actually decrement the
 * reference counters.
 *
 * decref_later() can be passed PyBList objects to delete whole
 * subtrees.
 */

static PyObject **decref_list = NULL;
static Py_ssize_t decref_max = 0;
static Py_ssize_t decref_num = 0;

#define DECREF_BASE (2*128)

void decref_init(void)
{
        decref_max = DECREF_BASE;
        decref_list = (PyObject **) PyMem_New(PyObject *, decref_max);
}

static void _decref_later(PyObject *ob)
{
        if (decref_num == decref_max) {
                PyObject **tmp = decref_list;
                decref_max *= 2;

                PyMem_Resize(decref_list, PyObject *, decref_max);
                if (decref_list == NULL) {
                        PyErr_NoMemory();
                        decref_list = tmp;
                        decref_max /= 2;
                        return;
                }
        }

        decref_list[decref_num++] = ob;
}
#define decref_later(ob) do { if ((ob)->ob_refcnt > 1) { Py_DECREF((ob)); } else { _decref_later((ob)); } } while (0)

static void xdecref_later(PyObject *ob)
{
        if (ob == NULL)
                return;

        decref_later(ob);
}

static void shift_left_decref(PyBList *self, int k, int n)
{
        register PyObject **src = &self->children[k];
        register PyObject **dst = &self->children[k - n];
        register PyObject **stop = &self->children[self->num_children];
        register PyObject **dec;
        register PyObject **dst_stop = &self->children[k];

        if (decref_num + n > decref_max) {
                while (decref_num + n > decref_max)
                        decref_max *= 2;
                /* XXX Out of memory not handled */
                PyMem_Resize(decref_list, PyObject *, decref_max);
        }

        dec = &decref_list[decref_num];

        assert(n >= 0);
        assert(k - n >= 0);
        assert(k >= 0);
        assert(k <= LIMIT);
        assert(self->num_children - n >= 0);

        while (src < stop && dst < dst_stop) {
                if (*dst != NULL) {
                        if ((*dst)->ob_refcnt > 1) {
                                Py_DECREF(*dst);
                        } else {
                                *dec++ = *dst;
                        }
                }
                *dst++ = *src++;
        }

        while (src < stop) {
                *dst++ = *src++;
        }

        while (dst < dst_stop) {
                if (*dst != NULL) {
                        if ((*dst)->ob_refcnt > 1) {
                                Py_DECREF(*dst);
                        } else {
                                *dec++ = *dst;
                        }
                }
                dst++;
        }

#ifdef Py_DEBUG
        src = &self->children[self->num_children - n];
        while (src < stop)
                *src++ = NULL;
#endif

        decref_num += dec - &decref_list[decref_num];
}

static void _decref_flush(void)
{
        while (decref_num) {
                /* Py_DECREF() can cause arbitrary other oerations on
                 * BList, potentially even resulting in additional
                 * calls to decref_later() and decref_flush()!
                 *
                 * Any changes to this function must be made VERY
                 * CAREFULLY to handle this case.
                 *
                 * Invariant: whenever we call Py_DECREF, the
                 * decref_list is in a coherent state.  It contains
                 * only items that still need to be decrefed.
                 * Furthermore, we can't cache anything about the
                 * global variables.
                 */
                
                decref_num--;
                DANGER_BEGIN
                Py_DECREF(decref_list[decref_num]);
                DANGER_END
        }

        if (decref_max > DECREF_BASE) {
                /* Return memory to the system now.
                 * We may never get another chance
                 */

                decref_max = DECREF_BASE;
                PyMem_Resize(decref_list, PyObject *, decref_max);
        }
}

/* Redefined in debug mode */
#define decref_flush() (_decref_flush())
#define SAFE_DECREF(x) Py_DECREF((x))
#define SAFE_XDECREF(x) Py_XDECREF((x))

/************************************************************************
 * Debug functions
 */

#ifdef Py_DEBUG

static void check_invariants(PyBList *self)
{
        if (self->leaf) {
                assert(self->n == self->num_children);
                int i;

                for (i = 0; i < self->num_children; i++) {
                        PyObject *child = self->children[i];
                        if (child != NULL)
                                assert(child->ob_refcnt > 0);
                }
        } else {
                int i;
                Py_ssize_t total = 0;

                assert(self->num_children > 0);

                for (i = 0; i < self->num_children; i++) {
                        assert(PyBList_Check(self->children[i]));
                        assert(!PyUserBList_Check(self->children[i]));

                        PyBList *child = (PyBList *) self->children[i];
                        assert(child != self);
                        total += child->n; 
                        assert(child->num_children <= LIMIT);
                        assert(HALF <= child->num_children);
                        /* check_invariants(child); */
                }
                assert(self->n == total);
                assert(self->num_children > 1 || self->num_children == 0);

        }

        assert (self->ob_refcnt >= 1 || self->ob_type == &PyUserBList_Type
                || (self->ob_refcnt == 0 && self->num_children == 0));
}

#define VALID_RW 1
#define VALID_PARENT 2
#define VALID_USER 4
#define VALID_OVERFLOW 8
#define VALID_COLLAPSE 16
#define VALID_DECREF 32
#define VALID_DEALLOC 64
#define VALID_NEWREF 128
#define VALID_CLEAR 256

typedef struct
{
        PyBList *self;
        int options;
} debug_t;

static void debug_setup(debug_t *debug)
{
        Py_INCREF(Py_None);
        blist_in_code++;

        assert(PyBList_Check(debug->self));
        
        if (debug->options & VALID_DEALLOC) {
                assert(debug->self->ob_refcnt == 0);
                debug->self->ob_refcnt = 1;
        }

        if (debug->options & VALID_DECREF)
                assert(blist_in_code == 1);
        
        if (debug->options & VALID_RW) {
                assert(debug->self->ob_refcnt == 1 
                       || PyUserBList_Check(debug->self));
        }
        
        if (debug->options & VALID_USER) {
                if (!debug->self->leaf)
                        assert(((PyBListRoot *)debug->self)->last_n
                               == debug->self->n);
                assert(((PyBListRoot *)debug->self)->dirty_length
                       || ((PyBListRoot *)debug->self)->dirty_root);

                if (!blist_danger)
                        assert(decref_num == 0);
                assert(debug->self->ob_refcnt >= 1); 
        } 

        if (debug->options & (VALID_USER | VALID_PARENT)) {
                check_invariants(debug->self);
        }

        if ((debug->options & VALID_USER) && (debug->options & VALID_RW)) {
                assert(!blist_unstable);
                blist_unstable = 1;
        }
}

static void debug_return(debug_t *debug)
{
        Py_DECREF(Py_None);
        assert(blist_in_code);
        blist_in_code--;

#if 0
        /* Comment this test out since users can get references via
         * the gc module */
        if (debug->options & VALID_RW) {
                assert(debug->self->ob_refcnt == 1
                       || PyList_Check(debug->self)); 
        }
#endif

        if (debug->options & (VALID_PARENT)) 
                check_invariants(debug->self); 

        if (debug->options
            & (VALID_PARENT|VALID_USER|VALID_OVERFLOW|VALID_COLLAPSE)) 
                check_invariants(debug->self); 

        if (debug->options & VALID_USER) {
                if (!blist_danger)
                        assert(decref_num == 0);
                if (!debug->self->leaf)
                        assert(((PyBListRoot *)debug->self)->last_n
                               == debug->self->n);
                assert(((PyBListRoot *)debug->self)->dirty_length
                       || ((PyBListRoot *)debug->self)->dirty_root);
        }

        if ((debug->options & VALID_USER) && (debug->options & VALID_RW)) {
                assert(blist_unstable);
                blist_unstable = 0;
        }

        if (debug->options & VALID_DEALLOC) {
                assert(debug->self->ob_refcnt == 1);
                debug->self->ob_refcnt = 0;
        }
}

static PyObject *debug_return_overflow(debug_t *debug, PyObject *ret)
{
        if (debug->options & VALID_OVERFLOW) {
                if (ret == NULL) {
                        debug_return(debug);
                        return ret;
                }

                assert(PyBList_Check((PyObject *) ret)); 
                check_invariants((PyBList *) ret);
        }

        assert(!(debug->options & VALID_COLLAPSE));

        debug_return(debug);

        return ret;
}

static Py_ssize_t debug_return_collapse(debug_t *debug, Py_ssize_t ret)
{
        if (debug->options & VALID_COLLAPSE) 
                assert (((Py_ssize_t) ret) >= 0);

        assert(!(debug->options & VALID_OVERFLOW));

        debug_return(debug);

        return ret;
}

#define invariants(self, options) debug_t _debug = { (PyBList *) (self), (options) }; \
        debug_setup(&_debug)

#define _blist(ret) (PyBList *) debug_return_overflow(&_debug, (PyObject *) (ret))
#define _ob(ret) debug_return_overflow(&_debug, (ret))
#define _int(ret) debug_return_collapse(&_debug, (ret))
#define _void() do {assert(!(_debug.options & (VALID_OVERFLOW|VALID_COLLAPSE))); debug_return(&_debug);} while (0)
#define _redir(ret) ((debug_return(&_debug), 1) ? (ret) : (ret))

#undef Py_RETURN_NONE
#define Py_RETURN_NONE return Py_INCREF(Py_None), _ob(Py_None)

#undef decref_flush
#define decref_flush() do { assert(_debug.options & VALID_DECREF); _decref_flush(); } while (0)

static void safe_decref_check(PyBList *self)
{
        int i;

        assert(PyBList_Check((PyObject *) self));
        
        if (self->ob_refcnt > 1)
                return;
        
        if (self->leaf) {
                for (i = 0; i < self->num_children; i++)
                        assert(self->children[i] == NULL
                               || self->children[i]->ob_refcnt > 1);
                return;
        }

        for (i = 0; i < self->num_children; i++)
                safe_decref_check((PyBList *)self->children[i]);
}

static void safe_decref(PyBList *self)
{
        assert(PyBList_Check((PyObject *) self));
        safe_decref_check(self);

        DANGER_BEGIN
        Py_DECREF(self);
        DANGER_END
}

#undef SAFE_DECREF
#undef SAFE_XDECREF
#define SAFE_DECREF(self) (safe_decref((PyBList *)(self)))
#define SAFE_XDECREF(self) if ((self) == NULL) ; else SAFE_DECREF((self))

#else /* !Py_DEBUG */

#define check_invariants(self)
#define invariants(self, options)
#define _blist(ret) (ret)
#define _ob(ret) (ret)
#define _int(ret) (ret)
#define _void() 
#define _redir(ret) (ret)

#endif

/************************************************************************
 * Back to BLists proper.
 */

/* Creates a new blist for internal use only */
static PyBList *blist_new(void)
{
        PyBList *self;

        if (num_free_lists) {
                self = free_lists[--num_free_lists];
                _Py_NewReference((PyObject *) self);
        } else {
                DANGER_BEGIN
                self = PyObject_GC_New(PyBList, &PyBList_Type);
                DANGER_END
                if (self == NULL)
                        return NULL;
                self->children = PyMem_New(PyObject *, LIMIT);
                if (self->children == NULL) {
                        PyObject_GC_Del(self);
                        PyErr_NoMemory();
                        return NULL;
                }                        

                self->leaf = 1; /* True */
                self->num_children = 0;
                self->n = 0;
        }

        PyObject_GC_Track(self);
        
        return self;
}

/* Creates a blist for user use */
static PyBList *blist_user_new(void)
{
        PyBList *self;

        if (num_free_ulists) {
                self = free_ulists[--num_free_ulists];
                _Py_NewReference((PyObject *) self);
        } else {
                DANGER_BEGIN
                self = (PyBList *) PyObject_GC_New(PyBListRoot, &PyUserBList_Type);
                DANGER_END
                if (self == NULL)
                        return NULL;
                self->children = PyMem_New(PyObject *, LIMIT);
                if (self->children == NULL) {
                        PyObject_GC_Del(self);
                        PyErr_NoMemory();
                        return NULL;
                }                        

                self->leaf = 1; /* True */
                self->n = 0;
                self->num_children = 0;
        }

        ext_init((PyBListRoot *) self);

        PyObject_GC_Track(self);
        
        return self;
}

/* Remove links to some of our children, decrementing their refcounts */
static void blist_forget_children2(PyBList *self, int i, int j)
{
        int delta = j - i;

        invariants(self, VALID_RW);

        shift_left_decref(self, j, delta);
        self->num_children -= delta;

        _void();
}
#define blist_forget_children(self) \
        (blist_forget_children2((self), 0, (self)->num_children))
#define blist_forget_child(self, i) \
        (blist_forget_children2((self), (i), (i)+1))

/* Version for internal use defers Py_DECREF calls */
static int blist_CLEAR(PyBList *self)
{
        invariants(self, VALID_RW|VALID_PARENT);
        
        blist_forget_children(self);
        self->n = 0;
        self->leaf = 1;

        return _int(0);
}

static void
blist_become(PyBList *self, PyBList *other)
{
        invariants(self, VALID_RW);
        assert(self != other);

        Py_INCREF(other); /* "other" may be one of self's children */
        blist_forget_children(self);
        self->n = other->n;
        xcopyref(self, 0, other, 0, other->num_children);
        self->num_children = other->num_children;
        self->leaf = other->leaf;

        SAFE_DECREF(other);
        _void();
}

static void
blist_become_and_consume(PyBList *self, PyBList *other)
{
        PyObject **tmp;
        
        invariants(self, VALID_RW);
        assert(self != other);
        assert(other->ob_refcnt == 1 || PyUserBList_Check(other));

        Py_INCREF(other);
        blist_forget_children(self);
        tmp = self->children;
        self->children = other->children;
        self->n = other->n;
        self->num_children = other->num_children;
        self->leaf = other->leaf;

        other->children = tmp;
        other->n = 0;
        other->num_children = 0;
        other->leaf = 1;

        SAFE_DECREF(other);
        _void();
}

static PyBList *blist_copy(PyBList *self)
{
        PyBList *copy;

        copy = blist_new();
        blist_become(copy, self);
        return copy;
}

static PyBList *blist_user_copy(PyBList *self)
{
        PyBList *copy;

        copy = blist_user_new();
        blist_become(copy, self);
        ext_mark(copy, 0, DIRTY);
        return copy;
}

/************************************************************************
 * Useful internal utility functions
 */

/* We are searching for the child that contains leaf element i.
 *
 * Returns a 3-tuple: (the child object, our index of the child,
 *                     the number of leaf elements before the child)
 */
static void blist_locate(PyBList *self, Py_ssize_t i,
                         PyObject **child, int *idx, Py_ssize_t *before)
{
        invariants(self, VALID_PARENT);
        assert (!self->leaf);

        if (i <= self->n/2) {
                /* Search from the left */
                Py_ssize_t so_far = 0;
                int k;
                for (k = 0; k < self->num_children; k++) {
                        PyBList *p = (PyBList *) self->children[k];
                        if (i < so_far + p->n) {
                                *child = (PyObject *) p;
                                *idx = k;
                                *before = so_far;
                                _void();
                                return;
                        }
                        so_far += p->n;
                }
        } else {
                /* Search from the right */
                Py_ssize_t so_far = self->n;
                int k;
                for (k = self->num_children-1; k >= 0; k--) {
                        PyBList *p = (PyBList *) self->children[k];
                        so_far -= p->n;
                        if (i >= so_far) {
                                *child = (PyObject *) p;
                                *idx = k;
                                *before = so_far;
                                _void();
                                return;
                        }
                }
        }

        /* Just append */
        *child = self->children[self->num_children-1];
        *idx = self->num_children-1;
        *before = self->n - ((PyBList *)(*child))->n;

        _void();
}

/* Find the current height of the tree.
 *
 *      We could keep an extra few bytes in each node rather than
 *      figuring this out dynamically, which would reduce the
 *      asymptotic complexitiy of a few operations.  However, I
 *      suspect it's not worth the extra overhead of updating it all
 *      over the place.
 */
static int blist_get_height(PyBList *self)
{
        invariants(self, VALID_PARENT);
        if (self->leaf)
                return _int(1);
        return _int(blist_get_height((PyBList *)
                                     self->children[self->num_children - 1])
                    + 1);
}

static PyBList *blist_prepare_write(PyBList *self, int pt)
{
        /* We are about to modify the child at index pt.  Prepare it.
         *
         * This function returns the child object.  If the caller has
         * other references to the child, they must be discarded as they
         * may no longer be valid.
         * 
         * If the child's .refcount is 1, we simply return the
         * child object.
         * 
         * If the child's .refcount is greater than 1, we:
         * 
         * - copy the child object
         * - decrement the child's .refcount
         * - replace self.children[pt] with the copy
         * - return the copy
         */

        invariants(self, VALID_RW);
        
        if (pt < 0)
                pt += self->num_children;
        if (!self->leaf && self->children[pt]->ob_refcnt > 1) {
                PyBList *new_copy = blist_new();
                blist_become(new_copy, (PyBList *) self->children[pt]);
                SAFE_DECREF(self->children[pt]);
                self->children[pt] = (PyObject *) new_copy;
        }

        return (PyBList *) _ob(self->children[pt]);
}

static void
blist_adjust_n(PyBList *self)
{
        int i;
        
        invariants(self, VALID_RW);
        
        if (self->leaf) {
                self->n = self->num_children;
                _void();
                return;
        }
        self->n = 0;
        for (i = 0; i < self->num_children; i++)
                self->n += ((PyBList *)self->children[i])->n;

        _void();
}

/* Non-default constructor.  Create a node with specific children.
 *
 * We steal the reference counters from the caller.
 */

static PyBList *blist_new_sibling(PyBList *sibling)
{
        PyBList *self = blist_new();
        assert(sibling->num_children == LIMIT);
        copy(self, 0, sibling, HALF, HALF);
        self->leaf = sibling->leaf;
        self->num_children = HALF;
        sibling->num_children = HALF;
        blist_adjust_n(self);
        return self;
}

/************************************************************************
 * Functions for the index extension used by the root node
 */

#if 1

static unsigned highest_set_bit(unsigned x)
{
        /* XXX Speed up with a lookup table */
        unsigned rv = 0;
        unsigned mask;
        for (mask = 0x1; mask; mask <<= 1)
                if (mask & x) rv = mask;
        return rv;
}

static void ext_init(PyBListRoot *root)
{
        root->index_list = NULL;
        root->offset_list = NULL;
        root->setclean_list = NULL;
        root->index_length = 0;
        root->dirty = NULL; /* Everything is dirty */
        root->dirty_length = 0;
        root->dirty_root = DIRTY;
        root->free_root = -1;

        root->last_n = root->n;
}

static void ext_dealloc(PyBListRoot *root)
{
        if (root->index_list) PyMem_Free(root->index_list);
        if (root->offset_list) PyMem_Free(root->offset_list);
        if (root->setclean_list) PyMem_Free(root->setclean_list);
        if (root->dirty) PyMem_Free(root->dirty);
        ext_init(root);
}

/* amortized O(1) */
static int ext_alloc(PyBListRoot *root)
{
        if (root->free_root < 0) {
                int newl;
                int i;
                
                if (!root->dirty) {
                        newl = 32;
                        root->dirty = PyMem_New(int, newl);
                        root->dirty_root = DIRTY;
                        if (!root->dirty) return -1;
                } else {
                        assert(root->dirty_length > 0);
                        newl = root->dirty_length*2;
                        void *tmp = root->dirty;
                        PyMem_Resize(tmp, int, newl);
                        if (!tmp) {
                                PyMem_Free(root->dirty);
                                root->dirty = NULL;
                                root->dirty_root = DIRTY;
                                return -1;
                        }
                        root->dirty = tmp;
                }
                
                for (i = root->dirty_length; i < newl; i += 2) {
                        root->dirty[i] = i+2;
                        root->dirty[i+1] = -1;
                }
                root->dirty[newl-2] = -1;
                root->free_root = root->dirty_length;
                root->dirty_length = newl;
                assert(root->free_root >= 0);
                assert(root->free_root+1 < root->dirty_length);
        }

        /* Depth-first search for a node with fewer than 2 children.
         * Guaranteed to terminate in O(log n) since any leaf node
         * will suffice.
         */

        int i = root->free_root;
        int parent = -1;
        assert(i >= 0);
        assert(i+1 < root->dirty_length);
        while (root->dirty[i] >= 0 && root->dirty[i+1] >= 0) {
                assert(0);
                assert(i >= 0);
                assert(i+1 < root->dirty_length);
                parent = i;
                i = root->dirty[i];
        }

        /* At this point, "i" is the node to be alloced.  "parent" is
         * the node containing a pointer to "i" or -1 if free_root
         * points to "i"
         *
         * parent's pointer to i is always the left-hand pointer
         *
         * i has at most one child
         */

        if (parent < 0) {
                if (root->dirty[i] >= 0)
                        root->free_root = root->dirty[i];
                else
                        root->free_root = root->dirty[i+1];
        } else {
                if (root->dirty[i] >= 0)
                        root->dirty[parent] = root->dirty[i];
                else
                        root->dirty[parent] = root->dirty[i+1];
        }

        assert(i >= 0);
        assert(i+1 < root->dirty_length);
        return i;
}

/* O(n) */
/* Add each node in the tree rooted at loc to the free tree */
/* Amortized O(1), since each node to be freed corresponds with
 * an earlier lookup. */
static void ext_free(PyBListRoot *root, int loc)
{
        assert(loc >= 0);
        assert(loc+1 < root->dirty_length);
        if (root->dirty[loc] >= 0)
                ext_free(root, root->dirty[loc]);
        if (root->dirty[loc+1] >= 0)
                ext_free(root, root->dirty[loc+1]);

        root->dirty[loc] = root->free_root;
        root->dirty[loc+1] = -1;
        root->free_root = loc;
        assert(root->free_root >= 0);
        assert(root->free_root+1 < root->dirty_length);
}

static void
ext_mark_r(PyBListRoot *root, int offset, int i, int bit, int value)
{
        int next;
        
        if (!(offset & bit)) {
                /* Take left fork */

                if (value == DIRTY) {
                        /* Mark right fork dirty */
                        assert(i >= 0 && i+1 < root->dirty_length);
                        if (root->dirty[i+1] >= 0)
                                ext_free(root, root->dirty[i+1]);
                        root->dirty[i+1] = DIRTY;
                }
                next = i;
        } else {
                /* Take right fork */
                next = i+1;
        }

        assert(next >= 0 && next < root->dirty_length);

        int j = root->dirty[next];
        
        if (j == value)
                return;

        if (bit == 1) {
                root->dirty[next] = value;
                return;
        }

        if (j < 0) {
                int nvalue = j;
                root->dirty[next] = ext_alloc(root);
                if (root->dirty[next] < 0) {
                        ext_dealloc(root);
                        return;
                }
                j = root->dirty[next];
                assert(j >= 0);
                assert(j+1 < root->dirty_length);
                root->dirty[j] = nvalue;
                root->dirty[j+1] = nvalue;
        }
                
        ext_mark_r(root, offset, j, bit >> 1, value);

        if (root->dirty
            && (root->dirty[j] == root->dirty[j+1]
                || (root->dirty[j] < 0
                    && (((offset | (bit>>1)) & ~((bit>>1)-1))
                        > (root->n-1) /INDEX_FACTOR)))) {
                /* Both the same?  Consolidate */
                ext_free(root, j);
                root->dirty[next] = value;
        }
}

static void ext_mark(PyBList *broot, int offset, int value)
{
        PyBListRoot *root = (PyBListRoot*) broot;
        if (!root->n) {
                root->last_n = root->n;
                return;
        }
        if ((!offset && value == DIRTY) || root->n <= INDEX_FACTOR) {
                if (root->dirty_root >= 0)
                        ext_free(root, root->dirty_root);
                root->dirty_root = DIRTY;
                root->last_n = root->n;
                return;
        }

        assert(root->last_n == root->n);

        if (root->dirty_root == value) return;

        if (root->dirty_root < 0) {
                int nvalue = root->dirty_root;
                root->dirty_root = ext_alloc(root);
                if (root->dirty_root < 0) {
                        ext_dealloc(root);
                        return;
                }
                assert(root->dirty_root >= 0);
                assert(root->dirty_root+1 < root->dirty_length);
                root->dirty[root->dirty_root] = nvalue;
                root->dirty[root->dirty_root+1] = nvalue;
        }
        offset /= INDEX_FACTOR;

        int bit = highest_set_bit((root->n-1) / INDEX_FACTOR);
        ext_mark_r(root, offset, root->dirty_root, bit, value);
        if (root->dirty &&
            (root->dirty[root->dirty_root] ==root->dirty[root->dirty_root+1])){
                ext_free(root, root->dirty_root);
                root->dirty_root = value;
        }
}

#if 0
/* These functions are unused, but useful for debugging.  Do not remove. */

static void ext_print_r(PyBListRoot *root, int i)
{
        printf("(");
        if (root->dirty[i] < 0)
                printf("%d", root->dirty[i]);
        else
                ext_print_r(root, root->dirty[i]);
        printf(",");
        if (root->dirty[i+1] < 0)
                printf("%d", root->dirty[i+1]);
        else
                ext_print_r(root, root->dirty[i+1]);
        printf(")");
}

static void ext_print(PyBListRoot *root)
{
        if (root->dirty_root < 0)
                printf("%d", root->dirty_root);
        else
                ext_print_r(root, root->dirty_root);
        printf("\n");
}
#endif

static int ext_find_dirty(PyBListRoot *root, int offset, int bit, int i)
{
        assert(root->dirty);
        assert(i >= 0);
        assert(bit);
        
        if (root->dirty[i] == DIRTY)
                return offset;
        if (root->dirty[i] >= 0)
                return ext_find_dirty(root, offset, bit >> 1, root->dirty[i]);

        if (root->dirty[i+1] == DIRTY)
                return offset | bit;
        assert(root->dirty[i+1] >= 0);
        return ext_find_dirty(root, offset | bit, bit >> 1, root->dirty[i+1]);
}

/* O(log n) */
static int ext_is_dirty(PyBListRoot *root, int offset, int *dirty_offset)
{
        if (root->dirty == NULL) {
                *dirty_offset = -1;
                return 1; /* Everything is dirty */
        }
        if (root->dirty_root < 0) {
                *dirty_offset = -1;
                return root->dirty_root == DIRTY;
        }
        int i = root->dirty_root;
        int parent = -1;
        offset /= INDEX_FACTOR;
        int bit = highest_set_bit((root->n-1) / INDEX_FACTOR);

        assert(root->last_n == root->n);

        do {
                assert(bit);
                parent = i;
                if (!(offset & bit)) {
                        assert (i >= 0 && i < root->dirty_length);
                        i = root->dirty[i];
                } else {
                        assert (i >= 0 && i+1 < root->dirty_length);
                        i = root->dirty[i+1];
                }
                bit >>= 1;
        } while (i >= 0);

        if (i != DIRTY) {
                if (!bit) bit = 1; else bit <<= 1;
                *dirty_offset = INDEX_FACTOR *
                        ext_find_dirty(root, (offset ^ bit) & ~(bit-1), bit,
                                       parent);
                assert(*dirty_offset >= 0);
                assert(*dirty_offset < root->n);
        }
        
        return i == DIRTY;
}

static int ext_grow_index(PyBListRoot *root)
{
        int oldl = root->index_length;
        if (!root->index_length) {
                if (root->index_list) PyMem_Free(root->index_list);
                if (root->offset_list) PyMem_Free(root->offset_list);
                if (root->setclean_list) PyMem_Free(root->setclean_list);

                root->index_list = NULL;
                root->offset_list = NULL;
                root->setclean_list = NULL;
                
                root->index_length = root->n / INDEX_FACTOR + 1;
                root->index_list = PyMem_New(PyBList *, root->index_length);
                if (!root->index_list) {
                fail:
                        root->index_length = oldl;
                        return -1;
                }
                root->offset_list = PyMem_New(int, root->index_length);
                if (!root->offset_list) goto fail;
                root->setclean_list
                        = PyMem_New(unsigned,SETCLEAN_LEN(root->index_length));
                if (!root->setclean_list) goto fail;
        } else {
                do {
                        root->index_length *= 2;
                } while (root->n / INDEX_FACTOR + 1 > root->index_length);
                void *tmp = root->index_list;
                PyMem_Resize(tmp, PyBList *, root->index_length);
                if (!tmp) goto fail;
                root->index_list = tmp;

                tmp = root->offset_list;
                PyMem_Resize(tmp, int, root->index_length);
                if (!tmp) goto fail;
                root->offset_list = tmp;

                tmp = root->setclean_list;
                PyMem_Resize(tmp, unsigned, SETCLEAN_LEN(root->index_length));
                if (!tmp) goto fail;
                root->setclean_list = tmp;
        }
        return 0;
}

static void
ext_mark_clean(PyBListRoot *root, int offset, PyBList *p, int setclean)
{
        int ioffset = offset / INDEX_FACTOR;
        while (ioffset * INDEX_FACTOR < offset)
                ioffset++;
        for (;ioffset * INDEX_FACTOR < offset + p->n; ioffset++) {
                ext_mark((PyBList*)root, ioffset * INDEX_FACTOR,CLEAN);

                if (ioffset >= root->index_length) {
                        int err = ext_grow_index(root);
                        if (err < -1) {
                                ext_dealloc(root);
                                return;
                        }
                }

                assert(ioffset >= 0);
                assert(ioffset < root->index_length);
                root->index_list[ioffset] = p;
                root->offset_list[ioffset] = offset;

                if (setclean)
                        SET_BIT(root->setclean_list, ioffset);
                else
                        CLEAR_BIT(root->setclean_list, ioffset);
        }
}

static PyObject *ext_make_clean(PyBListRoot *root, Py_ssize_t i)
{
        PyObject *rv;
        Py_ssize_t so_far;
        Py_ssize_t offset = 0;
        PyBList *p = (PyBList *)root;
        int j = i, k;
        int setclean = 1;
        do {
                blist_locate(p, j, (PyObject **) &p, &k, &so_far);
                if (p->ob_refcnt > 1)
                        setclean = 0;                        
                offset += so_far;
                j -= so_far;
        } while (!p->leaf);

        rv = p->children[j];
        ext_mark_clean(root, offset, p, setclean);
        return rv;
}

#endif

/************************************************************************
 * Functions for manipulating the tree
 */

/* Child k has underflowed.  Borrow from k+1 */
static void blist_borrow_right(PyBList *self, int k)
{
        PyBList *p = (PyBList *) self->children[k];
        PyBList *right;
        unsigned total;
        unsigned split;
        unsigned migrate;

        invariants(self, VALID_RW);
        
        right = blist_prepare_write(self, k+1);
        total = p->num_children + right->num_children;
        split = total / 2;
        migrate = split - p->num_children;

        assert(split >= HALF);
        assert(total-split >= HALF);

        copy(p, p->num_children, right, 0, migrate);
        p->num_children += migrate;
        shift_left(right, migrate, migrate);
        right->num_children -= migrate;
        blist_adjust_n(right);
        blist_adjust_n(p);

        _void();
}

/* Child k has underflowed.  Borrow from k-1 */
static void blist_borrow_left(PyBList *self, int k)
{
        PyBList *p = (PyBList *) self->children[k];
        PyBList *left;
        unsigned total;
        unsigned split;
        unsigned migrate;

        invariants(self, VALID_RW);
        
        left = blist_prepare_write(self, k-1);
        total = p->num_children + left->num_children;
        split = total / 2;
        migrate = split - p->num_children;

        assert(split >= HALF);
        assert(total-split >= HALF);

        shift_right(p, 0, migrate);
        copy(p, 0, left, left->num_children - migrate, migrate);
        p->num_children += migrate;
        left->num_children -= migrate;
        blist_adjust_n(left);
        blist_adjust_n(p);

        _void();
}

/* Child k has underflowed.  Merge with k+1 */
static void blist_merge_right(PyBList *self, int k)
{
        int i;
        PyBList *p = (PyBList *) self->children[k];
        PyBList *p2 = (PyBList *) self->children[k+1];

        invariants(self, VALID_RW);
        
        copy(p, p->num_children, p2, 0, p2->num_children);
        for (i = 0; i < p2->num_children; i++)
                Py_INCREF(p2->children[i]);
        p->num_children += p2->num_children;
        blist_forget_child(self, k+1);
        blist_adjust_n(p);

        _void();
}

/* Child k has underflowed.  Merge with k-1 */
static void blist_merge_left(PyBList *self, int k)
{
        int i;
        PyBList *p = (PyBList *) self->children[k];
        PyBList *p2 = (PyBList *) self->children[k-1];

        invariants(self, VALID_RW);
        
        shift_right(p, 0, p2->num_children);
        p->num_children += p2->num_children;
        copy(p, 0, p2, 0, p2->num_children);
        for (i = 0; i < p2->num_children; i++)
                Py_INCREF(p2->children[i]);
        blist_forget_child(self, k-1);
        blist_adjust_n(p);

        _void();
}

/* Collapse the tree, if possible */
static int
blist_collapse(PyBList *self)
{
        PyBList *p;
        invariants(self, VALID_RW|VALID_COLLAPSE);
        
        if (self->num_children != 1 || self->leaf) {
                blist_adjust_n(self);
                return _int(0);
        }

        p = blist_prepare_write(self, 0);
        blist_become_and_consume(self, p);
        check_invariants(self);
        return _int(1);
}

/* Check if children k-1, k, or k+1 have underflowed.
 *
 * If so, move things around until self is the root of a valid
 * subtree again, possibly requiring collapsing the tree.
 * 
 * Always calls self._adjust_n() (often via self.__collapse()).
 */
static int blist_underflow(PyBList *self, int k)
{
        invariants(self, VALID_RW|VALID_COLLAPSE);
        
        if (self->leaf) {
                blist_adjust_n(self);
                return _int(0);
        }

        if (k < self->num_children) {
                PyBList *p = blist_prepare_write(self, k);
                int shrt = HALF - p->num_children;

                while (shrt > 0) {
                        if (k+1 < self->num_children
                            && ((PyBList *)self->children[k+1])->num_children >= HALF + shrt)
                                blist_borrow_right(self, k);
                        else if (k > 0
                                 && (((PyBList *)self->children[k-1])->num_children
                                     >= HALF + shrt))
                                blist_borrow_left(self, k);
                        else if (k+1 < self->num_children)
                                blist_merge_right(self, k);
                        else if (k > 0)
                                blist_merge_left(self, k--);
                        else /* No siblings for p */
                                return _int(blist_collapse(self));

                        p = blist_prepare_write(self, k);
                        shrt = HALF - p->num_children;
                }
        }

        if (k > 0 && ((PyBList *)self->children[k-1])->num_children < HALF) {
                int collapse = blist_underflow(self, k-1);
                if (collapse) return _int(collapse);
        }
        
        if (k+1 < self->num_children
            && ((PyBList *)self->children[k+1])->num_children < HALF) {
                int collapse = blist_underflow(self, k+1);
                if (collapse) return _int(collapse);
        }

        return _int(blist_collapse(self));
}

/* Insert 'item', which may be a subtree, at index k. */
static PyBList *blist_insert_here(PyBList *self, int k, PyObject *item)
{
        /* Since the subtree may have fewer than half elements, we may
         * need to merge it after insertion.
         * 
         * This function may cause self to overflow.  If it does, it will
         * take the upper half of its children and put them in a new
         * subtree and return the subtree.  The caller is responsible for
         * inserting this new subtree just to the right of self.
         * 
         * Otherwise, it returns None.
         */

        PyBList *sibling;
        
        invariants(self, VALID_RW|VALID_OVERFLOW);
        assert(k >= 0);

        if (self->num_children < LIMIT) {
                int collapse;

                shift_right(self, k, 1);
                self->num_children++;
                self->children[k] = item;
                collapse = blist_underflow(self, k);
                assert(!collapse); (void) collapse;
                return _blist(NULL);
        }

        sibling = blist_new_sibling(self);

        if (k < HALF) {
                int collapse;

                shift_right(self, k, 1);
                self->num_children++;
                self->children[k] = item;
                collapse = blist_underflow(self, k);
                assert(!collapse); (void) collapse;
        } else {
                int collapse;

                shift_right(sibling, k - HALF, 1);
                sibling->num_children++;
                sibling->children[k - HALF] = item;
                collapse = blist_underflow(sibling, k - HALF);
                assert(!collapse); (void) collapse;
                blist_adjust_n(sibling);
        }

        blist_adjust_n(self);
        check_invariants(self);
        return _blist(sibling);
}

/* Recurse depth layers, then insert subtree on the left or right */
static PyBList *
blist_insert_subtree(PyBList *self, int side, PyBList *subtree, int depth)
{
        /* This function may cause an overflow.
         *    
         * depth == 0 means insert the subtree as a child of self.
         * depth == 1 means insert the subtree as a grandchild, etc.
         */

        PyBList *sibling;
        invariants(self, VALID_RW|VALID_OVERFLOW);
        assert(side == 0 || side == -1);

        self->n += subtree->n;

        if (depth) {
                PyBList *p = blist_prepare_write(self, side);
                PyBList *overflow = blist_insert_subtree(p, side,
                                                         subtree, depth-1);
                if (!overflow) return _blist(NULL);
                if (side == 0)
                        side = 1;
                subtree = overflow;
        }

        if (side < 0)
                side = self->num_children;

        sibling = blist_insert_here(self, side, (PyObject *) subtree);

        return _blist(sibling);
}

/* Handle the case where a user-visible node overflowed */
static int
blist_overflow_root(PyBList *self, PyBList *overflow)
{
        PyBList *child;
        
        invariants(self, VALID_RW);
        
        if (!overflow) return _int(0);
        child = blist_new();
        blist_become_and_consume(child, self);
        self->children[0] = (PyObject *)child;
        self->children[1] = (PyObject *)overflow;
        self->num_children = 2;
        self->leaf = 0;
        blist_adjust_n(self);
        return _int(-1);
}

/* Concatenate two trees of potentially different heights. */
static PyBList *
blist_concat_blist(PyBList *left_subtree, PyBList *right_subtree,
                   int height_diff, int *padj)
{
        /* The parameters are the two trees, and the difference in their
         * heights expressed as left_height - right_height.
         * 
         * Returns a tuple of the new, combined tree, and an integer.
         * The integer expresses the height difference between the new
         * tree and the taller of the left and right subtrees.  It will
         * be 0 if there was no change, and 1 if the new tree is taller
         * by 1.
         */

        int adj = 0;
        PyBList *overflow;
        PyBList *root;
        
        assert(left_subtree->ob_refcnt == 1);
        assert(right_subtree->ob_refcnt == 1);

        if (height_diff == 0) {
                int collapse;
                
                root = blist_new();
                root->children[0] = (PyObject *) left_subtree;
                root->children[1] = (PyObject *) right_subtree;
                root->leaf = 0;
                root->num_children = 2;
                collapse = blist_underflow(root, 0);
                if (!collapse)
                        collapse = blist_underflow(root, 1);
                if (!collapse)
                        adj = 1;
                overflow = NULL;
        } else if (height_diff > 0) { /* Left is larger */
                root = left_subtree;
                overflow = blist_insert_subtree(root, -1, right_subtree,
                                                height_diff - 1);
        } else { /* Right is larger */
                root = right_subtree;
                overflow = blist_insert_subtree(root, 0, left_subtree,
                                                -height_diff - 1);
        }

        adj += -blist_overflow_root(root, overflow);
        if (padj) *padj = adj;

        return root;
}

/* Concatenate two subtrees of potentially different heights. */
static PyBList *blist_concat_subtrees(PyBList *left_subtree,
                                      int left_depth,
                                      PyBList *right_subtree,
                                      int right_depth,
                                      int *pdepth)
{
        /* Returns a tuple of the new, combined subtree and its depth.
         *
         * Depths are the depth in the parent, not their height.
         */

        int deepest = left_depth > right_depth ?
                left_depth : right_depth;
        PyBList *root = blist_concat_blist(left_subtree, right_subtree,
                                     -(left_depth - right_depth), pdepth);
        if (pdepth) *pdepth = deepest - *pdepth;
        return root;
}
        
/* Concatenate two roots of potentially different heights. */
static PyBList *blist_concat_roots(PyBList *left_root, int left_height,
                                   PyBList *right_root, int right_height,
                                   int *pheight)
{
        /* Returns a tuple of the new, combined root and its height.
         *
         * Heights are the height from the root to its leaf nodes.
         */

        PyBList *root = blist_concat_blist(left_root, right_root,
                                     left_height - right_height, pheight);
        int highest = left_height > right_height ?
                left_height : right_height;

        if (pheight) *pheight = highest + *pheight;
        
        return root;
}

static PyBList *
blist_concat_unknown_roots(PyBList *left_root, PyBList *right_root)
{
        return blist_concat_roots(left_root, blist_get_height(left_root),
                                  right_root, blist_get_height(right_root),
                                  NULL);
}

/* Child at position k is too short by "depth".  Fix it */
static int blist_reinsert_subtree(PyBList *self, int k, int depth)
{
        PyBList *subtree;
        
        invariants(self, VALID_RW);
        
        assert(self->children[k]->ob_refcnt == 1);
        subtree = (PyBList *) self->children[k];
        shift_left(self, k+1, 1);
        self->num_children--;
        
        if (self->num_children > k) {
                /* Merge right */
                PyBList *p = blist_prepare_write(self, k);
                PyBList *overflow = blist_insert_subtree(p, 0,
                                                         subtree, depth-1);
                if (overflow) {
                        shift_right(self, k+1, 1);
                        self->num_children++;
                        self->children[k+1] = (PyObject *) overflow;
                }
        } else {
                /* Merge left */
                PyBList *p = blist_prepare_write(self, k-1);
                PyBList *overflow = blist_insert_subtree(p, -1,
                                                         subtree, depth-1);
                if (overflow) {
                        shift_right(self, k, 1);
                        self->num_children++;
                        self->children[k] = (PyObject *) overflow;
                }
        }
        
        return _int(blist_underflow(self, k));
}

/************************************************************************
 * The main insert and deletion operations
 */

/* Recursive to find position i, and insert item just there. */
static PyBList *ins1(PyBList *self, Py_ssize_t i, PyObject *item)
{
        PyBList *ret;
        PyBList *p;
        int k;
        Py_ssize_t so_far;
        PyBList *overflow;

        invariants(self, VALID_RW|VALID_OVERFLOW);
        
        if (self->leaf) {
                Py_INCREF(item);

                /* Speed up the common case */
                if (self->num_children < LIMIT) {
                        shift_right(self, i, 1);
                        self->num_children++;
                        self->n++;
                        self->children[i] = item;
                        return _blist(NULL);
                }

                return _blist(blist_insert_here(self, i, item));
        }

        blist_locate(self, i, (PyObject **) &p, &k, &so_far);

        self->n += 1;
        p = blist_prepare_write(self, k);
        overflow = ins1(p, i - so_far, item);

        if (!overflow) ret = NULL;
        else ret = blist_insert_here(self, k+1, (PyObject *) overflow);

        return _blist(ret);
}

static int blist_extend_blist(PyBList *self, PyBList *other)
{
        PyBList *right, *left, *root;
        
        invariants(self, VALID_RW);

        /* Special case for speed */
        if (self->leaf && other->leaf && self->n + other->n <= LIMIT) {
                copyref(self, self->n, other, 0, other->n);
                self->n += other->n;
                self->num_children = self->n;
                return _int(0);
        }

        /* Make not-user-visible roots for the subtrees */
        right = blist_copy(other); /* XXX not checking return values */
        left = blist_new();
        blist_become_and_consume(left, self);

        root = blist_concat_unknown_roots(left, right);
        blist_become_and_consume(self, root);
        SAFE_DECREF(root);
        return _int(0);
}

/* Recursive version of __delslice__ */
static int blist_delslice(PyBList *self, Py_ssize_t i, Py_ssize_t j)
{
        /* This may cause self to collapse.  It returns 0 if it did
         * not.  If a collapse occured, it returns a positive integer
         * indicating how much shorter this subtree is compared to when
         * _delslice() was entered.
         *
         * As a special exception, it may return 0 if the entire subtree
         * is deleted.
         * 
         * Additionally, this function may cause an underflow.
         */

        PyBList *p, *p2;
        int k, k2, depth;
        Py_ssize_t so_far, so_far2, low;
        int collapse_left, collapse_right, deleted_k, deleted_k2;

        invariants(self, VALID_RW | VALID_PARENT | VALID_COLLAPSE);
        check_invariants(self);

        if (j > self->n)
                j = self->n;

        if (i == j)
                return _int(0);
        
        if (self->leaf) {
                blist_forget_children2(self, i, j);
                self->n = self->num_children;
                return _int(0);
        }

        if (i == 0 && j >= self->n) {
                /* Delete everything. */
                blist_CLEAR(self);
                return _int(0);
        }
        
        blist_locate(self, i, (PyObject **) &p, &k, &so_far);
        blist_locate(self, j-1, (PyObject **) &p2, &k2, &so_far2);

        if (k == k2) {
                /* All of the deleted elements are contained under a single
                 * child of this node.  Recurse and check for a short
                 * subtree and/or underflow
                 */

                assert(so_far == so_far2);
                p = blist_prepare_write(self, k);
                depth = blist_delslice(p, i - so_far, j - so_far);
                if (p->n == 0) {
                        SAFE_DECREF(p);
                        shift_left(self, k+1, 1);
                        self->num_children--;
                        return _int(blist_collapse(self));
                }
                if (!depth) 
                        return _int(blist_underflow(self, k));
                return _int(blist_reinsert_subtree(self, k, depth));
        }

        /* Deleted elements are in a range of child elements.  There 
         * will be:
         * - a left child (k) where we delete some (or all) of its children
         * - a right child (k2) where we delete some (or all) of it children
         * - children in between who are deleted entirely
         */

        /* Call _delslice recursively on the left and right */
        p = blist_prepare_write(self, k);
        collapse_left = blist_delslice(p, i - so_far, j - so_far);
        p2 = blist_prepare_write(self, k2);
        low = i-so_far2 > 0 ? i-so_far2 : 0;
        collapse_right = blist_delslice(p2, low, j - so_far2);

        deleted_k = 0; /* False */
        deleted_k2 = 0; /* False */

        /* Delete [k+1:k2] */
        blist_forget_children2(self, k+1, k2);
        k2 = k+1;

        /* Delete k1 and k2 if they are empty */
        if (!((PyBList *)self->children[k2])->n) {
                decref_later((PyObject *) self->children[k2]);
                shift_left(self, k2+1, 1);
                self->num_children--;
                deleted_k2 = 1; /* True */
        }
        if (!((PyBList *)self->children[k])->n) {
                decref_later(self->children[k]);
                shift_left(self, k+1, 1);
                self->num_children--;
                deleted_k = 1; /* True */
        }

        if (deleted_k && deleted_k2) /* # No messy subtrees.  Good. */
                return _int(blist_collapse(self));

        /* The left and right may have collapsed and/or be in an 
         * underflow state.  Clean them up.  Work on fixing collapsed
         * trees first, then worry about underflows.
         */

        if (!deleted_k && !deleted_k2 && collapse_left && collapse_right) {
                /* Both exist and collapsed.  Merge them into one subtree. */
                PyBList *left, *right, *subtree;
                
                left = (PyBList *) self->children[k];
                right = (PyBList *) self->children[k+1];
                shift_left(self, k+1, 1);
                self->num_children--;
                subtree = blist_concat_subtrees(left, collapse_left,
                                                right, collapse_right,
                                                &depth);
                self->children[k] = (PyObject *) subtree;
        } else if (deleted_k) {
                /* Only the right potentially collapsed, point there. */
                depth = collapse_right;
                /* k already points to the old k2, since k was deleted */
        } else if (!deleted_k2 && !collapse_left) {
                /* Only the right potentially collapsed, point there. */
                k = k + 1;
                depth = collapse_right;
        } else {
                depth = collapse_left;
        }

        /* At this point, we have a potentially short subtree at k, 
         * with depth "depth".
         */

        if (!depth || self->num_children == 1) {
                /* Doesn't need merging, or no siblings to merge with */
                return _int(depth + blist_underflow(self, k));
        }

        /* We have a short subtree at k, and we have other children */
        return _int(blist_reinsert_subtree(self, k, depth));
}

static PyObject *
blist_get1(PyBList *self, Py_ssize_t i)
{
        PyBList *p;
        int k;
        Py_ssize_t so_far;

        invariants(self, VALID_PARENT);

        if (self->leaf)
                return _ob(self->children[i]);

        blist_locate(self, i, (PyObject **) &p, &k, &so_far);
        assert(i >= so_far);
        return _ob(blist_get1(p, i - so_far));
}

static void blist_delitem(PyBList *self, Py_ssize_t i)
{
        blist_delslice(self, i, i+1);
}

static PyObject *blist_delitem_return(PyBList *self, Py_ssize_t i)
{
        PyObject *rv = blist_get1(self, i);
        Py_INCREF(rv);
        blist_delitem(self, i);
        return rv;
}

/************************************************************************
 * BList iterator
 */

static iter_t *iter_init2(iter_t *iter, PyBList *lst, Py_ssize_t start, Py_ssize_t stop)
{
        iter->depth = 0;

        assert(stop >= 0);
        assert(start >= 0);
        iter->remaining = stop - start;
        while (!lst->leaf) {
                PyBList *p;
                int k;
                Py_ssize_t so_far;

                blist_locate(lst, start, (PyObject **) &p, &k, &so_far);
                iter->stack[iter->depth].lst = lst;
                iter->stack[iter->depth++].i = k + 1;
                Py_INCREF(lst);
                lst = p;
                start -= so_far;
        }

        iter->leaf = lst;
        iter->i = start;
        iter->depth++;
        Py_INCREF(lst);

        return iter;
}
#define iter_init(iter, lst) (iter_init2((iter), (lst), 0, (lst)->n))

static PyObject *iter_next(iter_t *iter)
{
        PyBList *p;
        int i;

        p = iter->leaf;
        if (iter->remaining == 0)
                return NULL;
        
        iter->remaining--;
        if (iter->i < p->num_children) 
                return p->children[iter->i++];

        iter->depth--;
        do {
                decref_later((PyObject *) p);
                if (!iter->depth) {
                        iter->remaining = 0;
                        iter->leaf = NULL;
                        return NULL;
                }
                p = iter->stack[--iter->depth].lst;
                i = iter->stack[iter->depth].i;
        } while (i >= p->num_children);
        
        assert(iter->stack[iter->depth].lst == p);
        iter->stack[iter->depth++].i = i+1;

        while (!p->leaf) {
                p = (PyBList *) p->children[i];
                Py_INCREF(p);
                i = 0;
                iter->stack[iter->depth].lst = p;
                iter->stack[iter->depth++].i = i+1;
        }

        iter->leaf = iter->stack[iter->depth-1].lst;
        iter->i = iter->stack[iter->depth-1].i;

        return p->children[i];
}

static void iter_cleanup(iter_t *iter)
{
        int i;
        for (i = 0; i < iter->depth-1; i++)
                decref_later((PyObject *) iter->stack[i].lst);
        if (iter->depth)
                decref_later((PyObject *) iter->leaf);
}

static PyObject *
py_blist_iter(PyObject *oseq)
{
        PyBList *seq;
        blistiterobject *it;

        if (!PyBList_Check(oseq)) {
                PyErr_BadInternalCall();
                return NULL;
        }

        seq = (PyBList *) oseq;

        invariants(seq, VALID_USER | VALID_NEWREF);
        
        if (num_free_iters) {
                it = free_iters[--num_free_iters];
                _Py_NewReference((PyObject *) it);
        } else {
                DANGER_BEGIN
                it = PyObject_GC_New(blistiterobject, &PyBListIter_Type);
                DANGER_END
                if (it == NULL)
                        return _ob(NULL);
        }

        if (seq->leaf) {
                /* Speed up common case */
                it->iter.leaf = seq;
                it->iter.i = 0;
                it->iter.depth = 1;
                it->iter.remaining = seq->n;
                Py_INCREF(seq);
        } else 
                iter_init(&it->iter, seq);

        PyObject_GC_Track(it);
        return _ob((PyObject *) it);
}

static void blistiter_dealloc(PyObject *oit)
{
        blistiterobject *it;
        
        assert(PyBListIter_Check(oit));
        it = (blistiterobject *) oit;
        
        PyObject_GC_UnTrack(it);
        iter_cleanup(&it->iter);
        if (num_free_iters < MAXFREELISTS
            && (it->ob_type == &PyBListIter_Type))
                free_iters[num_free_iters++] = it;
        else
                PyObject_GC_Del(it);
        _decref_flush();
}

static int blistiter_traverse(PyObject *oit, visitproc visit, void *arg)
{
        blistiterobject *it;
        int i;

        assert(PyBListIter_Check(oit));
        it = (blistiterobject *) oit;
        
        for (i = 0; i < it->iter.depth-1; i++)
                Py_VISIT(it->iter.stack[i].lst);
        if (it->iter.depth)
                Py_VISIT(it->iter.leaf);
        return 0;
}

static PyObject *blistiter_next(PyObject *oit)
{
        blistiterobject *it = (blistiterobject *) oit;
        PyObject *obj;
        
        /* Speed up common case */
        PyBList *p;
        p = it->iter.leaf;
        if (it->iter.remaining == 0)
                return NULL;
        if (it->iter.i < p->num_children) {
                it->iter.remaining--;
                obj = p->children[it->iter.i++];
                Py_INCREF(obj);
                return obj;
        }

        obj = iter_next(&it->iter);
        if (obj != NULL)
                Py_INCREF(obj);

        _decref_flush();
        return obj;
}

static PyObject *
blistiter_len(blistiterobject *it)
{
        return PyInt_FromSsize_t(it->iter.remaining);
}

PyDoc_STRVAR(length_hint_doc, "Private method returning an estimate of len(list(it)).");

static PyMethodDef blistiter_methods[] = {
        {"__length_hint__", (PyCFunction)blistiter_len, METH_NOARGS, length_hint_doc},
        {NULL,          NULL}           /* sentinel */
};

PyTypeObject PyBListIter_Type = {
        PyObject_HEAD_INIT(NULL)
        0,                                      /* ob_size */
        "blistiterator",                        /* tp_name */
        sizeof(blistiterobject),                /* tp_basicsize */
        0,                                      /* tp_itemsize */
        /* methods */
        blistiter_dealloc,                      /* tp_dealloc */
        0,                                      /* tp_print */
        0,                                      /* tp_getattr */
        0,                                      /* tp_setattr */
        0,                                      /* tp_compare */
        0,                                      /* tp_repr */
        0,                                      /* tp_as_number */
        0,                                      /* tp_as_sequence */
        0,                                      /* tp_as_mapping */
        0,                                      /* tp_hash */
        0,                                      /* tp_call */
        0,                                      /* tp_str */
        PyObject_GenericGetAttr,                /* tp_getattro */
        0,                                      /* tp_setattro */
        0,                                      /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
        0,                                      /* tp_doc */
        blistiter_traverse,                     /* tp_traverse */
        0,                                      /* tp_clear */
        0,                                      /* tp_richcompare */
        0,                                      /* tp_weaklistoffset */
        PyObject_SelfIter,                      /* tp_iter */
        blistiter_next,                         /* tp_iternext */
        blistiter_methods,                      /* tp_methods */
        0,                                      /* tp_members */
};

/************************************************************************
 * BList reverse iterator
 */

static iter_t *
riter_init2(iter_t *iter, PyBList *lst, Py_ssize_t start, Py_ssize_t stop)
{
        iter->depth = 0;

        assert(stop >= 0);
        assert(start >= 0);
        assert(start >= stop);
        iter->remaining = start - stop;
        while (!lst->leaf) {
                PyBList *p;
                int k;
                Py_ssize_t so_far;

                blist_locate(lst, start-1, (PyObject **) &p, &k, &so_far);
                iter->stack[iter->depth].lst = lst;
                iter->stack[iter->depth++].i = k - 1;
                Py_INCREF(lst);
                lst = p;
                start -= so_far;
        }

        iter->leaf = lst;
        iter->i = start-1;
        iter->depth++;
        Py_INCREF(lst);

        return iter;
}
#define riter_init(iter, lst) (riter_init2((iter), (lst), (lst)->n, 0))

static PyObject *
iter_prev(iter_t *iter)
{
        PyBList *p;
        int i;

        p = iter->leaf;
        if (iter->remaining == 0)
                return NULL;
        
        iter->remaining--;

        if (iter->i >= p->num_children && iter->i >= 0)
                iter->i = p->num_children - 1;

        if (iter->i >= 0) 
                return p->children[iter->i--];

        iter->depth--;
        do {
                decref_later((PyObject *) p);
                if (!iter->depth) {
                        iter->remaining = 0;
                        iter->leaf = NULL;
                        return NULL;
                }
                p = iter->stack[--iter->depth].lst;
                i = iter->stack[iter->depth].i;

                if (i >= p->num_children && i >= 0)
                        i = p->num_children - 1;
        } while (i < 0);
        
        assert(iter->stack[iter->depth].lst == p);
        iter->stack[iter->depth++].i = i-1;

        while (!p->leaf) {
                p = (PyBList *) p->children[i];
                Py_INCREF(p);
                i = p->num_children-1;
                iter->stack[iter->depth].lst = p;
                iter->stack[iter->depth++].i = i-1;
        }

        iter->leaf = iter->stack[iter->depth-1].lst;
        iter->i = iter->stack[iter->depth-1].i;

        return p->children[i];
}

static PyObject *
py_blist_reversed(PyBList *seq)
{
        blistiterobject *it;

        invariants(seq, VALID_USER | VALID_NEWREF);
        
        DANGER_BEGIN
        it = PyObject_GC_New(blistiterobject,
                             &PyBListReverseIter_Type);
        DANGER_END
        if (it == NULL)
                return _ob(NULL);

        if (seq->leaf) {
                /* Speed up common case */
                it->iter.leaf = seq;
                it->iter.i = seq->n-1;
                it->iter.depth = 1;
                it->iter.remaining = seq->n;
                Py_INCREF(seq);
        } else 
                riter_init(&it->iter, seq);

        PyObject_GC_Track(it);
        return _ob((PyObject *) it);
}

static PyObject *blistiter_prev(PyObject *oit)
{
        blistiterobject *it = (blistiterobject *) oit;
        PyObject *obj;
        
        /* Speed up common case */
        PyBList *p;
        p = it->iter.leaf;
        if (it->iter.remaining == 0)
                return NULL;

        if (it->iter.i >= p->num_children && it->iter.i >= 0)
                it->iter.i = p->num_children - 1;
        
        if (it->iter.i >= 0) {
                it->iter.remaining--;
                obj = p->children[it->iter.i--];
                Py_INCREF(obj);
                return obj;
        }

        obj = iter_prev(&it->iter);
        if (obj != NULL)
                Py_INCREF(obj);

        _decref_flush();
        return obj;
}

PyTypeObject PyBListReverseIter_Type = {
        PyObject_HEAD_INIT(NULL)
        0,                                      /* ob_size */
        "blistreverseiterator",                 /* tp_name */
        sizeof(blistiterobject),                /* tp_basicsize */
        0,                                      /* tp_itemsize */
        /* methods */
        blistiter_dealloc,                      /* tp_dealloc */
        0,                                      /* tp_print */
        0,                                      /* tp_getattr */
        0,                                      /* tp_setattr */
        0,                                      /* tp_compare */
        0,                                      /* tp_repr */
        0,                                      /* tp_as_number */
        0,                                      /* tp_as_sequence */
        0,                                      /* tp_as_mapping */
        0,                                      /* tp_hash */
        0,                                      /* tp_call */
        0,                                      /* tp_str */
        PyObject_GenericGetAttr,                /* tp_getattro */
        0,                                      /* tp_setattro */
        0,                                      /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
        0,                                      /* tp_doc */
        blistiter_traverse,                     /* tp_traverse */
        0,                                      /* tp_clear */
        0,                                      /* tp_richcompare */
        0,                                      /* tp_weaklistoffset */
        PyObject_SelfIter,                      /* tp_iter */
        blistiter_prev,                         /* tp_iternext */
        blistiter_methods,                      /* tp_methods */
        0,                                      /* tp_members */
};

/************************************************************************
 * A forest is an array of BList tree structures.
 */

typedef struct Forest
{
        unsigned num_leafs;
        unsigned num_trees;
        unsigned max_trees;
        PyBList **list;        
} Forest;

static PyBList *
forest_get_leaf(Forest *forest)
{
        PyBList *node = forest->list[--forest->num_trees];
        PyBList **list;
        while (!node->leaf) {
                int i;
                while (forest->num_trees + node->num_children
                       > forest->max_trees) {
                        list = forest->list;
                        forest->max_trees *= 2;
                        PyMem_Resize(list, PyBList*,forest->max_trees);
                        if (list == NULL) {
                                PyErr_NoMemory();
                                return NULL;
                        }
                        forest->list = list;
                }
                        
                for (i = node->num_children - 1; i >= 0; i--)
                        forest->list[forest->num_trees++]
                                = (PyBList *) node->children[i];

                node->num_children = 0;
                SAFE_DECREF(node);
                node = forest->list[--forest->num_trees];
        }

        return node;
}

#define MAX_FREE_FORESTS 4
static PyBList **forest_saved[MAX_FREE_FORESTS];
static unsigned forest_max_trees[MAX_FREE_FORESTS];
static unsigned num_free_forests = 0;

static Forest *
forest_init(Forest *forest)
{
        forest->num_trees = 0;
        forest->num_leafs = 0;
        if (num_free_forests) {
                forest->list = forest_saved[--num_free_forests];
                forest->max_trees = forest_max_trees[num_free_forests];
        } else {
                forest->max_trees = LIMIT; /* enough for O(LIMIT**2) items */
                forest->list = PyMem_New(PyBList *, forest->max_trees);
                if (forest->list == NULL)
                        return (Forest *) PyErr_NoMemory();
        }
        return forest;
}

static int forest_append(Forest *forest, PyBList *leaf)
{
        Py_ssize_t power = LIMIT;

        if (!leaf->num_children) {  /* Don't bother adding empty leaf nodes */
                SAFE_DECREF(leaf);
                return 0;
        }

        leaf->n = leaf->num_children;

        if (forest->num_trees == forest->max_trees) {
                PyBList **list = forest->list;

                forest->max_trees <<= 1;
                PyMem_Resize(list, PyBList *, forest->max_trees);
                if (list == NULL) {
                        PyErr_NoMemory();
                        return -1;
                }
                forest->list = list;
        }

        forest->list[forest->num_trees++] = leaf;
        forest->num_leafs++;

        while (forest->num_leafs % power == 0) {
                struct PyBList *parent = blist_new();
                int x;

                if (parent == NULL) {
                        PyErr_NoMemory();
                        return -1;
                }
                parent->leaf = 0;
                memcpy(parent->children,
                       &forest->list[forest->num_trees - LIMIT],
                       sizeof (PyBList *) * LIMIT);
                parent->num_children = LIMIT;
                forest->num_trees -= LIMIT;
                x = blist_underflow(parent, LIMIT - 1);
                assert(!x); (void) x;

                forest->list[forest->num_trees++] = parent;
                power *= LIMIT;
        }
        
        return 0;
}

/* Like forest_append(), but handles the case where the previously
 * added leaf is in an underflow state. */
static int
forest_append_safe(Forest *forest, PyBList *leaf)
{
        PyBList *last;
        
        if (forest->num_trees == 0)
                goto append;
        
        last = forest->list[forest->num_trees-1];

        if (!last->leaf || last->num_children >= HALF)
                goto append;
        
        if (last->num_children + leaf->num_children <= LIMIT) {
                copy(last, last->num_children, leaf, 0, leaf->num_children);
                last->num_children += leaf->num_children;
                last->n += leaf->num_children;
                leaf->num_children = 0;
        } else {
                int needed = HALF - last->num_children;
                
                copy(last, last->num_children, leaf, 0, needed);
                last->num_children += needed;
                last->n += needed;
                shift_left(leaf, needed, needed);
                leaf->num_children -= needed;
        }

 append:
        return forest_append(forest, leaf);
}

static void
forest_delete(Forest *forest)
{
        int i;
        for (i = 0; i < forest->num_trees; i++)
                decref_later((PyObject *) forest->list[i]);
        if (num_free_forests < MAX_FREE_FORESTS && forest->max_trees == LIMIT){
                forest_saved[num_free_forests] = forest->list;
                forest_max_trees[num_free_forests++] = forest->max_trees;
        } else 
                PyMem_Free(forest->list);
}

static void
forest_delete_now(Forest *forest)
{
        int i;
        for (i = 0; i < forest->num_trees; i++)
                Py_DECREF((PyObject *) forest->list[i]);
        if (num_free_forests < MAX_FREE_FORESTS && forest->max_trees == LIMIT){
                forest_saved[num_free_forests] = forest->list;
                forest_max_trees[num_free_forests++] = forest->max_trees;
        } else 
                PyMem_Free(forest->list);
}

/* Combine the forest into a final BList */
static PyBList *forest_finish(Forest *forest)
{
        PyBList *out_tree = NULL; /* The final BList we are building */
        int out_height = 0;       /* It's height */
        int group_height = 1;     /* height of the next group from forest */

        while(forest->num_trees) {
                int n = forest->num_leafs % LIMIT;
                PyBList *group;
                int adj;

                forest->num_leafs /= LIMIT;
                group_height++;

                if (!n) continue;  /* No nodes at this height */

                /* Merge nodes of the same height into 1 node, and
                 * merge it into our output BList.
                 */
                group = blist_new();
                if (group == NULL) {
                        forest_delete(forest);
                        xdecref_later((PyObject *) out_tree);
                        return NULL;
                }
                group->leaf = 0;
                memcpy(group->children,
                       &forest->list[forest->num_trees - n],
                       sizeof (PyBList *) * n);
                group->num_children = n;
                forest->num_trees -= n;
                adj = blist_underflow(group, n - 1);
                if (out_tree == NULL) {
                        out_tree = group;
                        out_height = group_height - adj;
                } else {
                        out_tree = blist_concat_roots(group, group_height- adj,
                                                      out_tree, out_height,
                                                      &out_height);
                }
        }

        forest_delete(forest);
        
        return out_tree;
}

/************************************************************************
 * Functions that rely on forests.
 */

static int
blist_init_from_array(PyBList *self, PyObject **src, Py_ssize_t n)
{
        int i;
        PyBList *final, *cur;
        PyObject **dst;
        PyObject **stop = &src[n];
        Forest forest;
        
        if (n <= LIMIT) {
                dst = self->children;
                while (src < stop) {
                        Py_INCREF(*src);
                        *dst++ = *src++;
                }
                self->num_children = n;
                self->n = n;
                return 0;
        }

        if (forest_init(&forest) == NULL)
                return -1;
        cur = blist_new();
        if (cur == NULL)
                goto error2;
        dst = cur->children;
        i = 0;
        
        while (src < stop) {
                if (i == LIMIT) {
                        cur->num_children = LIMIT;
                        if (forest_append(&forest, cur) < 0)
                                goto error;
                        cur = blist_new();
                        if (cur == NULL)
                                goto error2;
                        dst = cur->children;
                        i = 0;
                }

                Py_INCREF(*src);
                dst[i++] = *src++;
        }

        if (i) {
                cur->num_children = i;
                if (forest_append(&forest, cur) < 0) {
                error:
                        Py_DECREF(cur);
                error2:
                        forest_delete(&forest);
                        return -1;
                }
        } else {
                Py_DECREF(cur);
        }

        final = forest_finish(&forest);
        blist_become_and_consume(self, final);

        ext_mark(self, 0, DIRTY);
        SAFE_DECREF(final);
        
        return 0;
}

static int
blist_init_from_seq(PyBList *self, PyObject *b)
{
        PyObject *it;
        PyObject *(*iternext)(PyObject *);
        PyBList *cur, *final;
        Forest forest;

        invariants(self, VALID_RW);
        
        if (PyBList_Check(b)) {
                /* We can copy other BLists in O(1) time :-) */
                blist_become(self, (PyBList *) b);
                ext_mark(self, 0, DIRTY);
                return _int(0);
        }

        if (PyTuple_CheckExact(b)) {
                PyTupleObject *t = (PyTupleObject *) b;
                return _int(blist_init_from_array(self, t->ob_item,
                                                  t->ob_size));
        }
#ifndef Py_BUILD_CORE
        if (PyList_CheckExact(b)) {
                PyListObject *l = (PyListObject *) b;
                return _int(blist_init_from_array(self, l->ob_item,
                                                  l->ob_size));
        }
#endif
        
        it = PyObject_GetIter(b);
        if (it == NULL)
                return _int(-1);
        iternext = *it->ob_type->tp_iternext;

        /* Try common case of len(sequence) <= LIMIT */
        for (self->num_children = 0; self->num_children < LIMIT;
             self->num_children++) {
                PyObject *item;

                DANGER_BEGIN
                item = iternext(it);
                DANGER_END

                if (item == NULL) {
                        self->n = self->num_children;
                        if (PyErr_Occurred()) {
                                if (PyErr_ExceptionMatches(PyExc_StopIteration))
                                        PyErr_Clear();
                                else
                                        goto error;
                        }
                        goto done;
                }

                self->children[self->num_children] = item;
        }

        /* No such luck, build bottom-up instead.  The sequence data
         * so far goes in a leaf node. */

        cur = blist_new();
        if (cur == NULL)
                goto error;
        blist_become_and_consume(cur, self);

        if (forest_init(&forest) == NULL) {
                decref_later(it);
                decref_later((PyObject *) cur);
                return _int(-1);
        }
                
        if (0 > forest_append(&forest, cur))
                goto error2;
        
        cur = blist_new();
        if (cur == NULL)
                goto error2;

        while (1) {
                PyObject *item;
                DANGER_BEGIN
                item = iternext(it);
                DANGER_END
                if (item == NULL) {
                        if (PyErr_Occurred()) {
                                if (PyErr_ExceptionMatches(PyExc_StopIteration))
                                        PyErr_Clear();
                                else 
                                        goto error2;
                        }
                        break;
                }

                if (cur->num_children == LIMIT) {
                        if (forest_append(&forest, cur) < 0) goto error2;
                        cur = blist_new();
                        if (cur == NULL)
                                goto error2;
                }

                cur->children[cur->num_children++] = item;
        }

        if (cur->num_children) {
                if (forest_append(&forest, cur) < 0) goto error2;
                cur->n = cur->num_children;
        } else {
                SAFE_DECREF(cur);
        }

        final = forest_finish(&forest);
        blist_become_and_consume(self, final);
        SAFE_DECREF(final);
        
 done:
        ext_mark(self, 0, DIRTY);
        decref_later(it);
        return _int(0);
        
 error2:
        DANGER_BEGIN
        Py_XDECREF((PyObject *) cur);
        forest_delete_now(&forest);
        DANGER_END
 error:
        DANGER_BEGIN
        Py_DECREF(it);
        DANGER_END
        blist_CLEAR(self);
        return _int(-1);
}

/* Utility function for performing repr() */
static int
blist_repr_r(PyBList *self)
{
        int i;
        PyObject *s;

        invariants(self, VALID_RW|VALID_PARENT);
        
        if (self->leaf) {
                for (i = 0; i < self->num_children; i++) {
                        s = PyObject_Repr(self->children[i]);
                        if (s == NULL)
                                return _int(-1);
                        Py_DECREF(self->children[i]);
                        self->children[i] = s;
                }
        } else {
                for (i = 0; i < self->num_children; i++) {
                        PyBList *child = blist_prepare_write(self, i);
                        int status = blist_repr_r(child);
                        if (status < 0)
                                return _int(status);
                }
        }

        return _int(0);
}

PyObject *
ext_make_clean_set(PyBListRoot *root, Py_ssize_t i, PyObject *v)
{
        PyBList *p = (PyBList *) root;
        PyBList *next;
        int k;
        Py_ssize_t so_far, offset = 0;
        PyObject *old_value;
        int did_mark = 0;

        while (!p->leaf) {
                blist_locate(p, i, (PyObject **) &next, &k, &so_far);
                if (next->ob_refcnt <= 1)
                        p = next;
                else {
                        p = blist_prepare_write(p, k);
                        if (!did_mark) {
                                ext_mark((PyBList *) root, offset, DIRTY);
                                did_mark = 1;
                        }
                }
                assert(i >= so_far);
                i -= so_far;
                offset += so_far;
        }

        if (!root->leaf)
                ext_mark_clean(root, offset, p, 1);

        old_value = p->children[i];
        p->children[i] = v;
        return old_value;
}

PyObject *
blist_ass_item_return_slow(PyBListRoot *root, Py_ssize_t i, PyObject *v)
{
        int dirty_offset;
        assert(i >= 0);
        invariants(root, VALID_RW);
        PyObject *rv;
        int ioffset = i / INDEX_FACTOR;

        if (root->leaf || ext_is_dirty(root, i, &dirty_offset)
            || !GET_BIT(root->setclean_list, ioffset)) {
                rv = ext_make_clean_set(root, i, v);
        } else {
                Py_ssize_t offset = root->offset_list[ioffset];
                PyBList *p = root->index_list[ioffset];
                assert(i >= offset);
                assert(p);
                assert(p->leaf);
                if (i < offset + p->n) {
                good:
                        rv = p->children[i - offset];
                        p->children[i - offset] = v;
                        if (dirty_offset >= 0)
                                ext_make_clean(root, dirty_offset);
                } else if (ext_is_dirty(root,i + INDEX_FACTOR,&dirty_offset)
                        || !GET_BIT(root->setclean_list, ioffset+1)) {
                        rv = ext_make_clean_set(root, i, v);
                } else {
                        ioffset++;
                        assert(ioffset < root->index_length);
                        offset = root->offset_list[ioffset];
                        p = root->index_list[ioffset];
                        assert(p);
                        assert(p->leaf);
                        assert(i < offset + p->n);

                        goto good;
                }
        }

        return _ob(rv);
}

static inline PyObject *
blist_ass_item_return(PyBList *self, Py_ssize_t i, PyObject *v)
{
        Py_INCREF(v);
        if (self->leaf) {
                PyObject *rv = self->children[i];
                self->children[i] = v;
                return rv;
        }

        return blist_ass_item_return2((PyBListRoot*)self, i, v);
}

#ifndef Py_BUILD_CORE
static PyObject *
blist_richcompare_list(PyBList *v, PyListObject *w, int op)
{
        Py_ssize_t i;
        iter_t it;
        int cmp;
        PyObject *ret;
        int v_stopped = 0;
        int w_stopped = 0;

        invariants(v, VALID_RW);
        
        iter_init(&it, v);

        if (v->n != w->ob_size && (op == Py_EQ || op == Py_NE)) {
                /* Shortcut: if the lengths differe, the lists differ */
                PyObject *res;
                if (op == Py_EQ) {
                false:
                        res = Py_False;
                } else {
                true:
                        res = Py_True;
                }
                iter_cleanup(&it);
                Py_INCREF(res);
                return _ob(res);
        }

        /* Search for the first index where items are different */
        
        for (i = 0 ;; i++) {
                PyObject *item1 = iter_next(&it);
                PyObject *item2;

                if (item1 == NULL)
                        v_stopped = 1;

                if (i == w->ob_size)
                        w_stopped = 1;

                if (v_stopped || w_stopped)
                        break;

                item2 = w->ob_item[i];

                DANGER_BEGIN
                cmp = PyObject_RichCompareBool(item1, item2, Py_EQ);
                DANGER_END
                
                if (cmp < 0) {
                        goto error;
                } else if (!cmp) {
                        if (op == Py_EQ) goto false;
                        if (op == Py_NE) goto true;
                        iter_cleanup(&it);
                        DANGER_BEGIN
                        ret = PyObject_RichCompare(item1, item2, op);
                        DANGER_END
                        return ret;
                }
        }

        switch (op) {
        case Py_LT: cmp = v_stopped && !w_stopped; break;
        case Py_LE: cmp = v_stopped; break;
        case Py_EQ: cmp = v_stopped == w_stopped; break;
        case Py_NE: cmp = v_stopped != w_stopped; break;
        case Py_GT: cmp = !v_stopped && w_stopped; break;
        case Py_GE: cmp = w_stopped; break;
        default: goto error; /* cannot happen */
        }

        if (cmp) goto true;
        else goto false;

 error:
        iter_cleanup(&it);
        return _ob(NULL);
}
#endif

static PyObject *
blist_richcompare_item(int c, int op, PyObject *item1, PyObject *item2)
{
        PyObject *ret;
        
        if (c < 0)
                return NULL;
        if (!c) {
                if (op == Py_EQ) {
                        Py_INCREF(Py_False);
                        return Py_False;
                }
                if (op == Py_NE) {
                        Py_INCREF(Py_True);
                        return Py_True;
                }
                DANGER_BEGIN
                ret = PyObject_RichCompare(item1, item2, op);
                DANGER_END
                return ret;
        }

        /* Impossible to get here */
        assert(0);
        return NULL;
}

#define Py_RETURN_TRUE return Py_INCREF(Py_True), Py_True
#define Py_RETURN_FALSE return Py_INCREF(Py_False), Py_False

static PyObject *blist_richcompare_len(PyBList *v, PyBList *w, int op)
{
        /* No more items to compare -- compare sizes */
        switch (op) {
        case Py_LT: if (v->n <  w->n) Py_RETURN_TRUE; else Py_RETURN_FALSE;
        case Py_LE: if (v->n <= w->n) Py_RETURN_TRUE; else Py_RETURN_FALSE;
        case Py_EQ: if (v->n == w->n) Py_RETURN_TRUE; else Py_RETURN_FALSE;
        case Py_NE: if (v->n != w->n) Py_RETURN_TRUE; else Py_RETURN_FALSE;
        case Py_GT: if (v->n >  w->n) Py_RETURN_TRUE; else Py_RETURN_FALSE;
        case Py_GE: if (v->n >= w->n) Py_RETURN_TRUE; else Py_RETURN_FALSE;
        default: return NULL; /* cannot happen */
        }
}

static PyObject *blist_richcompare_slow(PyBList *v, PyBList *w, int op)
{
        /* Search for the first index where items are different */
        PyObject *item1, *item2;
        iter_t it1, it2;
        int c;
        PyBList *leaf1, *leaf2;

        iter_init(&it1, v);
        iter_init(&it2, w);

        leaf1 = it1.leaf;
        leaf2 = it2.leaf;
        do {
                if (it1.i < leaf1->num_children) {
                        item1 = leaf1->children[it1.i++];
                } else {
                        item1 = iter_next(&it1);
                        leaf1 = it1.leaf;
                        if (item1 == NULL) {
                        compare_len:
                                iter_cleanup(&it1);
                                iter_cleanup(&it2);
                                return blist_richcompare_len(v, w, op);
                        }
                }

                if (it2.i < leaf2->num_children) {
                        item2 = leaf2->children[it2.i++];
                } else {
                        item2 = iter_next(&it2);
                        leaf2 = it2.leaf;
                        if (item2 == NULL)
                                goto compare_len;
                }

                DANGER_BEGIN
                c = PyObject_RichCompareBool(item1, item2, Py_EQ);
                DANGER_END
        } while (c >= 1);
        
        iter_cleanup(&it1);
        iter_cleanup(&it2);
        return blist_richcompare_item(c, op, item1, item2);
}

static PyObject *
blist_richcompare_blist(PyBList *v, PyBList *w, int op)
{
        int i, c;

        if (v->n != w->n) {
                /* Shortcut: if the lengths differ, the lists differ */
                if (op == Py_EQ) {
                        Py_INCREF(Py_False);
                        return Py_False;
                } else if (op == Py_NE) {
                        Py_INCREF(Py_True);
                        return Py_True;
                }
        }

        if (!v->leaf || !w->leaf)
                return blist_richcompare_slow(v, w, op);
                
        for (i = 0; i < v->num_children && i < w->num_children; i++) {
                DANGER_BEGIN
                c = PyObject_RichCompareBool(v->children[i],
                                             w->children[i],Py_EQ);
                DANGER_END
                if (c < 1)
                        return blist_richcompare_item(c, op, v->children[i],
                                                      w->children[i]);
        }
        return blist_richcompare_len(v, w, op);
                                     
}

/* Swiped from listobject.c */
/* Reverse a slice of a list in place, from lo up to (exclusive) hi. */
static void
reverse_slice(PyObject **lo, PyObject **hi)
{
        assert(lo && hi);

        --hi;
        while (lo < hi) {
                PyObject *t = *lo;
                *lo = *hi;
                *hi = t;
                ++lo;
                --hi;
        }
}

static void blist_double(PyBList *self)
{
        if (self->num_children > HALF) {
                blist_extend_blist(self, self);
                return;
        }

        copyref(self, self->num_children, self, 0, self->num_children);
        self->num_children *= 2;
        self->n *= 2;
}

static int
blist_extend(PyBList *self, PyObject *other)
{
        int err;
        PyBList *bother = NULL;
        
        invariants(self, VALID_PARENT|VALID_RW);

        if (PyBList_Check(other)) {
                err = blist_extend_blist(self, (PyBList *) other);
                goto done;
        }

        bother = blist_user_new();
        err = blist_init_from_seq(bother, other);
        if (err < 0)
                goto done;
        err = blist_extend_blist(self, bother);
        ext_mark(self, 0, DIRTY);

 done:
        SAFE_XDECREF(bother);
        return _int(err);
}        

static PyObject *
blist_repeat(PyBList *self, Py_ssize_t n)
{
        Py_ssize_t mask;
        PyBList *power = NULL, *rv, *remainder = NULL;
        Py_ssize_t remainder_n = 0;

        invariants(self, VALID_PARENT);

        if (n <= 0 || self->n == 0)
                return _ob((PyObject *) blist_user_new());

        if (n > (PY_SSIZE_T_MAX / self->n / 2))
                return _ob(PyErr_NoMemory());
        
        rv = blist_user_new();
        if (rv == NULL)
                return _ob(NULL);

        if (n == 1) {
                blist_become(rv, self);
                ext_mark(rv, 0, DIRTY);
                return _ob((PyObject *) rv);
        }

        if (self->num_children > HALF)
                blist_become(rv, self);
        else {
                Py_ssize_t fit, fitn, so_far;

                rv->leaf = self->leaf;
                fit = LIMIT / self->num_children;
                if (fit > n) fit = n;
                fitn = fit * self->num_children;
                xcopyref(rv, 0, self, 0, self->num_children);
                so_far = self->num_children;
                while (so_far*2 < fitn) {
                        xcopyref(rv, so_far, rv, 0, so_far);
                        so_far *= 2;
                }
                xcopyref(rv, so_far, rv, 0, (fitn - so_far));
                
                rv->num_children = fitn;
                rv->n = self->n * fit;
                check_invariants(rv);

                if (fit == n) 
                        return _ob((PyObject *) rv);

                remainder_n = n % fit;
                n /= fit;

                if (remainder_n) {
                        remainder = blist_user_new();
                        if (remainder == NULL)
                                goto error;
                        remainder->n = self->n * remainder_n;
                        remainder_n *= self->num_children;
                        remainder->leaf = self->leaf;
                        xcopyref(remainder, 0, rv, 0, remainder_n);
                        remainder->num_children = remainder_n;
                        check_invariants(remainder);
                }
        }

        if (n == 0) 
                goto do_remainder;
                
        power = rv;
        rv = blist_user_new();
        if (rv == NULL) {
                SAFE_XDECREF(remainder);
        error:
                SAFE_DECREF(power);
                return _ob(NULL);
        }

        if (n & 1)
                blist_become(rv, power);

        for (mask = 2; mask <= n; mask <<= 1) {
                blist_double(power);
                if (mask & n)
                        blist_extend_blist(rv, power);
        }
        SAFE_DECREF(power);

 do_remainder:
        
        if (remainder) {
                blist_extend_blist(rv, remainder);
                SAFE_DECREF(remainder);
        }

        check_invariants(rv);
        ext_mark(rv, 0, DIRTY);
        return _ob((PyObject *) rv);
}

static void
blist_reverse(PyBList *self)
{
        invariants(self, VALID_PARENT|VALID_RW);
        
        if (self->num_children > 1) {
                reverse_slice(self->children,
                              &self->children[self->num_children]);
                if (!self->leaf) {
                        int i;
                        for (i = 0; i < self->num_children; i++) {
                                PyBList *p = blist_prepare_write(self, i);
                                blist_reverse(p);
                        }
                }
        }

        _void();
}

static int
blist_append(PyBList *self, PyObject *v)
{
        PyBList *overflow;
        
        invariants(self, VALID_PARENT|VALID_RW);

        if (self->n == PY_SSIZE_T_MAX) {
                PyErr_SetString(PyExc_OverflowError,
                                "cannot add more objects to list");
                return _int(-1);
        }
        
        overflow = ins1(self, self->n, v);
        if (overflow)
                blist_overflow_root(self, overflow);
        ext_mark(self, 0, DIRTY);

        return _int(0);
}

/************************************************************************
 * Sorting code
 *
 * Bits and pieces swiped from Python's listobject.c
 *
 * Invariant: In case of an error, any sort function returns the list
 * to a valid state before returning.  "Valid" means that each item
 * originally in the list is still in the list.  No removals, no
 * additions, and no changes to the reference counters.
 *
 ************************************************************************/

/* If COMPARE is NULL, calls PyObject_RichCompareBool with Py_LT, else calls
 * islt.  This avoids a layer of function call in the usual case, and
 * sorting does many comparisons.
 * Returns -1 on error, 1 if x < y, 0 if x >= y.
 */

#ifndef Py_DEBUG
#define ISLT(X, Y, COMPARE) ((COMPARE) == NULL ?                        \
                             PyObject_RichCompareBool(X, Y, Py_LT) :    \
                             islt(X, Y, COMPARE))
#else
static int
_rich_compare_bool(PyObject *x, PyObject *y)
{
        int ret;
        DANGER_BEGIN
        ret = PyObject_RichCompareBool(x, y, Py_LT);
        DANGER_END
        return ret;
}

#define ISLT(X, Y, COMPARE) ((COMPARE) == NULL ?                        \
                             _rich_compare_bool(X, Y) :    \
                             islt(X, Y, COMPARE))
#endif

typedef struct {
        PyObject *compare;
        PyObject *keyfunc;
} compare_t;

/* XXX

   Efficiency improvement:
   Keep one PyTuple in compare_t and just change what it points to.
   We can also skip all the INCREF/DECREF stuff then and just borrow
   references
*/

static int islt(PyObject *x, PyObject *y, const compare_t *compare)
{
        PyObject *res;
        PyObject *args;
        Py_ssize_t i;

        if (compare->keyfunc != NULL) {
                DANGER_BEGIN
                x = PyObject_CallFunctionObjArgs(compare->keyfunc, x, NULL);
                DANGER_END
                if (x == NULL) return -1;
                DANGER_BEGIN
                y = PyObject_CallFunctionObjArgs(compare->keyfunc, y, NULL);
                DANGER_END
                if (y == NULL) {
                        DANGER_BEGIN
                        Py_DECREF(x);
                        DANGER_END
                        return -1;
                }
        } else {
                Py_INCREF(x);
                Py_INCREF(y);
        }

        if (compare->compare == NULL) {
                DANGER_BEGIN
                i = PyObject_RichCompareBool(x, y, Py_LT);
                Py_DECREF(x);
                Py_DECREF(y);
                DANGER_END
                if (i < 0)
                        return -1;
                return i;
        }

        DANGER_BEGIN
        args = PyTuple_New(2);
        DANGER_END

        if (args == NULL) {
                DANGER_BEGIN
                Py_DECREF(x);
                Py_DECREF(y);
                DANGER_END
                return -1;
        }

        PyTuple_SET_ITEM(args, 0, x);
        PyTuple_SET_ITEM(args, 1, y);
        DANGER_BEGIN
        res = PyObject_Call(compare->compare, args, NULL);
        Py_DECREF(args);
        DANGER_END
        if (res == NULL)
                return -1;
        if (!PyInt_CheckExact(res)) {
                PyErr_Format(PyExc_TypeError,
                             "comparison function must return int, not %.200s",
                             res->ob_type->tp_name);
                Py_DECREF(res);
                return -1;
        }
        i = PyInt_AsLong(res);
        Py_DECREF(res);
        return i < 0;
}

#define INSERTION_THRESH 0
#define BINARY_THRESH 10

#if 0
/* Compare X to Y via "<".  Goto "fail" if the comparison raises an
   error.  Else "k" is set to true iff X<Y, and an "if (k)" block is
   started.  It makes more sense in context <wink>.  X and Y are PyObject*s.
*/
#define IFLT(X, Y) if ((k = ISLT(X, Y, compare)) < 0) goto fail;  \
                   if (k)

#define SWAP(x, y) {PyObject *_tmp = x; x = y; y = _tmp;}
#define TESTSWAP(x, y) IFLT(y, x) SWAP(x, y)

static int
network_sort(PyObject **array, int n, const compare_t *compare)
{
        int k;

        switch(n) {
        case 0:
        case 1:
                return 0;
        case 2:
                TESTSWAP(array[0], array[1]);
                return 0;
        case 3:
                TESTSWAP(array[0], array[1]);
                TESTSWAP(array[0], array[2]);
                TESTSWAP(array[1], array[2]);
                return 0;
        case 4:
                TESTSWAP(array[0], array[1]);
                TESTSWAP(array[2], array[3]);
                TESTSWAP(array[0], array[2]);
                TESTSWAP(array[1], array[3]);
                TESTSWAP(array[1], array[2]);
                return 0;
        case 5:
                TESTSWAP(array[0], array[1]);
                TESTSWAP(array[3], array[4]);
                TESTSWAP(array[0], array[2]);
                TESTSWAP(array[1], array[2]);
                TESTSWAP(array[0], array[3]);
                TESTSWAP(array[2], array[3]);
                TESTSWAP(array[1], array[4]);
                TESTSWAP(array[1], array[2]);
                TESTSWAP(array[3], array[4]);
                return 0;
        default:
                /* Should not be possible */
                assert (0);
                abort();
        }

 fail:
        return -1;
}

static int insertion_sort(PyObject **array, int n, const compare_t *compare)
{
        int i, j;
        PyObject *tmp;
        for (i = 1; i < n; i++) {
                tmp = array[i];
                for (j = i; j >= 1; j--) {
                        int c = ISLT(tmp, array[j-1], compare);
                        if (c < 0) {
                                array[j] = tmp;
                                return -1;
                        }
                        if (c == 0)
                                break;
                        array[j] = array[j-1];
                }
                array[j] = tmp;
        }

        return 0;
}

static int binary_sort(PyObject **array, int n, const compare_t *compare)
{
        int i, j, low, high, mid, c;
        PyObject *tmp;

        for (i = 1; i < n; i++) {
                tmp = array[i];

                c = ISLT(tmp, array[i-1], compare);
                if (c < 0)
                        return -1;
                if (c == 0)
                        continue;
                
                low = 0;
                high = i-1;

                while (low < high) {
                        mid = low + (high - low)/2;
                        c = ISLT(tmp, array[mid], compare);
                        if (c < 0) 
                                return -1;
                        if (c == 0)
                                low = mid+1;
                        else
                                high = mid;
                }

                for (j = i; j >= low; j--)
                        array[j] = array[j-1];

                array[low] = tmp;
        }

        return 0;
}
#endif

static int
mini_merge(PyObject **array, int middle, int n, const compare_t *compare)
{
        int c, ret = 0;

        PyObject *copy[LIMIT];
        PyObject **left;
        PyObject **right = &array[middle];
        PyObject **rend = &array[n];
        PyObject **lend = &copy[middle];
        PyObject **src;
        PyObject **dst;

        assert (middle <= LIMIT);

        for (left = array; left < right; left++) {
                c = ISLT(*right, *left, compare);
                if (c < 0)
                        return -1;
                if (c)
                        goto normal;
        }

        return 0;
        
 normal:
        src = left;
        dst = left;
        
        for (left = copy; src < right; left++)
                *left = *src++;

        lend = left;

        *dst++ = *right++;        
        
        for (left = copy; left < lend && right < rend; dst++) {
                c = ISLT(*right, *left, compare);
                if (c < 0) {
                        ret = -1;
                        goto done;
                }
                if (c == 0)
                        *dst = *left++;
                else
                        *dst = *right++;
        }

 done:
        while (left < lend)
                *dst++ = *left++;

        return ret;
}

#define RUN_THRESH 5

static int
gallop_sort(PyObject **array, int n, const compare_t *compare)
{
        int i, j;
        int run_length = 1, run_dir = 1;
        PyObject **runs[LIMIT/RUN_THRESH+2];
        int ns[LIMIT/RUN_THRESH+2];
        int num_runs = 0;
        PyObject **run = array;

        if (n < 2) return 0;
        
        for (i = 1; i < n; i++) {
                int c = ISLT(array[i], array[i-1], compare);
                if (c < 0)
                        return -1;
                c = !!c; /* Ensure c is 0 or 1 */
                if (run_length == 1)
                        run_dir = c;
                if (c == run_dir)
                        run_length++;
                else if (run_length >= RUN_THRESH) {
                                if (run_dir > 0)
                                        reverse_slice(run, &array[i]);
                                runs[num_runs] = run;
                                ns[num_runs++] = run_length;
                                run = &array[i];
                                run_length = 1;
                } else {
                        int low = run - array;
                        int high = i-1;
                        int mid;
                        PyObject *tmp = array[i];

                        /* XXX: Is this a stable sort? */
                        
                        while (low < high) {
                                mid = low + (high - low)/2;
                                c = ISLT(tmp, array[mid], compare);
                                if (c < 0) 
                                        return -1;
                                if ((!!c) == run_dir)
                                        low = mid+1;
                                else
                                        high = mid;
                        }

                        for (j = i; j >= low; j--)
                                array[j] = array[j-1];

                        array[low] = tmp;

                        run_length++;
                }
        }

        if (run_dir > 0)
                reverse_slice(run, &array[i]);
        runs[num_runs] = run;
        ns[num_runs++] = run_length;

        while(num_runs > 1) {
                for (i = 0; i < num_runs/2; i++) {
                        int total = ns[2*i] + ns[2*i+1];
                        if (0 > mini_merge(runs[2*i], ns[2*i], total,
                                           compare)) {
                                /* List valid due to invariants */
                                return -1;
                        }

                        runs[i] = runs[2*i];
                        ns[i] = total;
                }

                if (num_runs & 1) {
                        runs[i] = runs[num_runs - 1];
                        ns[i] = ns[num_runs - 1];
                }
                num_runs = (num_runs+1)/2;
        }

        assert(ns[0] == n);

        return 0;
        
}

#if 0
static int
mini_merge_sort(PyObject **array, int n, const compare_t *compare)
{
        int i, run_size = BINARY_THRESH;
        
        for (i = 0; i < n; i += run_size) {
                int len = run_size;
                if (n - i < len)
                        len = n - i;
                if (binary_sort(&array[i], len, compare) < 0)
                        return -1;
        }

        run_size *= 2;
        while (run_size < n) {
                for (i = 0; i < n; i += run_size) {
                        int len = run_size;
                        if (n - i < len)
                                len = n - i;
                        if (len <= run_size/2)
                                continue;
                        if (mini_merge(&array[i], run_size/2, len, compare) < 0)
                                return -1;
                }
                run_size *= 2;
        }

        return 0;
}
#endif

static int
is_default_cmp(PyObject *cmpfunc)
{
        PyCFunctionObject *f;
        if (cmpfunc == NULL || cmpfunc == Py_None)
                return 1;
        if (!PyCFunction_Check(cmpfunc))
                return 0;
        f = (PyCFunctionObject *)cmpfunc;
        if (f->m_self != NULL)
                return 0;
        if (!PyString_Check(f->m_module))
                return 0;
        if (strcmp(PyString_AS_STRING(f->m_module), "__builtin__") != 0)
                return 0;
        if (strcmp(f->m_ml->ml_name, "cmp") != 0)
                return 0;
        return 1;
}

static PyBList *
merge(PyBList *self, PyBList *other, const compare_t *compare, int *err)
{
        int c, i, j;
        Forest forest1, forest2, forest_out;
        PyBList *leaf1, *leaf2, *output, *ret;

        *err = 0;
        
#if 0
        c = ISLT(blist_get1(self, self->n-1), blist_get1(other, 0), compare);
        if (c < 0) {
                /* XXX */
                return NULL;
        }
        if (c > 0) {
                blist_extend_blist(self, other);
                Py_DECREF(other);
                return self;
        }
#endif
        
        forest_init(&forest1);
        forest_init(&forest2);
        forest_init(&forest_out);

        /* XXX: Check return values */
        forest_append(&forest1, self);
        forest_append(&forest2, other);

        leaf1 = forest_get_leaf(&forest1);
        leaf2 = forest_get_leaf(&forest2);

        i = 0; /* Index into leaf 1 */
        j = 0; /* Index into leaf 2 */

        output = blist_new();

        while ((forest1.num_trees || i < leaf1->num_children)
               && (forest2.num_trees || j < leaf2->num_children)) {

                /* Check if we need to get a new input leaf node */
                if (i == leaf1->num_children) {
                        leaf1->num_children = 0;
                        SAFE_DECREF(leaf1);
                        leaf1 = forest_get_leaf(&forest1);
                        i = 0;
                }

                if (j == leaf2->num_children) {
                        leaf2->num_children = 0;
                        SAFE_DECREF(leaf2);
                        leaf2 = forest_get_leaf(&forest2);
                        j = 0;
                }

                /* Check if we have filled up an output leaf node */
                if (output->n == LIMIT) {
                        forest_append(&forest_out, output);
                        output = blist_new();
                }

                /* Figure out which input leaf has the lower element */
                c = ISLT(leaf2->children[j], leaf1->children[i], compare);
                if (c < 0) {
                        *err = -1;
                        goto done;
                }
                if (c == 0) {
                        output->children[output->num_children++]
                                = leaf1->children[i++];
                } else {
                        output->children[output->num_children++]
                                = leaf2->children[j++];
                }

                output->n++;
        }

 done:
        /* Append our partially-complete output leaf node to the forest */
        forest_append(&forest_out, output);

        /* Append a partially-consumed input leaf node, if one exists */
        if (i < leaf1->num_children) {
                shift_left(leaf1, i, i);
                leaf1->num_children -= i;
                forest_append_safe(&forest_out, leaf1);
        } else {
                leaf1->num_children = 0;
                SAFE_DECREF(leaf1);
        }

        if (j < leaf2->num_children) {
                shift_left(leaf2, j, j);
                leaf2->num_children -= j;
                forest_append_safe(&forest_out, leaf2);
        } else {
                leaf2->num_children = 0;
                SAFE_DECREF(leaf2);
        }

        /* Append the rest of whichever input forest still has nodes. */

        ret = forest_finish(&forest_out);
        while (forest1.num_trees) {
                PyBList *tree = forest1.list[--forest1.num_trees];
                ret = blist_concat_unknown_roots(ret, tree);
        }
        while (forest2.num_trees) {
                PyBList *tree = forest2.list[--forest2.num_trees];
                ret = blist_concat_unknown_roots(ret, tree);
        }
                
        forest_delete(&forest1);
        forest_delete(&forest2);
                                   
        return ret;
}

static PyBList *
merge_no_compare(PyBList *self, PyBList *other, const compare_t *compare,
                 int *err)
{
        blist_extend_blist(self, other);
        SAFE_DECREF(other);
        return self;
}

static int
sort(PyBList *self, const compare_t *compare)
{
        int i, ret = 0;
        PyBList *s;
        PyBList *(*mergefunc)(PyBList *self, PyBList *other,
                              const compare_t *compare, int *err);

        invariants(self, VALID_PARENT);

        mergefunc = merge;

        if (self->leaf)
                return _int(gallop_sort(self->children, self->num_children,
                                        compare));

        for (i = 0; i < self->num_children; i++) {
                blist_prepare_write(self, i);
                ret = sort((PyBList *) self->children[i], compare);
                if (ret < 0) {
                        mergefunc = merge_no_compare;
                        break;
                }
        }

        while (self->num_children != 1) {
                for (i = 0; i < self->num_children/2; i++) {
                        s = mergefunc((PyBList *) self->children[2*i],
                                      (PyBList *) self->children[2*i+1],
                                      compare, &ret);
                        /* Necessary in case GC traversal occurs in merge() */
                        self->children[2*i] = NULL;
                        self->children[2*i+1] = NULL;

                        self->children[i] = (PyObject *) s;

                        if (ret < 0)
                                mergefunc = merge_no_compare;
                }

                if (self->num_children & 1)
                        self->children[i]
                                = self->children[self->num_children - 1];
                self->num_children = (self->num_children+1)/2;
        }

        blist_become_and_consume(self, (PyBList *) self->children[0]);
        check_invariants(self);
        
        return _int(ret);
}

/************************************************************************
 * Section for functions callable directly by the interpreter.
 *
 * Each of these functions are marked with VALID_USER for debug mode.
 *
 * If they, or any function they call, makes calls to decref_later,
 * they must call decref_flush() just before returning.
 *
 * These functions must not be called directly by other blist
 * functions.  They should *only* be called by the interpreter, to
 * ensure that decref_flush() is the last thing called before
 * returning to the interpreter.
 */

static PyObject *
py_blist_user_tp_new(PyTypeObject *subtype, PyObject *args, PyObject *kwds)
{
        PyBList *self;
        
        if (subtype == &PyUserBList_Type)
                return (PyObject *) blist_user_new();

        self = (PyBList *) subtype->tp_alloc(subtype, 0);
        if (self == NULL)
                return NULL;
        self->children = PyMem_New(PyObject *, LIMIT);
        if (self->children == NULL) {
                subtype->tp_free(self);
                return NULL;
        }

        self->leaf = 1;
        ext_init((PyBListRoot *)self);
        
        return (PyObject *) self;
}

static int
py_blist_init(PyObject *oself, PyObject *args, PyObject *kw)
{
        int ret;
        PyObject *arg = NULL;
        static char *kwlist[] = {"sequence", 0};
        int err;
        PyBList *self;

        invariants(oself, VALID_USER|VALID_DECREF);
        self = (PyBList *) oself;

        DANGER_BEGIN
        err = PyArg_ParseTupleAndKeywords(args, kw, "|O:list", kwlist, &arg);
        DANGER_END
        if (!err)
                return _int(-1);

        if (self->n) {
                blist_CLEAR(self);
                ext_dealloc((PyBListRoot *) self);
        }

        if (arg == NULL)
                return _int(0);

        ret = blist_init_from_seq(self, arg);
        
        decref_flush(); /* Needed due to blist_CLEAR() call */
        return _int(ret);
}

static PyObject *
py_blist_richcompare(PyObject *v, PyObject *w, int op)
{
        if (!PyUserBList_Check(v)) {
        not_implemented:
                Py_INCREF(Py_NotImplemented);
                return Py_NotImplemented;
        }

        invariants((PyBList *) v, VALID_USER);
        if (PyUserBList_Check(w))
                return _ob(blist_richcompare_blist((PyBList *)v,
                                                   (PyBList *)w, op));
#ifndef Py_BUILD_CORE
        if (PyList_Check(w))
                return _ob(blist_richcompare_list((PyBList*)v,
                                                  (PyListObject*)w, op));
#endif
        _void();
        goto not_implemented;
}

static int
py_blist_traverse(PyObject *oself, visitproc visit, void *arg)
{
        PyBList *self;
        int i;

        assert(PyBList_Check(oself));
        self = (PyBList *) oself;
        
        for (i = 0; i < self->num_children; i++) {
                if (self->children[i] != NULL)
                        Py_VISIT(self->children[i]);
        }
        return 0;
}

static int
py_blist_clear(PyObject *oself)
{
        PyBList *self;
        
        invariants(oself, VALID_USER|VALID_RW|VALID_DECREF);
        self = (PyBList *) oself;
        
        blist_forget_children(self);
        self->n = 0;
        self->leaf = 1;
        ext_dealloc((PyBListRoot *) self);

        decref_flush();        
        return _int(0);
}

static long
py_blist_nohash(PyObject *self)
{
        PyErr_SetString(PyExc_TypeError, "list objects are unhashable");
        return -1;
}

static void
py_blist_dealloc(PyObject *oself)
{
        int i;
        PyBList *self;

        assert(PyBList_Check(oself));
        self = (PyBList *) oself;
        
        PyObject_GC_UnTrack(self);

        Py_TRASHCAN_SAFE_BEGIN(self)

        for (i = 0; i < self->num_children; i++)
                Py_XDECREF(self->children[i]);

        self->num_children = 0;
        self->n = 0;
        self->leaf = 1;

        if (self->ob_type == &PyUserBList_Type)
                ext_dealloc((PyBListRoot *) self);

        if (num_free_lists < MAXFREELISTS
            && (self->ob_type == &PyBList_Type)) {
                free_lists[num_free_lists++] = self;
        } else if (num_free_ulists < MAXFREELISTS
                   && (self->ob_type == &PyUserBList_Type)) {
                free_ulists[num_free_ulists++] = self;
        } else {
                PyMem_Free(self->children);
                self->ob_type->tp_free((PyObject *)self);
        }

        Py_TRASHCAN_SAFE_END(self);
}

static int
py_blist_ass_item(PyObject *oself, Py_ssize_t i, PyObject *v)
{
        PyObject *old_value;
        PyBList *self;
        
        invariants(oself, VALID_USER|VALID_RW|VALID_DECREF);

        self = (PyBList *) oself;

        if (i >= self->n || i < 0) {
                set_index_error();
                return _int(-1);
        }

        if (v == NULL) {
                blist_delitem(self, i);
                ext_mark(self, 0, DIRTY);
                decref_flush();
                return _int(0);
        }
        
        old_value = blist_ass_item_return(self, i, v);
        Py_XDECREF(old_value);
        return _int(0);
}

static int
py_blist_ass_slice(PyObject *oself, Py_ssize_t ilow, Py_ssize_t ihigh,
                   PyObject *v)
{
        Py_ssize_t net;
        PyBList *other, *left, *right, *self;
        
        invariants(oself, VALID_RW|VALID_USER|VALID_DECREF);

        self = (PyBList *) oself;
        
        if (ilow < 0) ilow = 0;
        else if (ilow > self->n) ilow = self->n;
        if (ihigh < ilow) ihigh = ilow;
        else if (ihigh > self->n) ihigh = self->n;

        if (!v) {
                blist_delslice(self, ilow, ihigh);
                ext_mark(self, 0, DIRTY);
                decref_flush();
                return _int(0);
        }
        
        if (PyBList_Check(v) && (PyObject *) self != v) {
                other = (PyBList *) v;
                Py_INCREF(other);
        } else {
                other = blist_user_new();
                if (v) {
                        int err = blist_init_from_seq(other, v);
                        if (err < 0) {
                                decref_later((PyObject *) other);
                                decref_flush();
                                return _int(-1);
                        }
                }
        }

        net = other->n - (ihigh - ilow);

        /* Special case small lists */
        if (self->leaf && other->leaf && (self->n + net <= LIMIT))
        {
                int i;

                for (i = ilow; i < ihigh; i++)
                        decref_later(self->children[i]);

                if (net >= 0)
                        shift_right(self, ihigh, net);
                else
                        shift_left(self, ihigh, -net);
                self->num_children += net;
                copyref(self, ilow, other, 0, other->n);
                SAFE_DECREF(other);
                blist_adjust_n(self);
                decref_flush();
                return _int(0);
        }

        left = self;
        right = blist_user_copy(self);
        blist_delslice(left, ilow, left->n);
        blist_delslice(right, 0, ihigh);
        blist_extend_blist(left, other); /* XXX check return values */
        blist_extend_blist(left, right);

        ext_mark(self, 0, DIRTY);

        SAFE_DECREF(other);
        SAFE_DECREF(right);

        decref_flush();

        return _int(0);
}

static int
py_blist_ass_subscript(PyObject *oself, PyObject *item, PyObject *value)
{
        PyBList *self;
        
        invariants(oself, VALID_USER|VALID_RW|VALID_DECREF);

        self = (PyBList *) oself;

        if (PyIndex_Check(item)) {
                Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
                PyObject *old_value;
                if (i == -1 && PyErr_Occurred())
                        return _int(-1);
                if (i < 0)
                        i += self->n;

                if (i >= self->n || i < 0) {
                        set_index_error();
                        return _int(-1);
                }

                if (self->leaf) {
                        /* Speed up common cases */

                        old_value = self->children[i];
                        if (value == NULL) {
                                shift_left(self, i+1, 1);
                                self->num_children--;
                                self->n--;
                        } else {
                                self->children[i] = value;
                                Py_INCREF(value);
                        }
                        DANGER_BEGIN
                        Py_DECREF(old_value);
                        DANGER_END
                        return _int(0);
                }
                        
                if (value == NULL) {
                        blist_delitem(self, i);
                        ext_mark(self, 0, DIRTY);
                        decref_flush();
                        return _int(0);
                }

                Py_INCREF(value);
                old_value = blist_ass_item_return2((PyBListRoot*)self,i,value);
                DANGER_BEGIN
                Py_DECREF(old_value);
                DANGER_END
                return _int(0);
        } else if (PySlice_Check(item)) {
                Py_ssize_t start, stop, step, slicelength;

                ext_mark(self, 0, DIRTY);

                if (PySlice_GetIndicesEx((PySliceObject*)item, self->n,
                                         &start, &stop,&step,&slicelength)<0)
                        return _int(-1);

                /* treat L[slice(a,b)] = v _exactly_ like L[a:b] = v */
                if (step == 1 && ((PySliceObject*)item)->step == Py_None) 
                        return _redir(py_blist_ass_slice(oself,start,stop,value));

                if (value == NULL) {
                        /* Delete back-to-front */
                        Py_ssize_t i, cur;

                        if (slicelength <= 0)
                                return _int(0);

                        if (step > 0) {
                                stop = start - 1;
                                start = start + step*(slicelength-1);
                                step = -step;
                        }
                        
                        for (cur = start, i = 0; i < slicelength;
                             cur += step, i++) {
                                PyObject *ob = blist_delitem_return(self, cur);
                                decref_later(ob);
                        }

                        decref_flush();
                        ext_mark(self, 0, DIRTY);
                        
                        return _int(0);
                } else { /* assign slice */
                        PyObject *ins, *seq;
                        Py_ssize_t cur, i;

                        DANGER_BEGIN
                        seq = PySequence_Fast(value,
                                  "Must assign iterable to extended slice");
                        DANGER_END
                        if (!seq)
                                return _int(-1);

                        if (seq == (PyObject *) self) {
                                Py_DECREF(seq);
                                seq = (PyObject *) blist_user_copy(self);
                        }

                        if (PySequence_Fast_GET_SIZE(seq) != slicelength) {
                                PyErr_Format(PyExc_ValueError,
                                             "attempt to assign sequence of size %zd to extended slice of size %zd",
                                             PySequence_Fast_GET_SIZE(seq),
                                             slicelength);
                                Py_DECREF(seq);
                                return _int(-1);
                        }

                        if (!slicelength) {
                                Py_DECREF(seq);
                                return _int(0);
                        }

                        for (cur = start, i = 0; i < slicelength;
                             cur += step, i++) {
                                PyObject *ob;
                                ins = PySequence_Fast_GET_ITEM(seq, i);
                                ob = blist_ass_item_return(self, cur, ins);
                                decref_later(ob);
                        }

                        Py_DECREF(seq);

                        decref_flush();

                        return _int(0);
                }
        } else {
                PyErr_SetString(PyExc_TypeError,
                                "list indices must be integers");
                return _int(-1);
        }
}

static Py_ssize_t
py_blist_length(PyObject *ob)
{
        assert(PyUserBList_Check(ob));
        return ((PyBList *) ob)->n;
}

static PyObject *
py_blist_repeat(PyObject *oself, Py_ssize_t n)
{
        PyObject *ret;
        PyBList *self;
        
        invariants(oself, VALID_USER|VALID_DECREF);

        self = (PyBList *) oself;

        ret = blist_repeat(self, n);
        decref_flush();

        return _ob(ret);
}

static PyObject *
py_blist_inplace_repeat(PyObject *oself, Py_ssize_t n)
{
        PyBList *tmp, *self;
        
        invariants(oself, VALID_USER|VALID_RW|VALID_NEWREF|VALID_DECREF);

        self = (PyBList *) oself;
        
        tmp = (PyBList *) blist_repeat(self, n);
        if (tmp == NULL)
                return (PyObject *) _blist(NULL);
        blist_become_and_consume(self, tmp);
        Py_INCREF(self);
        SAFE_DECREF(tmp);

        decref_flush();

        ext_mark(self, 0, DIRTY);

        return (PyObject *) _blist(self);
}

static PyObject *
py_blist_extend(PyBList *self, PyObject *other)
{
        int err;

        invariants(self, VALID_USER|VALID_RW|VALID_DECREF);

        err = blist_extend(self, other);
        decref_flush();
        
        if (err < 0)
                return _ob(NULL);
        ext_mark(self, 0, DIRTY);
        Py_RETURN_NONE;
}

static PyObject *
py_blist_inplace_concat(PyObject *oself, PyObject *other)
{
        int err;
        PyBList *self;

        invariants(oself, VALID_RW|VALID_USER|VALID_NEWREF|VALID_DECREF);

        self = (PyBList *) oself;

        err = blist_extend(self, other);
        decref_flush();

        if (err < 0) 
                return _ob(NULL);
        
        ext_mark(self, 0, DIRTY);
        Py_INCREF(self);
        return _ob((PyObject *)self);
}

static int
py_blist_contains(PyObject *oself, PyObject *el)
{
        int c, ret = 0;
        PyObject *item;
        PyBList *self;

        invariants(oself, VALID_USER | VALID_DECREF);

        self = (PyBList *) oself;

        ITER(self, item, {
                DANGER_BEGIN
                c = PyObject_RichCompareBool(el, item, Py_EQ);
                DANGER_END
                if (c < 0) {
                        ret = -1;
                        break;
                }
                if (c > 0) {
                        ret = 1;
                        break;
                }
        });

        decref_flush();
        return _int(ret);
}

static PyObject *
py_blist_get_slice(PyObject *oself, Py_ssize_t ilow, Py_ssize_t ihigh)
{
        PyBList *rv, *self;
        
        invariants(oself, VALID_USER | VALID_DECREF);

        self = (PyBList *) oself;

        if (ilow < 0) ilow = 0;
        else if (ilow > self->n) ilow = self->n;
        if (ihigh < ilow) ihigh = ilow;
        else if (ihigh > self->n) ihigh = self->n;

        rv = blist_user_new();
        if (rv == NULL)
                return (PyObject *) _blist(NULL);

        if (ihigh <= ilow || ilow >= self->n)
                return (PyObject *) _blist(rv);

        if (self->leaf) {
                Py_ssize_t delta = ihigh - ilow;

                copyref(rv, 0, self, ilow, delta);
                rv->num_children = delta;
                rv->n = delta;
                return (PyObject *) _blist(rv);
        }

        blist_become(rv, self);
        blist_delslice(rv, ihigh, self->n);
        blist_delslice(rv, 0, ilow);

        ext_mark(rv, 0, DIRTY);
        decref_flush();
        
        return (PyObject *) _blist(rv);
}

/* This should only be called by _PyBList_GET_ITEM_FAST2() */
PyObject *_PyBList_GetItemFast3(PyBListRoot *root, Py_ssize_t i)
{
        PyObject *rv;
        int dirty_offset = -1;

        invariants(root, VALID_PARENT);
        assert(!root->leaf);
        assert(root->dirty_root != CLEAN);

        if (ext_is_dirty(root, i, &dirty_offset)){
                rv = ext_make_clean(root, i);
        } else {
                int ioffset = i / INDEX_FACTOR;
                Py_ssize_t offset = root->offset_list[ioffset];
                PyBList *p = root->index_list[ioffset];
                assert(i >= offset);
                assert(p);
                assert(p->leaf);
                if (i < offset + p->n) {
                        rv = p->children[i - offset];
                        if (dirty_offset >= 0)
                                ext_make_clean(root, dirty_offset);
                } else if (ext_is_dirty(root,i + INDEX_FACTOR,&dirty_offset)){
                        rv = ext_make_clean(root, i);
                } else {
                        ioffset++;
                        assert(ioffset < root->index_length);
                        offset = root->offset_list[ioffset];
                        p = root->index_list[ioffset];
                        rv = p->children[i - offset];
                        assert(p);
                        assert(p->leaf);
                        assert(i < offset + p->n);
                        if (dirty_offset >= 0)
                                ext_make_clean(root, dirty_offset);
                }
        }

        assert(rv == blist_get1((PyBList *)root, i));

        return _ob(rv);        
}

static PyObject *
py_blist_get_item(PyObject *oself, Py_ssize_t i)
{
        PyBList *self = (PyBList *) oself;
        PyObject *ret;

        invariants(self, VALID_USER);
        
        if (i < 0 || i >= self->n) {
                set_index_error();
                return _ob(NULL);
        }

        if (self->leaf)
                ret = self->children[i];
        else
                ret = _PyBList_GET_ITEM_FAST2((PyBListRoot*)self, i);
        Py_INCREF(ret);
        return _ob(ret);
}

static PyObject *
py_blist_concat(PyObject *oself, PyObject *oother)
{
        PyBList *other, *rv, *self;

        invariants((PyBList *) oself, VALID_USER|VALID_RW|VALID_DECREF);

        self = (PyBList *) oself;
        
        if (!PyBList_Check(oother)) {
                PyErr_Format(PyExc_TypeError,
                        "can only concatenate blist (not \"%.200s\") to blist",
                         oother->ob_type->tp_name);
                return _ob(NULL);
        }

        other = (PyBList *) oother;

        rv = blist_user_copy(self);
        blist_extend_blist(rv, other);
        ext_mark(rv, 0, DIRTY);

        decref_flush();
        return (PyObject *) _blist(rv);
}

/* User-visible repr() */
static PyObject *
py_blist_repr(PyObject *oself)
{
        /* Basic approach: Clone self in O(1) time, then walk through
         * the clone, changing each element to repr() of the element,
         * in O(n) time.  Finally, enclose it in square brackets and
         * call join.
         */

        Py_ssize_t i;
        PyBList *pieces = NULL, *self;
        PyObject *result = NULL;
        PyObject *s, *tmp;

        invariants(oself, VALID_USER);
        self = (PyBList *) oself;

        DANGER_BEGIN
        i = Py_ReprEnter((PyObject *) self);
        DANGER_END
        if (i) {
                return i > 0 ? _ob(PyString_FromString("[...]")) : _ob(NULL);
        }

        if (self->n == 0) {
#ifdef Py_BUILD_CORE
                result = PyString_FromString("[]");
#else
                result = PyString_FromString("blist([])");
#endif
                goto Done;
        }

        pieces = blist_user_copy(self);
        if (pieces == NULL)
                goto Done;

        if (blist_repr_r(pieces) < 0)
                goto Done;

#ifdef Py_BUILD_CORE
        s = PyString_FromString("[");
#else
        s = PyString_FromString("blist([");
#endif
        if (s == NULL)
                goto Done;
        tmp = blist_get1(pieces, 0);
        PyString_Concat(&s, tmp);
        DANGER_BEGIN
        py_blist_ass_item((PyObject *) pieces, 0, s);
        DANGER_END
        Py_DECREF(s);

#ifdef Py_BUILD_CORE
        s = PyString_FromString("]");
#else
        s = PyString_FromString("])");
#endif
        if (s == NULL)
                goto Done;
        tmp = blist_get1(pieces, pieces->n-1);
        Py_INCREF(tmp);
        PyString_ConcatAndDel(&tmp, s);
        DANGER_BEGIN
        py_blist_ass_item((PyObject *) pieces, pieces->n-1, tmp);
        DANGER_END
        Py_DECREF(tmp);

        s = PyString_FromString(", ");
        if (s == NULL)
                goto Done;
        result = _PyString_Join(s, (PyObject *) pieces);
        Py_DECREF(s);
        
 Done:
        DANGER_BEGIN
        /* Only deallocating strings, so this is safe */
        Py_XDECREF(pieces);
        DANGER_END

        DANGER_BEGIN
        Py_ReprLeave((PyObject *) self);
        DANGER_END
        return _ob(result);
}

/* Return a string that shows the internal structure of the BList */
static PyObject *
py_blist_debug(PyBList *self, PyObject *indent)
{
        PyObject *result, *s, *nl_indent, *comma, *indent2;

        invariants(self, VALID_USER);
        
        comma = PyString_FromString(", ");
        
        if (indent == NULL)
                indent = PyString_FromString("");
        else
                Py_INCREF(indent);

        indent2 = indent;
        Py_INCREF(indent);
        PyString_ConcatAndDel(&indent2, PyString_FromString("  "));

        if (!self->leaf) {
                int i;
                
                nl_indent = indent2;
                Py_INCREF(nl_indent);
                PyString_ConcatAndDel(&nl_indent, PyString_FromString("\n"));
        
                result = PyString_FromFormat("blist(leaf=%d, n=%d, r=%d, ",
                                             self->leaf, self->n,
                                             self->ob_refcnt);
                /* PyString_Concat(&result, nl_indent); */

                for (i = 0; i < self->num_children; i++) {
                        s = py_blist_debug((PyBList *)self->children[i], indent2);
                        PyString_Concat(&result, nl_indent);
                        PyString_ConcatAndDel(&result, s);
                }

                PyString_ConcatAndDel(&result, PyString_FromString(")"));
        } else {
                int i;

                result = PyString_FromFormat("blist(leaf=%d, n=%d, r=%d, ",
                                             self->leaf, self->n,
                                             self->ob_refcnt);
                for (i = 0; i < self->num_children; i++) {
                        s = PyObject_Str(self->children[i]);
                        PyString_ConcatAndDel(&result, s);
                        PyString_Concat(&result, comma);
                }
        }

        s = indent;
        Py_INCREF(s);
        PyString_ConcatAndDel(&s, result);
        result = s;

        Py_DECREF(comma);
        Py_DECREF(indent);
        check_invariants(self);
        return _ob(result);
}

static PyObject *
py_blist_sort(PyBList *self, PyObject *args, PyObject *kwds)
{
        static char *kwlist[] = {"cmp", "key", "reverse", 0};
        compare_t compare = {NULL, NULL};
        int reverse = 0;
        int ret;
        PyBListRoot saved;
        PyObject *result = Py_None;

        invariants(self, VALID_USER|VALID_RW | VALID_DECREF);
        
        if (args != NULL) {
                int err;
                DANGER_BEGIN
                err = PyArg_ParseTupleAndKeywords(args, kwds, "|OOi:sort",
                                                  kwlist, &compare.compare,
                                                  &compare.keyfunc,
                                                  &reverse);
                DANGER_END
                if (!err) 
                        return _ob(NULL);
        }
        
        if (is_default_cmp(compare.compare))
                compare.compare = NULL;
        if (compare.keyfunc == Py_None)
                compare.keyfunc = NULL;

        saved.children = self->children;
        saved.ob_type = &PyUserBList_Type; /* Make validations happy */
        saved.ob_refcnt = 1;               /* Make valgrind happy */
        saved.n = self->n;
        saved.num_children = self->num_children;
        saved.leaf = self->leaf;
        ext_init(&saved);
        
        self->children = PyMem_New(PyObject *, LIMIT);
        self->n = 0;
        self->num_children = 0;
        self->leaf = 1;

        /* Reverse sort stability achieved by initially reversing the list,
           applying a stable forward sort, then reversing the final result. */
        if (reverse)
                blist_reverse((PyBList*)&saved);
        
        if (compare.compare == NULL && compare.keyfunc == NULL)
                ret = sort((PyBList*)&saved, NULL);
        else
                ret = sort((PyBList*)&saved, &compare);

        if (reverse)
                blist_reverse((PyBList*)&saved);

        if (ret < 0)
                result = NULL;

        if (self->n && saved.n) {
                DANGER_BEGIN
                /* An error may also have been raised by a comparison
                 * function.  Since may decref that traceback, it can
                 * execute arbitrary python code */
                PyErr_SetString(PyExc_ValueError, "list modified during sort");
                DANGER_END
                result = NULL;
                blist_CLEAR(self);
        }

        PyMem_Free(self->children);
        assert(!self->n);
        self->n = saved.n;
        self->num_children = saved.num_children;
        self->leaf = saved.leaf;
        self->children = saved.children;
        
        Py_XINCREF(result);

        decref_flush();
        
        ext_mark(self, 0, DIRTY);
        return _ob(result);
}

static PyObject *
py_blist_reverse(PyBList *self)
{
        invariants(self, VALID_USER|VALID_RW);

        blist_reverse(self);
        ext_mark(self, 0, DIRTY);

        Py_RETURN_NONE;
}

static PyObject *
py_blist_count(PyBList *self, PyObject *v)
{
        Py_ssize_t count = 0;
        PyObject *item;
        int c;

        invariants(self, VALID_USER | VALID_DECREF);

        ITER(self, item, {
                DANGER_BEGIN
                c = PyObject_RichCompareBool(item, v, Py_EQ);
                DANGER_END
                if (c > 0)
                        count++;
                else if (c < 0) {
                        ITER_CLEANUP();
                        decref_flush();
                        return _ob(NULL);
                }
        })

        decref_flush();
        return _ob(PyInt_FromSsize_t(count));
}

static PyObject *
py_blist_index(PyBList *self, PyObject *args)
{
        Py_ssize_t i, start=0, stop=self->n;
        PyObject *v;
        int c, err;
        PyObject *item;
        
        invariants(self, VALID_USER|VALID_DECREF);

        DANGER_BEGIN
        err = PyArg_ParseTuple(args, "O|O&O&:index", &v,
                               _PyEval_SliceIndex, &start,
                               _PyEval_SliceIndex, &stop);
        DANGER_END
        if (!err)
                return _ob(NULL);
        if (start < 0) {
                start += self->n;
                if (start < 0)
                        start = 0;
        }
        if (stop < 0) {
                stop += self->n;
                if (stop < 0)
                        stop = 0;
        }

        i = start;
        ITER2(self, item, start, stop, {
                DANGER_BEGIN
                c = PyObject_RichCompareBool(item, v, Py_EQ);
                DANGER_END
                if (c > 0) {
                        ITER_CLEANUP();
                        decref_flush();
                        return _ob(PyInt_FromSsize_t(i));
                } else if (c < 0) {
                        ITER_CLEANUP();
                        decref_flush();
                        return _ob(NULL);
                }
                i++;
        })

        decref_flush();
        PyErr_SetString(PyExc_ValueError, "list.index(x): x not in list");
        return _ob(NULL);
}

static PyObject *
py_blist_remove(PyBList *self, PyObject *v)
{
        Py_ssize_t i;
        int c;
        PyObject *item;

        invariants(self, VALID_USER|VALID_RW|VALID_DECREF);

        i = 0;
        ITER(self, item, {
                DANGER_BEGIN
                c = PyObject_RichCompareBool(item, v, Py_EQ);
                DANGER_END
                if (c > 0) {
                        ITER_CLEANUP();
                        blist_delitem(self, i);
                        decref_flush();
                        ext_mark(self, 0, DIRTY);
                        Py_RETURN_NONE;
                } else if (c < 0) {
                        ITER_CLEANUP();
                        decref_flush();
                        return _ob(NULL);
                }
                i++;
        })

        decref_flush();
        PyErr_SetString(PyExc_ValueError, "list.remove(x): x not in list");
        return _ob(NULL);
}

static PyObject *
py_blist_pop(PyBList *self, PyObject *args)
{
        Py_ssize_t i = -1;
        PyObject *v;
        int err;

        invariants(self, VALID_USER|VALID_RW|VALID_DECREF);

        DANGER_BEGIN
        err = PyArg_ParseTuple(args, "|n:pop", &i);
        DANGER_END
        if (!err)
                return _ob(NULL);

        if (self->n == 0) {
                /* Special-case most common failure cause */
                PyErr_SetString(PyExc_IndexError, "pop from empty list");
                return _ob(NULL);
        }
        if (i < 0)
                i += self->n;
        if (i < 0 || i >= self->n) {
                PyErr_SetString(PyExc_IndexError, "pop index out of range");
                return _ob(NULL);
        }
        
        v = blist_delitem_return(self, i);
        ext_mark(self, 0, DIRTY);

        decref_flush(); /* Remove any deleted BList nodes */

        return _ob(v); /* the caller now owns the reference the list had */
}

static PyObject *
py_blist_insert(PyBList *self, PyObject *args)
{
        Py_ssize_t i;
        PyObject *v;
        PyBList *overflow;
        int err;
        
        invariants(self, VALID_USER|VALID_RW);

        DANGER_BEGIN
        err = PyArg_ParseTuple(args, "nO:insert", &i, &v);
        DANGER_END
        if (!err)
                return _ob(NULL);

        if (self->n == PY_SSIZE_T_MAX) {
                PyErr_SetString(PyExc_OverflowError,
                                "cannot add more objects to list");
                return _ob(NULL);
        }

        if (i < 0) {
                i += self->n;
                if (i < 0)
                        i = 0;
        } else if (i > self->n)
                i = self->n;
        
        /* Speed up the common case */
        if (self->leaf && self->num_children < LIMIT) {
                Py_INCREF(v);

                shift_right(self, i, 1);
                self->num_children++;
                self->n++;
                self->children[i] = v;
                Py_RETURN_NONE;
        }
        
        overflow = ins1(self, i, v);
        if (overflow)
                blist_overflow_root(self, overflow);
        ext_mark(self, 0, DIRTY);
        Py_RETURN_NONE;
}

static PyObject *
py_blist_append(PyBList *self, PyObject *v)
{
        int err;

        invariants(self, VALID_USER|VALID_RW);

        err = blist_append(self, v);
        ext_mark(self, 0, DIRTY);

        if (err < 0)
                return _ob(NULL);

        Py_RETURN_NONE;
}

static PyObject *
py_blist_subscript(PyObject *oself, PyObject *item)
{
        PyBList *self;
        
        invariants(oself, VALID_USER);

        self = (PyBList *) oself;

        if (PyIndex_Check(item)) {
                Py_ssize_t i;
                PyObject *ret;
                
                i = PyNumber_AsSsize_t(item, PyExc_IndexError);
                if (i == -1 && PyErr_Occurred())
                        return _ob(NULL);
                if (i < 0)
                        i += self->n;

                if (i < 0 || i >= self->n) {
                        set_index_error();
                        return _ob(NULL);
                }

                if (self->leaf)
                        ret = self->children[i];
                else
                        ret = _PyBList_GET_ITEM_FAST2((PyBListRoot*)self, i);
                Py_INCREF(ret);

                return _ob(ret);
        } else if (PySlice_Check(item)) {
                Py_ssize_t start, stop, step, slicelength, cur, i;
                PyBList* result;
                PyObject* it;

                if (PySlice_GetIndicesEx((PySliceObject*)item, self->n,
                                         &start, &stop,&step,&slicelength)<0) {
                        return _ob(NULL);
                }

                if (step == 1)
                        return _redir((PyObject *)
                                      py_blist_get_slice((PyObject *) self, start, stop));

                result = blist_user_new();
                
                if (slicelength <= 0)
                        return _ob((PyObject *) result);

                /* This could be made slightly faster by using forests */
                /* Also, by special-casing small trees */
                for (cur = start, i = 0; i < slicelength; cur += step, i++) {
                        int err;
                        
                        it = blist_get1(self, cur);
                        err = blist_append(result, it);
                        if (err < 0) {
                                Py_DECREF(result);
                                return _ob(NULL);
                        }
                }

                ext_mark(result, 0, DIRTY);
                return _ob((PyObject *) result);
        } else {
                PyErr_SetString(PyExc_TypeError,
                                "list indices must be integers");
                return _ob(NULL);
        }
}

PyDoc_STRVAR(getitem_doc,
             "x.__getitem__(y) <==> x[y]");
PyDoc_STRVAR(reversed_doc,
             "L.__reversed__() -- return a reverse iterator over the list");
PyDoc_STRVAR(append_doc,
"L.append(object) -- append object to end");
PyDoc_STRVAR(extend_doc,
"L.extend(iterable) -- extend list by appending elements from the iterable");
PyDoc_STRVAR(insert_doc,
"L.insert(index, object) -- insert object before index");
PyDoc_STRVAR(pop_doc,
"L.pop([index]) -> item -- remove and return item at index (default last)");
PyDoc_STRVAR(remove_doc,
"L.remove(value) -- remove first occurrence of value");
PyDoc_STRVAR(index_doc,
"L.index(value, [start, [stop]]) -> integer -- return first index of value");
PyDoc_STRVAR(count_doc,
"L.count(value) -> integer -- return number of occurrences of value");
PyDoc_STRVAR(reverse_doc,
"L.reverse() -- reverse *IN PLACE*");
PyDoc_STRVAR(sort_doc,
"L.sort(cmp=None, key=None, reverse=False) -- stable sort *IN PLACE*;\n\
cmp(x, y) -> -1, 0, 1");

static PyMethodDef blist_methods[] = {
        {"__getitem__", (PyCFunction)py_blist_subscript, METH_O|METH_COEXIST, getitem_doc},
        {"__reversed__",(PyCFunction)py_blist_reversed, METH_NOARGS, reversed_doc},
        {"append",      (PyCFunction)py_blist_append,  METH_O, append_doc},
        {"insert",      (PyCFunction)py_blist_insert,  METH_VARARGS, insert_doc},
        {"extend",      (PyCFunction)py_blist_extend,  METH_O, extend_doc},
        {"pop",         (PyCFunction)py_blist_pop,     METH_VARARGS, pop_doc},
        {"remove",      (PyCFunction)py_blist_remove,  METH_O, remove_doc},
        {"index",       (PyCFunction)py_blist_index,   METH_VARARGS, index_doc},
        {"count",       (PyCFunction)py_blist_count,   METH_O, count_doc},
        {"reverse",     (PyCFunction)py_blist_reverse, METH_NOARGS, reverse_doc},
        {"sort",        (PyCFunction)py_blist_sort,    METH_VARARGS | METH_KEYWORDS, sort_doc},
        {"debug",       (PyCFunction)py_blist_debug,   METH_NOARGS, NULL}, 
        {NULL,          NULL}           /* sentinel */
};

static PySequenceMethods blist_as_sequence = {
        py_blist_length,                   /* sq_length */
        py_blist_concat,                /* sq_concat */
        py_blist_repeat,              /* sq_repeat */
        py_blist_get_item,            /* sq_item */
        py_blist_get_slice,      /* sq_slice */
        py_blist_ass_item,         /* sq_ass_item */
        py_blist_ass_slice,   /* sq_ass_slice */
        py_blist_contains,              /* sq_contains */
        py_blist_inplace_concat,        /* sq_inplace_concat */
        py_blist_inplace_repeat,      /* sq_inplace_repeat */
};

PyDoc_STRVAR(blist_doc,
"blist() -> new list\n"
"blist(sequence) -> new list initialized from sequence's items");

static PyMappingMethods blist_as_mapping = {
        py_blist_length,
        py_blist_subscript,
        py_blist_ass_subscript
};

PyTypeObject PyBList_Type = {
        PyObject_HEAD_INIT(NULL)
        0,
        "blist",
        sizeof(PyBList),
        0,
        py_blist_dealloc,                       /* tp_dealloc */
        0,                                      /* tp_print */
        0,                                      /* tp_getattr */
        0,                                      /* tp_setattr */
        0,                                      /* tp_compare */
        0,                                      /* tp_repr */
        0,                                      /* tp_as_number */
        0,                                      /* tp_as_sequence */
        0,                                      /* tp_as_mapping */
        py_blist_nohash,                        /* tp_hash */
        0,                                      /* tp_call */
        0,                                      /* tp_str */
        PyObject_GenericGetAttr,                /* tp_getattro */
        0,                                      /* tp_setattro */
        0,                                      /* tp_as_buffer */
        Py_TPFLAGS_HAVE_GC,                     /* tp_flags */
        blist_doc,                              /* tp_doc */
        py_blist_traverse,                      /* tp_traverse */
        0,                                      /* tp_clear */
        0,                                      /* tp_richcompare */
        0,                                      /* tp_weaklistoffset */
        0,                                      /* tp_iter */
        0,                                      /* tp_iternext */
        0,                                      /* tp_methods */
        0,                                      /* tp_members */
        0,                                      /* tp_getset */
        0,                                      /* tp_base */
        0,                                      /* tp_dict */
        0,                                      /* tp_descr_get */
        0,                                      /* tp_descr_set */
        0,                                      /* tp_dictoffset */
        0,                                      /* tp_init */
        0,                                      /* tp_alloc */
        0,                                      /* tp_new */
        PyObject_GC_Del,                        /* tp_free */
};        

PyTypeObject PyUserBList_Type = {
        PyObject_HEAD_INIT(NULL)
        0,
        "list",
        sizeof(PyBListRoot),
        0,
        py_blist_dealloc,                       /* tp_dealloc */
        0,                                      /* tp_print */
        0,                                      /* tp_getattr */
        0,                                      /* tp_setattr */
        0,                                      /* tp_compare */
        py_blist_repr,                          /* tp_repr */
        0,                                      /* tp_as_number */
        &blist_as_sequence,                     /* tp_as_sequence */
        &blist_as_mapping,                      /* tp_as_mapping */
        py_blist_nohash,                        /* tp_hash */
        0,                                      /* tp_call */
        0,                                      /* tp_str */
        PyObject_GenericGetAttr,                /* tp_getattro */
        0,                                      /* tp_setattro */
        0,                                      /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_BASETYPE,            /* tp_flags */
        blist_doc,                              /* tp_doc */
        py_blist_traverse,                      /* tp_traverse */
        py_blist_clear,                         /* tp_clear */
        py_blist_richcompare,                   /* tp_richcompare */
        0,                                      /* tp_weaklistoffset */
        py_blist_iter,                          /* tp_iter */
        0,                                      /* tp_iternext */
        blist_methods,                          /* tp_methods */
        0,                                      /* tp_members */
        0,                                      /* tp_getset */
        0,                                      /* tp_base */
        0,                                      /* tp_dict */
        0,                                      /* tp_descr_get */
        0,                                      /* tp_descr_set */
        0,                                      /* tp_dictoffset */
        py_blist_init,                          /* tp_init */
        PyType_GenericAlloc,                    /* tp_alloc */
        py_blist_user_tp_new,                   /* tp_new */
        PyObject_GC_Del,                        /* tp_free */
};        

static PyMethodDef module_methods[] = { { NULL } };

PyMODINIT_FUNC
initblist(void)
{
        PyObject *m;
        PyObject *limit = PyInt_FromLong(LIMIT);

        decref_init();
        
        PyBList_Type.ob_type = &PyType_Type;
        PyUserBList_Type.ob_type = &PyType_Type;
        PyBListIter_Type.ob_type = &PyType_Type;
        
        Py_INCREF(&PyBList_Type);
        Py_INCREF(&PyUserBList_Type);
        Py_INCREF(&PyBListIter_Type);

        if (PyType_Ready(&PyUserBList_Type) < 0) return;
        if (PyType_Ready(&PyBList_Type) < 0) return;
        if (PyType_Ready(&PyBListIter_Type) < 0) return;

        m = Py_InitModule3("blist", module_methods, "blist");

        PyModule_AddObject(m, "blist", (PyObject *) &PyUserBList_Type);
        PyModule_AddObject(m, "_limit", limit);
}

/************************************************************************
 * Mirror List API
 */

#ifdef Py_BUILD_CORE
PyObject *PyList_New(Py_ssize_t size)
{
        PyBList *self = blist_user_new();
        PyObject *tmp;

        if (self == NULL)
                return NULL;

        if (size <= LIMIT) {
                self->n = size;
                self->num_children = size;
                memset(self->children, 0, sizeof(PyObject *) * size);
                check_invariants(self);
                return (PyObject *) self;
        }
        
        self->n = 1;
        self->num_children = 1;
        self->children[0] = NULL;

        tmp = blist_repeat(self, size);
        check_invariants((PyBList *) tmp);
        SAFE_DECREF(self);
        _decref_flush();
        check_invariants((PyBList *) tmp);
        assert(((PyBList *)tmp)->n == size);
        ext_dealloc((PyBListRoot *) tmp);
        return tmp;
}

Py_ssize_t PyList_Size(PyObject *ob)
{
        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return -1;
        }

        return py_blist_length(ob);
}

PyObject *PyList_GetItem(PyObject *ob, Py_ssize_t i)
{
        PyBList *self = (PyBList *) ob;
        PyObject *ret;

        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return NULL;
        }

        assert(i >= 0 && i < self->n); /* XXX Remove */
        
        if (i < 0 || i >= self->n) {
                set_index_error();
                return NULL;
        }
        
        ret = blist_get1((PyBList *) ob, i);
        assert(ret != NULL);
        return ret;
}

int PyList_SetItem(PyObject *ob, Py_ssize_t i, PyObject *item)
{
        int ret;

        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                Py_XDECREF(item);
                return -1;
        }

        assert(i >= 0 && i < ((PyBList *)ob)->n); /* XXX Remove */
        ret = py_blist_ass_item(ob, i, item);
        assert(item->ob_refcnt > 1);
        Py_XDECREF(item);
        return ret;
}

int PyList_Insert(PyObject *ob, Py_ssize_t i, PyObject *v)
{
        PyBList *overflow;
        PyBList *self = (PyBList *) ob;
        
        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return -1;
        }

        invariants(self, VALID_USER|VALID_RW);
        
        if (self->n == PY_SSIZE_T_MAX) {
                PyErr_SetString(PyExc_OverflowError,
                                "cannot add more objects to list");
                return _int(0);
        }

        if (i < 0) {
                i += self->n;
                if (i < 0)
                        i = 0;
        } else if (i > self->n)
                i = self->n;
        
        if (self->leaf && self->num_children < LIMIT) {
                Py_INCREF(v);

                shift_right(self, i, 1);
                self->num_children++;
                self->n++;
                self->children[i] = v;
                return _int(0);
        }
        
        overflow = ins1(self, i, v);
        if (overflow)
                blist_overflow_root(self, overflow);
        ext_mark(self, 0, DIRTY);

        return _int(0);
}

int PyList_Append(PyObject *ob, PyObject *item)
{
        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return -1;
        }

        return blist_append((PyBList *) ob, item);
}

PyObject *PyList_GetSlice(PyObject *ob, Py_ssize_t i, Py_ssize_t j)
{
        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return NULL;
        }

        return py_blist_get_slice(ob, i, j);
}

int PyList_SetSlice(PyObject *ob, Py_ssize_t i, Py_ssize_t j, PyObject *lst)
{
        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return -1;
        }

        return py_blist_ass_slice(ob, i, j, lst);
}

int PyList_Sort(PyObject *ob)
{
        PyObject *ret;

        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return -1;
        }

        ret = py_blist_sort((PyBList *) ob, NULL, NULL);
        if (ret == NULL)
                return -1;

        Py_DECREF(ret);
        return 0;
}

int PyList_Reverse(PyObject *ob)
{
        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return -1;
        }

        invariants((PyBList *) ob, VALID_USER|VALID_RW);

        blist_reverse((PyBList *) ob);
        ext_mark((PyBList *)ob, 0, DIRTY);
        
        return _int(0);
}

PyObject *PyList_AsTuple(PyObject *ob)
{
        PyBList *self = (PyBList *) ob;
        PyObject *item;
        PyTupleObject *tuple;
        int i;

        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return NULL;
        }

        invariants(self, VALID_USER | VALID_DECREF);

        DANGER_BEGIN
        tuple = (PyTupleObject *) PyTuple_New(self->n);
        DANGER_END
        if (tuple == NULL)
                return _ob(NULL);

        i = 0;
        ITER(self, item, {
                tuple->ob_item[i++] = item;
                Py_INCREF(item);
        })

        assert(i == self->n);

        decref_flush();
                
        return _ob((PyObject *) tuple);
        
}

PyObject *
_PyList_Extend(PyBListRoot *ob, PyObject *b)
{
        if (ob == NULL || !PyList_Check(ob)) {
                PyErr_BadInternalCall();
                return NULL;
        }

        return py_blist_extend((PyBList *) ob, b);
}
#endif
