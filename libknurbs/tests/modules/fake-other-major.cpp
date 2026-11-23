// A C++ module built against another major version of the API (99). The loader must reject it before any of its
// entry points run; its load would leave a marker file.
#include <cstdint>
#include <fstream>
#include <string>

#include "knurbs/module.hpp"

extern "C" KNURBS_MODULE_EXPORT const struct knurbs_module_header *knurbs_module_header(void)
{
	static constexpr struct knurbs_module_header header = {
		sizeof(struct knurbs_module_header),
		KNURBS_MODULE_API_CPP,
		99,
		0,
	};
	return &header;
}

extern "C" KNURBS_MODULE_EXPORT void knurbs_module_load(knurbs::module *m)
{
	std::ofstream(std::string(m->name()) + ".load") << "ran\n";
}
