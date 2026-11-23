#include "test.hpp"

#include "knurbs/data.hpp"
#include "knurbs/error.hpp"
#include "knurbs/properties.hpp"

using knurbs::data;
using knurbs::properties;
using knurbs::property;
using knurbs::property_kind;
using knurbs::ref;
namespace error = knurbs::error;

static void test_add_and_find(void**)
{
	ref<properties> props = properties::create();
	ref<properties> inner = properties::create();
	inner->add_int("gain", "Gain", -10, 30, 0.5);
	props->add_group("audio", "Audio", inner);

	ref<property> found = props->find("gain");
	assert_true(static_cast<bool>(found));
	assert_true(found->kind() == property_kind::integer);
	assert_true(found->id() == "gain");
	assert_true(found->label() == "Gain");
	assert_true(found->min() == -10);
	assert_true(found->max() == 30);
	assert_true(found->step() == 0.5);
	assert_true(found->enabled());
	assert_true(found->visible());
	found->set_enabled(false);
	found->set_visible(false);
	assert_false(found->enabled());
	assert_false(found->visible());

	assert_false(static_cast<bool>(props->find("missing")));
	assert_int_equal(props->list().size(), 1);
	assert_true(props->find("audio")->group().get() == inner.get());
}

static void test_insertion_order_and_kinds(void**)
{
	ref<properties> props = properties::create();
	props->add_bool("b", "B");
	props->add_float("f", "F", 0, 1, 0.1);
	props->add_text("t", "T");
	props->add_path("p", "P");
	ref<property> list = props->add_list("l", "L");
	props->add_color("c", "C");
	props->add_notice("n", "Careful");
	list->add_item("One", "1");
	list->add_item("Two", "2");
	assert_int_equal(list->item_count(), 2);

	const property_kind expected[] = {property_kind::boolean, property_kind::floating, property_kind::text, property_kind::path, property_kind::list, property_kind::color, property_kind::notice};
	auto                all        = props->list();
	assert_int_equal(all.size(), 7);
	for (size_t i = 0; i < all.size(); ++i) {
		assert_true(all[i]->kind() == expected[i]);
	}
	assert_true(all[6]->label() == "Careful");
}

static void test_duplicate_id_throws(void**)
{
	ref<properties> props = properties::create();
	props->add_bool("a", "A");
	KNURBS_ASSERT_THROWS(props->add_text("a", "Again"), error::already_exists);
}

static void test_invalid_arguments_throw(void**)
{
	ref<properties> props = properties::create();
	KNURBS_ASSERT_THROWS(props->add_int("a", "A", 5, 1, 1), error::invalid_argument);
	KNURBS_ASSERT_THROWS(props->add_int("a", "A", 0, 1, 0), error::invalid_argument);
	KNURBS_ASSERT_THROWS(props->add_float("a", "A", 0, 1, -1), error::invalid_argument);
	KNURBS_ASSERT_THROWS(props->add_group("g", "G", {}), error::invalid_argument);
	KNURBS_ASSERT_THROWS(props->add_group("g", "G", props), error::invalid_argument);
	KNURBS_ASSERT_THROWS(props->add_bool("Bad Id", "B"), error::invalid_argument);

	// A group may not lead back to its parent.
	ref<properties> child = properties::create();
	props->add_group("child", "Child", child);
	KNURBS_ASSERT_THROWS(child->add_group("parent", "Parent", props), error::invalid_argument);
}

static void test_wrong_kind_throws(void**)
{
	ref<properties> props = properties::create();
	ref<property>   text  = props->add_text("t", "T");
	KNURBS_ASSERT_THROWS(text->min(), error::invalid_argument);
	KNURBS_ASSERT_THROWS(text->max(), error::invalid_argument);
	KNURBS_ASSERT_THROWS(text->step(), error::invalid_argument);
	KNURBS_ASSERT_THROWS(text->add_item("a", "b"), error::invalid_argument);
	KNURBS_ASSERT_THROWS(text->item_count(), error::invalid_argument);
	KNURBS_ASSERT_THROWS(text->group(), error::invalid_argument);
	KNURBS_ASSERT_THROWS(text->click(), error::invalid_argument);
}

static void test_modified_callback(void**)
{
	ref<properties> props = properties::create();
	props->add_bool("a", "A");
	props->set_modified("a", [](properties&, property& p, data& settings) {
		settings.set_bool("b", p.id() == "a");
		return true;
	});
	ref<data> settings = data::create();
	assert_true(props->notify_modified("a", *settings));
	assert_true(settings->get_bool("b"));
	assert_false(props->notify_modified("other", *settings));
}

static void test_callback_releases_properties(void**)
{
	ref<properties> props = properties::create();
	props->add_bool("a", "A");
	ref<data> settings = data::create();
	// The callback drops the caller's last external ref; the call keeps the object alive until it returns.
	auto* holder = &props;
	props->set_modified("a", [holder](properties& self, property&, data& s) {
		holder->reset();
		// Still usable: the object is kept alive for the duration of the callback.
		bool found = static_cast<bool>(self.find("a"));
		self.add_bool("later", "Later");
		s.set_bool("found", found);
		return found;
	});
	properties* raw = props.get();
	assert_true(raw->notify_modified("a", *settings));
	assert_true(settings->get_bool("found"));
}

static void test_button(void**)
{
	ref<properties> props  = properties::create();
	int             clicks = 0;
	ref<property>   button = props->add_button("go", "Go", [&clicks](properties& self, property& p) {
		++clicks;
		return self.find("go").get() == &p;
	});
	assert_true(button->click());
	assert_int_equal(clicks, 1);

	props.reset();
	assert_false(button->click());
	assert_int_equal(clicks, 1);
}

namespace {
	// Calls back into the properties from its destructor, which deadlocks if that runs under the properties' lock.
	struct find_on_destroy {
		properties* props;
		int*        destroyed;
		find_on_destroy(properties* p, int* d) : props(p), destroyed(d) {}
		find_on_destroy(find_on_destroy&& other) noexcept : props(other.props), destroyed(other.destroyed)
		{
			other.props = nullptr;
		}
		~find_on_destroy()
		{
			if (props) {
				props->find("gain");
				++*destroyed;
			}
		}
		bool operator()(properties&, property&, data&) const
		{
			return true;
		}
	};
} // namespace

static void test_replaced_callback_destroyed_outside_lock(void**)
{
	ref<properties> props = properties::create();
	props->add_int("gain", "Gain", 0, 1, 0.1);
	int destroyed = 0;

	props->set_modified("gain", find_on_destroy(props.get(), &destroyed));
	assert_int_equal(destroyed, 0);

	// Replacing destroys the old callback
	props->set_modified("gain", find_on_destroy(props.get(), &destroyed));
	assert_int_equal(destroyed, 1);

	// Clearing destroys it too
	props->set_modified("gain", nullptr);
	assert_int_equal(destroyed, 2);
	ref<data> settings = data::create();
	assert_false(props->notify_modified("gain", *settings));
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_add_and_find), cmocka_unit_test(test_insertion_order_and_kinds), cmocka_unit_test(test_duplicate_id_throws), cmocka_unit_test(test_invalid_arguments_throw), cmocka_unit_test(test_wrong_kind_throws), cmocka_unit_test(test_modified_callback), cmocka_unit_test(test_callback_releases_properties), cmocka_unit_test(test_button), cmocka_unit_test(test_replaced_callback_destroyed_outside_lock),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
