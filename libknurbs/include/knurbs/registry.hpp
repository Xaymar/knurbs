#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "knurbs/data.hpp"
#include "knurbs/export.hpp"
#include "knurbs/object.hpp"
#include "knurbs/properties.hpp"
#include "knurbs/ref.hpp"

namespace knurbs {

	namespace detail {
		class registry_impl;
	}

	// Values are only ever appended.
	enum class type_kind : std::uint8_t {
		source,
		filter,
		transition,
		encoder,
		output,
		service,
	};

	// A registrable kind of source, filter, transition, encoder, output or service, implemented by a plugin. This is a
	// plugin interface: virtual functions are only ever appended. An implementation must be thread-safe, and its id()
	// and kind() must never change once it is registered: the registry reads both once, in add().
	//
	// A type's destructor can run during the core's shutdown, after the registry started refusing calls, so it must not
	// call the registry (that throws error::core_gone, which terminates in a destructor). A type that holds a ref<core>
	// keeps the core alive and so keeps the registry from ever being cleared by the last release; hold a weak<core>.
	class KNURBS_API type : public object {
		public:
		// A valid type ID (naming::is_valid_type_id).
		virtual std::string_view id() const noexcept   = 0;
		virtual type_kind        kind() const noexcept = 0;
		virtual std::uint32_t    version() const noexcept
		{
			return 1;
		}
		// Describes the settings of an object of this type. The default returns an empty properties. (The return type
		// is spelled knurbs::properties because this member function hides the class name.)
		virtual ref<knurbs::properties> properties(const data& settings) const;
		// Fills in the default values of the settings.
		virtual void defaults(data& /*settings*/) const {}
	};

	// The types registered with a core, and the aliases that let old IDs (such as OBS's "dshow_input") find them.
	// Owned by core (core::registry()); it cannot be created or copied. Thread-safe: one reader-writer lock covers
	// everything, and no plugin code (type virtuals and destructors included) runs while it is held.
	//
	// Every member throws error::core_gone once the core's shutdown has begun. Shutdown itself clears the registry
	// (see core), after the modules are unloaded: that drops only the registry's own references, and a type stays alive
	// for as long as someone else holds it. An add or add_alias with an empty `owner_module` made from inside a
	// module's entry point is recorded as that module's (see module::registry).
	class KNURBS_API registry {
		// Only created by core.
		registry() noexcept = default;
		friend class detail::registry_impl;

		public:
		registry(const registry&)            = delete;
		registry& operator=(const registry&) = delete;

		// Registers `t`, owned by the module `owner_module`. Empty means the module whose entry point runs on this
		// thread, if any, else none. Throws error::invalid_argument for an empty ref or an ID that is not a valid
		// type ID, and error::already_exists when the ID is already registered or is the old ID of an alias.
		void add(ref<type> t, std::string_view owner_module = {});
		// Makes `old_id` find the type `new_id`. `new_id` must be a valid type ID but need not be registered yet;
		// `old_id` is exempt from the naming rules but must not be empty. Throws error::invalid_argument for a bad
		// `new_id`, an empty `old_id` and for an alias that would form a cycle (a chain from `new_id` that reaches
		// `old_id`), and error::already_exists when `old_id` is a registered type or already an alias. An empty
		// `owner_module` means the same as for add().
		void add_alias(std::string_view old_id, std::string_view new_id, std::string_view owner_module = {});
		// The type with this ID, following alias chains to their end; empty if there is none.
		ref<type> find(std::string_view id) const;
		// The types of this kind, sorted by ID, so the order is the same on every machine.
		std::vector<ref<type>> list(type_kind kind) const;
		// Drops the types and aliases that `owner_module` added. An empty name drops those added without an owner.
		void remove_module(std::string_view owner_module);
	};

} // namespace knurbs
