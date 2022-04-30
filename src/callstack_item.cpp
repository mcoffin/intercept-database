#include "callstack_item.h"

namespace intercept_db {
	using namespace intercept::types;

	std::pair<int, int> callstack_item::stack() const {
		return callstack_item_stack(*this);
	}

	std::optional<std::pair<int, int>> callstack_item::parent_stack() const {
		if (_parent == nullptr) {
			return std::nullopt;
		} else {
			return callstack_item_stack(*_parent);
		}
	}
}
