#include <algorithm>
#include <stdexcept>

#include "crowdsim/commands/Command.hpp"

namespace crowdsim::commands {

Json makeCommand(const std::string& name, Json params) {
    return Json{{"cmd", name}, {"params", std::move(params)}};
}

void CommandRegistry::add(CommandSpec spec) {
    if (find(spec.name) != nullptr) {
        throw std::logic_error("command '" + spec.name + "' registered twice");
    }
    specs_.push_back(std::move(spec));
}

const CommandSpec* CommandRegistry::find(const std::string& name) const {
    const auto it = std::find_if(specs_.begin(), specs_.end(), [&](const CommandSpec& s) { return s.name == name; });
    return it == specs_.end() ? nullptr : &*it;
}

}  // namespace crowdsim::commands
