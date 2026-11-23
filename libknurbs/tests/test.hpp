#pragma once

// Standard headers that tests use come first: cmocka defines a fail() macro that breaks <ios> and friends if
// they are included after it.
#include <atomic>
#include <barrier>
#include <format>
#include <ios>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <csetjmp>
#include <cstdarg>
#include <cstddef>
#include <cstdint>

extern "C" {
#include <cmocka.h>
}

// Fails the test unless `expr` throws an exception of type `Type`.
#define KNURBS_ASSERT_THROWS(expr, Type)                                                        \
	do {                                                                                    \
		bool knurbs_threw_expected = false;                                             \
		try {                                                                           \
			(void)(expr);                                                           \
		} catch (const Type &) {                                                        \
			knurbs_threw_expected = true;                                           \
		} catch (...) {                                                                 \
		}                                                                               \
		if (!knurbs_threw_expected)                                                     \
			fail_msg("%s did not throw %s", #expr, #Type);                          \
	} while (0)
