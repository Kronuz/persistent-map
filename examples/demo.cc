#include "persistent_map.h"
#include <iostream>
#include <string>
int main() {
	using Map = persistent_map::PersistentMap<std::string, int>;
	Map values;
	values = values.set("rollout", 10);
	auto saved = values;
	values = values.set("rollout", 25);
	std::cout << "Saved: " << *saved.find("rollout") << ", current: " << *values.find("rollout") << '\n';
	saved.retire_root();
	values.retire_root();
	while (values.reclamation_pending())
		values.collect(128);
}
