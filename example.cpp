#include "backtrace.hpp"

// #undef eigen_assert
// #define eigen_assert(cond) backtrace_assert(cond, #cond)
// #include <eigen3/Eigen/Dense>

size_t factorial(size_t const n) {
  assert(n > 0); // will fail
  return n * factorial(n - 1);
}

int main() {
  factorial(3);
}
