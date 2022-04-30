#include <intercept.hpp>
#include <optional>
#include <utility>

namespace intercept_db {
	using namespace intercept::types;

	inline std::pair<int, int> callstack_item_stack(const vm_context::callstack_item& item) {
		return { item._stackEndAtStart, item._stackEnd };
	}

	class callstack_item : public vm_context::callstack_item {
	public:
		inline callstack_item(vm_context::callstack_item *parent) {
			if (parent != nullptr) {
				set_parent(parent);
			}
		}

		std::pair<int, int> stack() const;
		std::optional<std::pair<int, int>> parent_stack() const;
	protected:
		inline void set_parent(vm_context::callstack_item* parent) {
			_parent = parent;
			_varSpace.parent = &parent->_varSpace;
		}
	};
}
