/* Copyright 2001, 2002 by Hans Reiser, licensing governed by reiser4/README */

/* Scalable on-disk integers */

#if !defined( __FS_REISER4_DSCALE_H__ )
#define __FS_REISER4_DSCALE_H__

#include "dformat.h"

extern int dscale_read (char *address, __u64 *value);
extern int dscale_write(char *address, __u64 value);
extern int dscale_bytes(__u64 value);
extern int dscale_fit  (__u64 value, __u64 other);

/* __FS_REISER4_DSCALE_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
