/*
 * Copyright 2001, 2002 by Hans Reiser, licensing governed by reiser4/README
 */

/*
 * Key manipulations.
 */

#include "debug.h"
#include "key.h"
#include "super.h"
#include "reiser4.h"

#include <linux/types.h>	/* for __u??  */

/**
 * Minimal possible key: all components are zero. It is presumed that this is
 * independent of key scheme.
 */
static const reiser4_key MINIMAL_KEY = {
	.el = {{0ull}, {0ull}, {0ull}}
};

/**
 * Maximal possible key: all components are ~0. It is presumed that this is
 * independent of key scheme.
 */
static const reiser4_key MAXIMAL_KEY = {
	.el = {{~0ull}, {~0ull}, {~0ull}}
};

/** Initialise key. */
void
key_init(reiser4_key * key /* key to init */ )
{
	assert("nikita-1169", key != NULL);
	xmemset(key, 0, sizeof *key);
}

/** minimal possible key in the tree. Return pointer to the static storage. */
const reiser4_key *
min_key(void)
{
	return &MINIMAL_KEY;
}

/** maximum possible key in the tree. Return pointer to the static storage. */
const reiser4_key *
max_key(void)
{
	return &MAXIMAL_KEY;
}

#if REISER4_DEBUG_OUTPUT
/** debugging aid: print symbolic name of key type */
static const char *
type_name(unsigned int key_type /* key type */ )
{
	switch (key_type) {
	case KEY_FILE_NAME_MINOR:
		return "file name";
	case KEY_SD_MINOR:
		return "stat data";
	case KEY_ATTR_NAME_MINOR:
		return "attr name";
	case KEY_ATTR_BODY_MINOR:
		return "attr body";
	case KEY_BODY_MINOR:
		return "file body";
	default:
		return "unknown";
	}
}

/** debugging aid: print human readable information about key */
void
print_key(const char *prefix /* prefix to print */ ,
	  const reiser4_key * key /* key to print */ )
{
	/* turn bold on */
	/* printf ("\033[1m"); */
	if (key == NULL)
		info("%s: null key\n", prefix);
	else {
		info("%s: (%Lx:%x:%Lx:%Lx:%Lx)[%s]\n", prefix,
		     get_key_locality(key), get_key_type(key),
		     get_key_band(key), get_key_objectid(key), get_key_offset(key), type_name(get_key_type(key)));
	}
	/* turn bold off */
	/* printf ("\033[m\017"); */
}

#endif

int
sprintf_key(char *buffer /* buffer to print key into */ ,
	    const reiser4_key * key /* key to print */ )
{
	return sprintf(buffer, "(%Lx:%x:%Lx:%Lx:%Lx)",
		       get_key_locality(key), get_key_type(key),
		       get_key_band(key), get_key_objectid(key), get_key_offset(key));
}

/* 
 * Make Linus happy.
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 120
 * End:
 */
