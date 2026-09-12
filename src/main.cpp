#include "taskengine/version.hpp"

#include <iostream>

// Task 1.1 only: prove that the library links and the program runs.
// Argument parsing, validation and real exit codes arrive with the CLI at M6.
int main() {
    std::cout << "task-engine " << taskengine::version() << '\n';
    return 0;
}
