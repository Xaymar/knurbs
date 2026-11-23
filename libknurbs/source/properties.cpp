#include "knurbs/properties.hpp"

#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "knurbs/error.hpp"
#include "knurbs/naming.hpp"

namespace knurbs {

	namespace detail {

		using click_callback    = knurbs::move_only_function<bool(properties&, property&)>;
		using modified_callback = knurbs::move_only_function<bool(properties&, property&, data&)>;

		// Locking: each operation holds at most one object's lock at a time, and callbacks run with no lock held.
		// The constant parts of a property (id, kind, label, range, group, owner) are written before it is published.
		class property_impl final : public property {
			public:
			std::string                     id;
			property_kind                   kind = property_kind::boolean;
			std::string                     label;
			double                          min  = 0;
			double                          max  = 0;
			double                          step = 0;
			ref<properties>                 group;
			weak<properties>                owner;
			std::shared_ptr<click_callback> on_click;

			mutable std::mutex                               mutex;
			bool                                             enabled = true;
			bool                                             visible = true;
			std::vector<std::pair<std::string, std::string>> items;

			property_impl() = default;
			~property_impl() override;
		};

		class properties_impl final : public properties {
			public:
			mutable std::mutex                                                     mutex;
			std::vector<ref<property>>                                             entries;
			std::map<std::string, std::shared_ptr<modified_callback>, std::less<>> modified;

			properties_impl() = default;
			~properties_impl() override;
		};

		property_impl::~property_impl()     = default;
		properties_impl::~properties_impl() = default;

	} // namespace detail

	namespace {

		using detail::properties_impl;
		using detail::property_impl;

		constexpr int max_depth = 64;

		const property_impl& impl(const property& p) noexcept
		{
			return static_cast<const property_impl&>(p);
		}

		property_impl& impl(property& p) noexcept
		{
			return static_cast<property_impl&>(p);
		}

		const properties_impl& impl(const properties& p) noexcept
		{
			return static_cast<const properties_impl&>(p);
		}

		properties_impl& impl(properties& p) noexcept
		{
			return static_cast<properties_impl&>(p);
		}

		const property_impl& numeric(const property& p, std::string_view what)
		{
			const property_impl& i = impl(p);
			if (i.kind != property_kind::integer && i.kind != property_kind::floating) {
				throw error::invalid_argument(std::format("property {}: {} needs an integer or floating property", i.id, what));
			}
			return i;
		}

		// Creates and appends a property; throws error::already_exists for a duplicate id.
		ref<property> append(properties& self, std::string_view id, property_kind kind, std::string label, ref<properties> group = {}, double min = 0, double max = 0, double step = 0, std::shared_ptr<detail::click_callback> on_click = {})
		{
			naming::check_key(id);
			auto p      = make<property_impl>();
			p->id       = std::string(id);
			p->kind     = kind;
			p->label    = std::move(label);
			p->min      = min;
			p->max      = max;
			p->step     = step;
			p->group    = std::move(group);
			p->on_click = std::move(on_click);
			p->owner    = weak<properties>(ref<properties>::retain(&self));

			properties_impl& i = impl(self);
			std::lock_guard  lock(i.mutex);
			for (const ref<property>& existing : i.entries) {
				if (impl(*existing).id == id) {
					throw error::already_exists(std::format("properties: duplicate id: {}", id));
				}
			}
			i.entries.push_back(p);
			return p;
		}

		ref<property> append_range(properties& self, std::string_view id, property_kind kind, std::string label, double min, double max, double step)
		{
			if (!(min <= max)) {
				throw error::invalid_argument(std::format("property {}: min must not exceed max", id));
			}
			if (!(step > 0)) {
				throw error::invalid_argument(std::format("property {}: step must be positive", id));
			}
			return append(self, id, kind, std::move(label), {}, min, max, step);
		}

		// True if `target` is `start` or a group below it.
		bool reaches(ref<properties> start, const properties* target, int depth)
		{
			if (start.get() == target) {
				return true;
			}
			if (depth > max_depth) {
				return true;
			}
			for (const ref<property>& entry : start->list()) {
				if (entry->kind() == property_kind::group && reaches(entry->group(), target, depth + 1)) {
					return true;
				}
			}
			return false;
		}

		ref<property> find_in(const properties& self, std::string_view id, int depth)
		{
			std::vector<ref<property>> entries = self.list();
			for (const ref<property>& entry : entries) {
				if (entry->id() == id) {
					return entry;
				}
			}
			if (depth >= max_depth) {
				return {};
			}
			for (const ref<property>& entry : entries) {
				if (entry->kind() == property_kind::group) {
					if (ref<property> found = find_in(*entry->group(), id, depth + 1)) {
						return found;
					}
				}
			}
			return {};
		}

	} // namespace

	std::string_view property::id() const noexcept
	{
		return impl(*this).id;
	}

	property_kind property::kind() const noexcept
	{
		return impl(*this).kind;
	}

	std::string_view property::label() const noexcept
	{
		return impl(*this).label;
	}

	bool property::enabled() const
	{
		const property_impl& i = impl(*this);
		std::lock_guard      lock(i.mutex);
		return i.enabled;
	}

	bool property::visible() const
	{
		const property_impl& i = impl(*this);
		std::lock_guard      lock(i.mutex);
		return i.visible;
	}

	void property::set_enabled(bool value)
	{
		property_impl&  i = impl(*this);
		std::lock_guard lock(i.mutex);
		i.enabled = value;
	}

	void property::set_visible(bool value)
	{
		property_impl&  i = impl(*this);
		std::lock_guard lock(i.mutex);
		i.visible = value;
	}

