/* Copyright 2002 by Hans Reiser, licensing governed by reiser4/README */

/* Wrapper functions/macros for spinlocks. */

#ifndef __SPIN_MACROS_H__
#define __SPIN_MACROS_H__

#include <asm/spinlock.h>

#include "debug.h"

/* not yet implemented */
#define check_is_write_locked(s)     (1)
#define check_is_read_locked(s)      (1)
#define check_is_not_read_locked(s)  (1)
#define check_is_not_write_locked(s) (1)

#if REISER4_USER_LEVEL_SIMULATION
#    define check_spin_is_locked(s)     spin_is_locked(s)
#    define check_spin_is_not_locked(s) spin_is_not_locked(s)
#elif defined(CONFIG_DEBUG_SPINLOCK) && defined(CONFIG_SMP) && 0
#    define check_spin_is_not_locked(s) ((s)->owner != get_current())
#    define spin_is_not_locked(s)       ((s)->owner == NULL)
#    define check_spin_is_locked(s)     ((s)->owner == get_current())
#else
#    define check_spin_is_not_locked(s) (1)
#    define spin_is_not_locked(s)       (1)
#    if defined(CONFIG_SMP)
#        define check_spin_is_locked(s)     spin_is_locked(s)
#    else
#        define check_spin_is_locked(s)     (1)
#    endif
#endif

#define __ODC ON_DEBUG_CONTEXT

/* Define several inline functions for each type of spinlock. */
#define SPIN_LOCK_FUNCTIONS(NAME,TYPE,FIELD)				\
									\
