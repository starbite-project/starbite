/* Forced into shim/starlte/catalog.c by tools/loader-test/run.sh: the catalog buffer is the host
 * test's own array instead of the loader's staging area at 0x8c000000. */
#pragma once
extern unsigned char host_cat_buf[];
