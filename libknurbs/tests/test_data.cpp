#include <limits>

#include "test.hpp"

#include "knurbs/data.hpp"
#include "knurbs/error.hpp"

using knurbs::data;
using knurbs::data_array;
using knurbs::ref;
namespace error = knurbs::error;

// `levels` nested objects: {"a":{"a":{}}} for 3.
static std::string nested_objects(int levels)
{
	std::string json;
	for (int i = 0; i < levels - 1; ++i) {
		json += "{\"a\":";
	}
	json += "{}";
	for (int i = 0; i < levels - 1; ++i) {
		json += "}";
	}
	return json;
}

// `levels` nested containers, alternating object and array: {"a":[{"a":[]}]} for 4.
static std::string nested_mixed(int levels)
{
	std::string open;
	std::string close;
	for (int i = 0; i < levels; ++i) {
		bool last = (i == levels - 1);
		if (i % 2 == 0) {
			open += last ? "{" : "{\"a\":";
			close.insert(0, "}");
		} else {
			open += "[";
			close.insert(0, "]");
		}
	}
	return open + close;
}

static void test_default_vs_user(void **)
{
	ref<data> d = data::create();
	d->set_default_int("a", 1);
	assert_int_equal(d->get_int("a"), 1);
	assert_true(d->has("a"));
	assert_false(d->has_user_value("a"));

	d->set_int("a", 2);
	assert_int_equal(d->get_int("a"), 2);
	assert_true(d->has_user_value("a"));

	d->erase("a");
	assert_int_equal(d->get_int("a"), 1);
	assert_false(d->has_user_value("a"));
	assert_true(d->has("a"));

	d->set_string("b", "x");
	d->set_default_bool("c", true);
	std::vector<std::string> keys = d->keys();
	assert_int_equal(keys.size(), 3);
	assert_string_equal(keys[0].c_str(), "a");
	assert_string_equal(keys[1].c_str(), "b");
	assert_string_equal(keys[2].c_str(), "c");

	// Defaults are never serialized.
	assert_string_equal(d->to_json().c_str(), "{\"b\":\"x\"}");
}

static void test_missing_key_throws(void **)
{
	ref<data> d = data::create();
	KNURBS_ASSERT_THROWS(d->get_int("nope"), error::not_found);
	KNURBS_ASSERT_THROWS(d->get_object("nope"), error::not_found);
	KNURBS_ASSERT_THROWS(d->get_array("nope"), error::not_found);
	assert_false(d->has("nope"));
	d->erase("nope"); // no-op
}

