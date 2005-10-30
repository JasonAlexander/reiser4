/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* reiser4 compression mode plugin (used by cryptcompress object plugin) */

#include "../../inode.h"
#include "../plugin.h"

/* plugin->should_deflate() */
static int should_deflate_test(cloff_t index)
{
	return !test_bit(0, &index);
}

/* plugin->discard_hook() */
static int discard_nocond(struct inode *inode, cloff_t index)
{
	int result;

	result = force_plugin(inode,
			      PSET_COMPRESSION,
			      compression_plugin_to_plugin
			      (dual_compression_plugin
			       (inode_compression_plugin(inode))));
	if (result)
		return result;
	__mark_inode_dirty(inode, I_DIRTY_PAGES);
	return 0;
}

static int discard_first(struct inode *inode, cloff_t index)
{
	assert("edward-1308", inode != NULL);

	return (index ? 0 : discard_nocond(inode, index));
}

/* compression mode_plugins */
compression_mode_plugin compression_mode_plugins[LAST_COMPRESSION_MODE_ID] = {
	[SMART_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = SMART_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "if-0-compressible",
			.desc = "If-first-cluster-compressible heuristic",
			.linkage = {NULL, NULL}
		},
		.should_deflate = NULL,
		.accept_hook = NULL,
		.discard_hook = discard_first
	},
	[LAZY_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = LAZY_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "if-all-compressible",
			.desc = "If-all-compressible heuristic",
			.linkage = {NULL, NULL}
		},
		.should_deflate = NULL,
		.accept_hook = NULL,
		.discard_hook = discard_nocond
	},
	[FORCE_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = FORCE_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "force",
			.desc = "Compress everything",
			.linkage = {NULL, NULL}
		},
		.should_deflate = NULL,
		.accept_hook = NULL,
		.discard_hook = NULL
	},
	[TEST_COMPRESSION_MODE_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
			.id = TEST_COMPRESSION_MODE_ID,
			.pops = NULL,
			.label = "test",	/* This mode is only for benchmarks */
			.desc = "Don't compress odd clusters",
			.linkage = {NULL, NULL}
		},
		.should_deflate = should_deflate_test,
		.accept_hook = NULL,
		.discard_hook = NULL
	}
};

/*
  Local variables:
  c-indentation-style: "K&R"
  mode-name: "LC"
  c-basic-offset: 8
  tab-width: 8
  fill-column: 120
  scroll-step: 1
  End:
*/
