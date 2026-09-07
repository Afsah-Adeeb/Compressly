#include "testing.h"

#include <cstdio>
#include <exception>

namespace testing {

std::vector<TestCase>& registry() {
  // Function-local static: the registry is guaranteed to be constructed before the first
  // Registrar runs, whatever order the translation units initialise in.
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }

void fail(const char* file, int line, const std::string& message) {
  std::ostringstream os;
  os << file << ":" << line << "\n    " << message;
  throw std::runtime_error(os.str());
}

int runAll() {
  int failures = 0;
  for (const TestCase& test : registry()) {
    try {
      test.fn();
      std::printf("  pass  %s\n", test.name);
    } catch (const std::exception& e) {
      std::printf("  FAIL  %s\n    %s\n", test.name, e.what());
      ++failures;
    }
  }
  std::printf("\n%d/%d passed\n", static_cast<int>(registry().size()) - failures,
              static_cast<int>(registry().size()));
  return failures == 0 ? 0 : 1;
}

}  // namespace testing

int main() { return testing::runAll(); }
