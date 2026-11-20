/* PS5Term - ps5compat: FreeBSD 14 <malloc.h> (the SDK's FreeBSD 11 one is an #error stub).
 * GPL-3.0-or-later. */
#pragma once
#include <stdlib.h>
#if __has_include(<malloc_np.h>)
#include <malloc_np.h>
#endif
