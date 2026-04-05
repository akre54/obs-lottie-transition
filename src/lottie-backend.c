#include "lottie-backend.h"

#include <string.h>

#ifndef LT_ENABLE_THORVG
#define LT_ENABLE_THORVG 0
#endif

/*
 * The runtime switch remains valid even if the native library is not linked in.
 */
#ifndef LT_THORVG_RUNTIME_READY
#define LT_THORVG_RUNTIME_READY 0
#endif

enum lt_backend_type lt_backend_parse(const char *name)
{
	if (!name || !*name)
		return LT_BACKEND_BROWSER;

	if (strcmp(name, "thorvg") == 0)
		return LT_BACKEND_THORVG;

	return LT_BACKEND_BROWSER;
}

const char *lt_backend_name(enum lt_backend_type backend)
{
	switch (backend) {
	case LT_BACKEND_THORVG:
		return "thorvg";
	case LT_BACKEND_BROWSER:
	default:
		return "browser";
	}
}

bool lt_backend_is_available(enum lt_backend_type backend)
{
	switch (backend) {
	case LT_BACKEND_THORVG:
		return LT_ENABLE_THORVG != 0 && LT_THORVG_RUNTIME_READY != 0;
	case LT_BACKEND_BROWSER:
	default:
		return true;
	}
}

enum lt_backend_type lt_backend_resolve(enum lt_backend_type requested)
{
	if (lt_backend_is_available(requested))
		return requested;

	return LT_BACKEND_BROWSER;
}

bool lt_backend_is_fallback(enum lt_backend_type requested,
			    enum lt_backend_type effective)
{
	return requested != effective;
}

bool lt_backend_recreate_on_transition_start(enum lt_backend_type backend)
{
	return backend == LT_BACKEND_BROWSER;
}