	double property::min() const
	{
		return numeric(*this, "min()").min;
	}

	double property::max() const
	{
		return numeric(*this, "max()").max;
	}

	double property::step() const
	{
		return numeric(*this, "step()").step;
	}

	void property::add_item(std::string label, std::string value)
	{
		property_impl& i = impl(*this);
		if (i.kind != property_kind::list) {
			throw error::invalid_argument(std::format("property {}: add_item() needs a list property", i.id));
		}
		std::lock_guard lock(i.mutex);
		i.items.emplace_back(std::move(label), std::move(value));
	}

	std::size_t property::item_count() const
	{
		const property_impl& i = impl(*this);
		if (i.kind != property_kind::list) {
			throw error::invalid_argument(std::format("property {}: item_count() needs a list property", i.id));
		}
		std::lock_guard lock(i.mutex);
		return i.items.size();
	}

	ref<properties> property::group() const
	{
		const property_impl& i = impl(*this);
		if (i.kind != property_kind::group) {
			throw error::invalid_argument(std::format("property {}: group() needs a group property", i.id));
		}
		return i.group;
	}

	bool property::click()
	{
		property_impl& i = impl(*this);
		if (i.kind != property_kind::button) {
			throw error::invalid_argument(std::format("property {}: click() needs a button property", i.id));
		}
		// Keep both objects alive for the whole callback, which runs without a lock.
		ref<properties> owner = i.owner.lock();
		if (!owner || !i.on_click) {
			return false;
		}
		ref<property>                           self     = ref<property>::retain(this);
		std::shared_ptr<detail::click_callback> callback = i.on_click;
		return (*callback)(*owner, *self);
	}

	ref<properties> properties::create()
	{
		return make<properties_impl>();
	}

	ref<property> properties::add_bool(std::string_view id, std::string label)
	{
		return append(*this, id, property_kind::boolean, std::move(label));
	}

	ref<property> properties::add_int(std::string_view id, std::string label, double min, double max, double step)
	{
		return append_range(*this, id, property_kind::integer, std::move(label), min, max, step);
	}

	ref<property> properties::add_float(std::string_view id, std::string label, double min, double max, double step)
	{
		return append_range(*this, id, property_kind::floating, std::move(label), min, max, step);
	}

	ref<property> properties::add_text(std::string_view id, std::string label)
	{
		return append(*this, id, property_kind::text, std::move(label));
	}

	ref<property> properties::add_path(std::string_view id, std::string label)
	{
		return append(*this, id, property_kind::path, std::move(label));
	}

	ref<property> properties::add_list(std::string_view id, std::string label)
	{
		return append(*this, id, property_kind::list, std::move(label));
	}

	ref<property> properties::add_color(std::string_view id, std::string label)
	{
		return append(*this, id, property_kind::color, std::move(label));
	}

	ref<property> properties::add_button(std::string_view id, std::string label, knurbs::move_only_function<bool(properties&, property&)> on_click)
	{
		if (!on_click) {
			throw error::invalid_argument(std::format("properties: button {} has no callback", id));
		}
		return append(*this, id, property_kind::button, std::move(label), {}, 0, 0, 0, std::make_shared<detail::click_callback>(std::move(on_click)));
	}

	ref<property> properties::add_group(std::string_view id, std::string label, ref<properties> group)
	{
		if (!group) {
			throw error::invalid_argument(std::format("properties: group {} is empty", id));
		}
		if (reaches(group, this, 0)) {
			throw error::invalid_argument(std::format("properties: group {} would contain its own parent", id));
		}
		return append(*this, id, property_kind::group, std::move(label), std::move(group));
	}

	ref<property> properties::add_notice(std::string_view id, std::string text)
	{
		return append(*this, id, property_kind::notice, std::move(text));
	}

	ref<property> properties::find(std::string_view id) const
	{
		return find_in(*this, id, 0);
	}

	std::vector<ref<property>> properties::list() const
	{
		const properties_impl& i = impl(*this);
		std::lock_guard        lock(i.mutex);
		return i.entries;
	}

	void properties::set_modified(std::string_view id, knurbs::move_only_function<bool(properties&, property&, data&)> callback)
	{
		naming::check_key(id);
		properties_impl&                           i = impl(*this);
		std::shared_ptr<detail::modified_callback> stored;
		if (callback) {
			stored = std::make_shared<detail::modified_callback>(std::move(callback));
		}
		// The replaced callback is destroyed after the lock is released: its destructor may call back into us.
		std::shared_ptr<detail::modified_callback> replaced;
		std::lock_guard                            lock(i.mutex);
		if (stored) {
			std::shared_ptr<detail::modified_callback>& slot = i.modified[std::string(id)];
			replaced                                         = std::move(slot);
			slot                                             = std::move(stored);
		} else if (auto it = i.modified.find(id); it != i.modified.end()) {
			replaced = std::move(it->second);
			i.modified.erase(it);
		}
	}

	bool properties::notify_modified(std::string_view id, data& settings)
	{
		properties_impl&                           i = impl(*this);
		std::shared_ptr<detail::modified_callback> callback;
		{
			std::lock_guard lock(i.mutex);
			if (auto it = i.modified.find(id); it != i.modified.end()) {
				callback = it->second;
			}
		}
		if (!callback) {
			return false;
		}
		// Keep this object and the property alive for the whole callback, which runs without a lock.
		ref<properties> self   = ref<properties>::retain(this);
		ref<property>   target = find(id);
		if (!target) {
			return false;
		}
		return (*callback)(*self, *target, settings);
	}

} // namespace knurbs