static void test_wrong_type_throws(void **)
{
	ref<data> d = data::create();
	d->set_string("a", "x");
	KNURBS_ASSERT_THROWS(d->get_int("a"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(d->get_double("a"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(d->get_object("a"), error::invalid_argument);

	d->set_int("b", 3);
	assert_true(d->get_double("b") == 3.0);
	KNURBS_ASSERT_THROWS(d->get_bool("b"), error::invalid_argument);

	d->set_double("c", 1.5);
	KNURBS_ASSERT_THROWS(d->get_int("c"), error::invalid_argument);
}

static void test_json_roundtrip(void **)
{
	ref<data> d = data::create();
	d->set_string("zeta", "last \"quoted\"");
	d->set_int("alpha", -42);
	d->set_bool("mid", true);
	d->set_double("ratio", 0.25);
	d->set_double("whole", 3.0);

	ref<data> child = data::create();
	child->set_int("y", 2);
	child->set_int("x", 1);
	d->set_object("nested", child);

	ref<data_array> list = data_array::create();
	ref<data> first = data::create();
	first->set_string("name", "one");
	ref<data> second = data::create();
	second->set_object("inner", data::create());
	list->push_back(first);
	list->push_back(second);
	d->set_array("list", list);

	std::string json = d->to_json();
	assert_string_equal(json.c_str(), "{\"alpha\":-42,\"list\":[{\"name\":\"one\"},{\"inner\":{}}],\"mid\":true,"
					  "\"nested\":{\"x\":1,\"y\":2},\"ratio\":0.25,\"whole\":3.0,"
					  "\"zeta\":\"last \\\"quoted\\\"\"}");

	ref<data> back = data::from_json(json);
	assert_string_equal(back->to_json().c_str(), json.c_str());
	assert_int_equal(back->get_int("alpha"), -42);
	assert_true(back->get_double("whole") == 3.0);
	KNURBS_ASSERT_THROWS(back->get_int("whole"), error::invalid_argument);
	assert_string_equal(back->get_array("list")->at(0)->get_string("name").c_str(), "one");
	assert_int_equal(back->get_object("nested")->get_int("y"), 2);

	ref<data> pretty = data::from_json(d->to_json(true));
	assert_string_equal(pretty->to_json().c_str(), json.c_str());
	assert_true(d->to_json(true).find('\n') != std::string::npos);
}

static void test_from_json_rejects_garbage(void **)
{
	KNURBS_ASSERT_THROWS(data::from_json("{"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json("[]"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json("{\"a\":}"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json(""), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json("{} x"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json("1"), error::invalid_argument);
}

// Values the tree cannot hold: null, arrays of anything but objects, integers above int64, invalid keys.
static void test_from_json_rejects_unrepresentable(void **)
{
	KNURBS_ASSERT_THROWS(data::from_json("{\"a\":null}"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json("{\"a\":[1]}"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json("{\"a\":[[]]}"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json("{\"a\":18446744073709551615}"), error::invalid_argument);
	// Beyond std::uint64_t the parser sees a floating-point number; it is still an integer out of range.
	KNURBS_ASSERT_THROWS(data::from_json("{\"a\":18446744073709551616}"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json("{\"a\":-9223372036854775809}"), error::invalid_argument);
	assert_true(data::from_json("{\"a\":1e30}")->get_double("a") == 1e30);
	assert_true(data::from_json("{\"a\":2.5E1}")->get_double("a") == 25.0);
	KNURBS_ASSERT_THROWS(data::from_json("{\"Bad_Key\":1}"), error::invalid_argument);

	ref<data> d = data::from_json("{\"a\":9223372036854775807,\"b\":[]}");
	assert_true(d->get_int("a") == INT64_MAX);
	assert_int_equal(d->get_array("b")->size(), 0);
}

static void test_from_json_depth_limit(void **)
{
	std::string deepest = nested_objects(64);
	ref<data> d = data::from_json(deepest);
	assert_string_equal(d->to_json().c_str(), deepest.c_str());
	assert_string_equal(d->clone()->to_json().c_str(), deepest.c_str());
	KNURBS_ASSERT_THROWS(data::from_json(nested_objects(65)), error::invalid_argument);

	std::string mixed = nested_mixed(64);
	assert_string_equal(data::from_json(mixed)->to_json().c_str(), mixed.c_str());
	KNURBS_ASSERT_THROWS(data::from_json(nested_mixed(65)), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data::from_json(nested_mixed(66)), error::invalid_argument);
}

static void test_invalid_key_throws(void **)
{
	ref<data> d = data::create();
	KNURBS_ASSERT_THROWS(d->set_int("Bad_Key", 1), error::invalid_argument);
	KNURBS_ASSERT_THROWS(d->set_default_int("Bad_Key", 1), error::invalid_argument);
	KNURBS_ASSERT_THROWS(d->set_string("", "x"), error::invalid_argument);
	assert_false(d->has("Bad_Key"));
}

static void test_null_and_non_finite_rejected(void **)
{
	ref<data> d = data::create();
	KNURBS_ASSERT_THROWS(d->set_object("a", ref<data>()), error::invalid_argument);
	KNURBS_ASSERT_THROWS(d->set_array("a", ref<data_array>()), error::invalid_argument);
	KNURBS_ASSERT_THROWS(data_array::create()->push_back(ref<data>()), error::invalid_argument);
	KNURBS_ASSERT_THROWS(d->set_double("a", std::numeric_limits<double>::infinity()), error::invalid_argument);
	KNURBS_ASSERT_THROWS(d->set_default_double("a", std::numeric_limits<double>::quiet_NaN()),
			     error::invalid_argument);
	assert_false(d->has("a"));
}

static void test_clone_is_deep(void **)
{
	ref<data> d = data::create();
	ref<data> child = data::create();
	child->set_int("x", 1);
	child->set_default_int("y", 5);
	d->set_object("child", child);
	ref<data_array> list = data_array::create();
	list->push_back(data::create());
	d->set_array("list", list);
	d->set_default_string("s", "default");

	ref<data> copy = d->clone();
	copy->get_object("child")->set_int("x", 2);
	copy->get_array("list")->at(0)->set_int("z", 3);
	copy->get_array("list")->push_back(data::create());

	assert_int_equal(child->get_int("x"), 1);
	assert_int_equal(copy->get_object("child")->get_int("x"), 2);
	assert_int_equal(copy->get_object("child")->get_int("y"), 5);
	assert_false(list->at(0)->has("z"));
	assert_int_equal(list->size(), 1);
	assert_string_equal(copy->get_string("s").c_str(), "default");
	assert_false(copy->has_user_value("s"));
}

static void test_array(void **)
{
	ref<data_array> list = data_array::create();
	assert_int_equal(list->size(), 0);
	KNURBS_ASSERT_THROWS(list->at(0), error::out_of_range);
	KNURBS_ASSERT_THROWS(list->erase(0), error::out_of_range);

	ref<data> a = data::create();
	ref<data> b = data::create();
	list->push_back(a);
	list->push_back(b);
	assert_ptr_equal(list->at(1).get(), b.get());
	list->erase(0);
	assert_int_equal(list->size(), 1);
	assert_ptr_equal(list->at(0).get(), b.get());
}

static void test_cycle_rejected(void **)
{
	ref<data> a = data::create();
	KNURBS_ASSERT_THROWS(a->set_object("self", a), error::invalid_argument);
	KNURBS_ASSERT_THROWS(a->set_default_object("self", a), error::invalid_argument);

	ref<data> b = data::create();
	a->set_object("b", b);
	KNURBS_ASSERT_THROWS(b->set_object("a", a), error::invalid_argument);

	ref<data_array> list = data_array::create();
	ref<data> c = data::create();
	list->push_back(c);
	b->set_default_array("list", list);
	KNURBS_ASSERT_THROWS(list->push_back(a), error::invalid_argument);
	KNURBS_ASSERT_THROWS(c->set_array("list", list), error::invalid_argument);
	KNURBS_ASSERT_THROWS(c->set_object("b", b), error::invalid_argument);
	assert_int_equal(list->size(), 1);
	assert_false(c->has("list"));

	// Sharing a subtree without a cycle is allowed.
	ref<data> other = data::create();
	other->set_object("b", b);
	a->set_object("again", b);
	assert_string_equal(a->to_json().c_str(), "{\"again\":{},\"b\":{}}");
	assert_string_equal(a->clone()->to_json().c_str(), "{\"again\":{},\"b\":{}}");
}

static void test_concurrent_set_get(void **)
{
	constexpr int ops = 10000;
	constexpr std::size_t key_count = 8;
	ref<data> d = data::create();
	std::vector<std::string> keys;
	for (std::size_t k = 0; k < key_count; ++k) {
		keys.push_back(std::format("k{}", k));
		d->set_default_int(keys.back(), -1);
	}

	std::atomic<int> failures{0};
	std::vector<std::thread> threads;
	for (int w = 0; w < 4; ++w) {
		threads.emplace_back([&, w] {
			for (int i = 0; i < ops; ++i) {
				const std::string &key = keys[static_cast<std::size_t>(i + w) % key_count];
				if (i % 3 == 2) {
					d->erase(key);
				} else if (i % 100 == 0) {
					ref<data> child = data::create();
					child->set_int("v", i);
					d->set_object(std::format("child{}", w), child);
				} else {
					d->set_int(key, i);
				}
			}
		});
	}
	for (int r = 0; r < 4; ++r) {
		threads.emplace_back([&, r] {
			for (int i = 0; i < ops; ++i) {
				const std::string &key = keys[static_cast<std::size_t>(i + r) % key_count];
				if (d->get_int(key) < -1) {
					failures.fetch_add(1);
				}
				if (i % 100 == 0) {
					if (d->to_json().empty() || d->clone()->keys().size() < key_count) {
						failures.fetch_add(1);
					}
				}
			}
		});
	}
	for (std::thread &t : threads) {
		t.join();
	}
	assert_int_equal(failures.load(), 0);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_default_vs_user),
		cmocka_unit_test(test_missing_key_throws),
		cmocka_unit_test(test_wrong_type_throws),
		cmocka_unit_test(test_json_roundtrip),
		cmocka_unit_test(test_from_json_rejects_garbage),
		cmocka_unit_test(test_from_json_rejects_unrepresentable),
		cmocka_unit_test(test_from_json_depth_limit),
		cmocka_unit_test(test_invalid_key_throws),
		cmocka_unit_test(test_null_and_non_finite_rejected),
		cmocka_unit_test(test_clone_is_deep),
		cmocka_unit_test(test_array),
		cmocka_unit_test(test_cycle_rejected),
		cmocka_unit_test(test_concurrent_set_get),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
