// A well-behaved C++ module: registers com.example.fake.ok in load and leaves a marker file in the working directory
// for each entry point that ran, so tests can tell what ran.
#include <fstream>
#include <string>
#include <string_view>

#include "knurbs/module.hpp"
#include "knurbs/registry.hpp"

namespace {

	class fake_ok_type final : public knurbs::type {
		public:
		std::string_view id() const noexcept override
		{
			return "com.example.fake.ok";
		}
		knurbs::type_kind kind() const noexcept override
		{
			return knurbs::type_kind::source;
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
	m->registry().add(knurbs::make<fake_ok_type>());
}

extern "C" KNURBS_MODULE_EXPORT void knurbs_module_post_load(knurbs::module* m)
{
	write_marker(m, "post_load");
}

extern "C" KNURBS_MODULE_EXPORT void knurbs_module_unload(knurbs::module* m)
{
	write_marker(m, "unload");
}