static inline void spin_ ## NAME ## _inc(void)				\
{									\
	__ODC(++ lock_counters()->spin_locked_ ## NAME);		\
	__ODC(++ lock_counters()->spin_locked);				\
}									\
									\
static inline void spin_ ## NAME ## _dec(void)				\
{									\
	__ODC(--lock_counters()->spin_locked_ ## NAME);			\
	__ODC(--lock_counters()->spin_locked);				\
}									\
									\
static inline int  spin_ ## NAME ## _is_locked (const TYPE *x)		\
{									\
	return check_spin_is_locked (& x->FIELD);			\
}									\
									\
static inline int  spin_ ## NAME ## _is_not_locked (TYPE *x)		\
{									\
	return check_spin_is_not_locked (& x->FIELD);			\
}									\
									\
static inline void spin_lock_ ## NAME ## _no_ord (TYPE *x)		\
{									\
	assert( "nikita-2703", spin_ ## NAME ## _is_not_locked(x));	\
	spin_lock( &x -> FIELD );					\
	spin_ ## NAME ## _inc();					\
}									\
									\
static inline void spin_lock_ ## NAME (TYPE *x)				\
{									\
	__ODC( assert( "nikita-1383",					\
				  spin_ordering_pred_ ## NAME( x ) ) );	\
	spin_lock_ ## NAME ## _no_ord( x );				\
}									\
									\
static inline int  spin_trylock_ ## NAME (TYPE *x)			\
{									\
	if (spin_trylock (& x->FIELD)) {				\
		spin_ ## NAME ## _inc();				\
		return 1;						\
	}								\
	return 0;							\
}									\
									\
static inline void spin_unlock_ ## NAME (TYPE *x)			\
{									\
	__ODC( assert( "nikita-1375",					\
		lock_counters() -> spin_locked_ ## NAME > 0 ) );	\
	__ODC( assert( "nikita-1376",					\
		lock_counters() -> spin_locked > 0 ) );			\
	spin_ ## NAME ## _dec();					\
	assert( "nikita-2703", spin_ ## NAME ## _is_locked( x ) );	\
	spin_unlock (& x->FIELD);					\
}									\
									\
typedef struct { int foo; } NAME ## _spin_dummy

#define UNDER_SPIN(obj_type, obj, exp)		\
({						\
	typeof (obj) __obj;			\
	typeof (exp) __result;			\
						\
	__obj = (obj);				\
	assert("nikita-2492", __obj != NULL);	\
	spin_lock_ ## obj_type (__obj);		\
	__result = exp;				\
	spin_unlock_ ## obj_type (__obj);	\
	__result;				\
})
										\
#define UNDER_SPIN_VOID(obj_type, obj, exp)	\
({						\
	typeof (obj) __obj;			\
						\
	__obj = (obj);				\
	assert("nikita-2492", __obj != NULL);	\
	spin_lock_ ## obj_type (__obj);		\
	exp;					\
	spin_unlock_ ## obj_type (__obj);	\
})

/* Define several inline functions for each type of rwlock. */
#define RW_LOCK_FUNCTIONS(NAME,TYPE,FIELD)					\
										\
static inline int  rw_ ## NAME ## _is_read_locked (const TYPE *x)		\
{										\
	return check_is_read_locked (& x->FIELD);				\
}										\
										\
static inline int  rw_ ## NAME ## _is_write_locked (const TYPE *x)		\
{										\
	return check_is_write_locked (& x->FIELD);				\
}										\
										\
static inline int  rw_ ## NAME ## _is_not_read_locked (TYPE *x)			\
{										\
	return check_is_not_read_locked (& x->FIELD);				\
}										\
										\
static inline int  rw_ ## NAME ## _is_not_write_locked (TYPE *x)		\
{										\
	return check_is_not_write_locked (& x->FIELD);				\
}										\
										\
static inline int  rw_ ## NAME ## _is_locked (const TYPE *x)			\
{										\
	return check_is_read_locked (& x->FIELD) &&				\
	       check_is_write_locked (& x->FIELD);				\
}										\
										\
static inline int  rw_ ## NAME ## _is_not_locked (const TYPE *x)		\
{										\
	return check_is_not_read_locked (& x->FIELD) &&				\
	       check_is_not_write_locked (& x->FIELD);				\
}										\
										\
static inline void read_ ## NAME ## _inc(void)					\
{										\
	__ODC(++ lock_counters()->read_locked_ ## NAME);			\
	__ODC(++ lock_counters()->rw_locked_ ## NAME);				\
	__ODC(++ lock_counters()->spin_locked);					\
}										\
										\
static inline void read_ ## NAME ## _dec(void)					\
{										\
	__ODC(-- lock_counters()->read_locked_ ## NAME);			\
	__ODC(-- lock_counters()->rw_locked_ ## NAME);				\
	__ODC(-- lock_counters()->spin_locked);					\
}										\
										\
static inline void write_ ## NAME ## _inc(void)					\
{										\
	__ODC(++ lock_counters()->write_locked_ ## NAME);			\
	__ODC(++ lock_counters()->rw_locked_ ## NAME);				\
	__ODC(++ lock_counters()->spin_locked);					\
}										\
										\
static inline void write_ ## NAME ## _dec(void)					\
{										\
	__ODC(-- lock_counters()->write_locked_ ## NAME);			\
	__ODC(-- lock_counters()->rw_locked_ ## NAME);				\
	__ODC(-- lock_counters()->spin_locked);					\
}										\
										\
										\
static inline void read_lock_ ## NAME ## _no_ord (TYPE *x)			\
{										\
	assert("nikita-2976", rw_ ## NAME ## _is_not_read_locked(x));		\
	read_lock(&x->FIELD);							\
	read_ ## NAME ## _inc();						\
}										\
										\
static inline void write_lock_ ## NAME ## _no_ord (TYPE *x)			\
{										\
	assert("nikita-2977", rw_ ## NAME ## _is_not_write_locked(x));		\
	write_lock(&x->FIELD);							\
	write_ ## NAME ## _inc();						\
}										\
										\
static inline void read_lock_ ## NAME (TYPE *x)					\
{										\
	__ODC(assert("nikita-2975", rw_ordering_pred_ ## NAME(x)));		\
	read_lock_ ## NAME ## _no_ord(x);					\
}										\
										\
static inline void write_lock_ ## NAME (TYPE *x)				\
{										\
	__ODC(assert("nikita-2978", rw_ordering_pred_ ## NAME(x)));		\
	write_lock_ ## NAME ## _no_ord(x);					\
}										\
										\
static inline void read_unlock_ ## NAME (TYPE *x)				\
{										\
	__ODC(assert("nikita-2979",						\
				lock_counters()->read_locked_ ## NAME > 0));	\
	__ODC(assert("nikita-2980",						\
				lock_counters()->rw_locked_ ## NAME > 0));	\
	__ODC(assert("nikita-2980",						\
				lock_counters()->spin_locked > 0));		\
	read_ ## NAME ## _dec();						\
	assert("nikita-2703", rw_ ## NAME ## _is_read_locked(x));		\
	read_unlock (& x->FIELD);						\
}										\
										\
static inline void write_unlock_ ## NAME (TYPE *x)				\
{										\
	__ODC(assert("nikita-2979",						\
				lock_counters()->write_locked_ ## NAME > 0));	\
	__ODC(assert("nikita-2980",						\
				lock_counters()->rw_locked_ ## NAME > 0));	\
	__ODC(assert("nikita-2980",						\
				lock_counters()->spin_locked > 0));		\
	write_ ## NAME ## _dec();						\
	assert("nikita-2703", rw_ ## NAME ## _is_write_locked(x));		\
	read_unlock (& x->FIELD);						\
}										\
										\
										\
static inline int  write_trylock_ ## NAME (TYPE *x)				\
{										\
	if (write_trylock (& x->FIELD)) {					\
		write_ ## NAME ## _inc();					\
		return 1;							\
	}									\
	return 0;								\
}										\
										\
										\
typedef struct { int foo; } NAME ## _rw_dummy

#define UNDER_RW(obj_type, obj, rw, exp)	\
({						\
	typeof (obj) __obj;			\
	typeof (exp) __result;			\
						\
	__obj = (obj);				\
	assert("nikita-2981", __obj != NULL);	\
	rw ## _lock_ ## obj_type (__obj);	\
	__result = exp;				\
	rw ## _unlock_ ## obj_type (__obj);	\
	__result;				\
})
										\
#define UNDER_RW_VOID(obj_type, obj, rw, exp)	\
({						\
	typeof (obj) __obj;			\
						\
	__obj = (obj);				\
	assert("nikita-2982", __obj != NULL);	\
	rw ## _lock_ ## obj_type (__obj);	\
	exp;					\
	rw ## _unlock_ ## obj_type (__obj);	\
})

/* __SPIN_MACROS_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
