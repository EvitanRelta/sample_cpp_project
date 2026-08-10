#include "greeting.h"

namespace sample_cpp_project {

std::string make_greeting(const std::string& name) {
    return "Hello, " + name + "!";
}

}  // namespace sample_cpp_project
