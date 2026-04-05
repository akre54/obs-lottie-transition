#ifndef LOTTIE_BACKEND_H
#define LOTTIE_BACKEND_H

#include <stdbool.h>

enum lt_backend_type {
	LT_BACKEND_BROWSER = 0,
	LT_BACKEND_THORVG = 1,
};

enum lt_backend_type lt_backend_parse(const char *name);
const char *lt_backend_name(enum lt_backend_type backend);
bool lt_backend_is_available(enum lt_backend_type backend);
enum lt_backend_type lt_backend_resolve(enum lt_backend_type requested);
bool lt_backend_is_fallback(enum lt_backend_type requested,
			    enum lt_backend_type effective);
bool lt_backend_recreate_on_transition_start(enum lt_backend_type backend);

#endif
