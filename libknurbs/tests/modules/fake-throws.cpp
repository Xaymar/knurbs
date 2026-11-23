// A C++ module whose load registers com.example.fake.throws and then throws. Its post_load and unload leave markers
// that must never appear, because the module failed.
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "knurbs/module.hpp"
#include "knurbs/registry.hpp"

namespace {

	class fake_throws_type final : public knurbs::type {
		public:
		std::string_view id() const noexcept override
		{
			return "com.example.fake.throws";
		}
		knurbs::type_kind kind() const noexcept override
		{
			return knurbs::type_kind::filter;
		}
	};

	void write_marker(knurbs::module* m, std::string_view entry)
	{
		std::ofstream(std::string(m->name()) + "." + std::string(entry)) << "ran\n";
	}

} // namespace

KNURBS_MODULE_CPP()

extern "C" KNURBS_MODULE_EXPORT void knurbs_module_load(knurbs::module* m)
{
	write_marker(m, "load");
	m->registry().add(knurbs::make<fake_throws_type>());
	m->registry().add_alias("fake_throws_old", "com.example.fake.throws");
	throw std::runtime_error("load failed on purpose");
}

extern "C" KNURBS_MODULE_EXPORT void knurbs_module_post_load(knurbs::module* m)
{
	write_marker(m, "post_load");
}

extern "C" KNURBS_MODULE_EXPORT void knurbs_module_unload(knurbs::module* m)
{
	write_marker(m, "unload");
}
