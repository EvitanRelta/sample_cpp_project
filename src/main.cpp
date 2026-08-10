#include <iostream>
#include <string>
#include "CLI11.hpp"
#include "greeting.h"

int main(int argc, char** argv) {
    CLI::App app{"A minimal C++20 app-plus-library sample"};
    std::string name = "world";
    app.add_option("-n,--name", name, "Name to greet");
    CLI11_PARSE(app, argc, argv);

    std::cout << sample_cpp_project::make_greeting(name) << '\n';
    return 0;
}
