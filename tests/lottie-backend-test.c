#include <assert.h>
#include <string.h>

#include "lottie-backend.h"

static void test_parse_defaults_to_browser(void)
{
	assert(lt_backend_parse(NULL) == LT_BACKEND_BROWSER);
	assert(lt_backend_parse("") == LT_BACKEND_BROWSER);
	assert(lt_backend_parse("browser") == LT_BACKEND_BROWSER);
	assert(lt_backend_parse("unexpected") == LT_BACKEND_BROWSER);
}

static void test_parse_thorvg(void)
{
	assert(lt_backend_parse("thorvg") == LT_BACKEND_THORVG);
	assert(strcmp(lt_backend_name(LT_BACKEND_THORVG), "thorvg") == 0);
	assert(strcmp(lt_backend_name(LT_BACKEND_BROWSER), "browser") == 0);
}

static void test_backend_resolution_matches_build_availability(void)
{
	enum lt_backend_type effective = lt_backend_resolve(LT_BACKEND_BROWSER);

	assert(effective == LT_BACKEND_BROWSER);
	assert(!lt_backend_is_fallback(LT_BACKEND_BROWSER, effective));

	effective = lt_backend_resolve(LT_BACKEND_THORVG);
	if (lt_backend_is_available(LT_BACKEND_THORVG)) {
		assert(effective == LT_BACKEND_THORVG);
		assert(!lt_backend_is_fallback(LT_BACKEND_THORVG, effective));
	} else {
		assert(effective == LT_BACKEND_BROWSER);
		assert(lt_backend_is_fallback(LT_BACKEND_THORVG, effective));
	}
}

static void test_transition_start_recreate_policy(void)
{
	assert(lt_backend_recreate_on_transition_start(LT_BACKEND_BROWSER));
	assert(!lt_backend_recreate_on_transition_start(LT_BACKEND_THORVG));
}

int main(void)
{
	test_parse_defaults_to_browser();
	test_parse_thorvg();
	test_backend_resolution_matches_build_availability();
	test_transition_start_recreate_policy();
	return 0;
}
