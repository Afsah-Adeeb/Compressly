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

int runAll(const std::string& filter) {
  int failures = 0;
  int selected = 0;
  for (const TestCase& test : registry()) {
    // Substring match, not an exact name: running one test usually means running the
    // handful that share a prefix.
    if (!filter.empty() && std::string(test.name).find(filter) == std::string::npos) continue;
    ++selected;
    try {
      test.fn();
      std::printf("  pass  %s\n", test.name);
    } catch (const std::exception& e) {
      std::printf("  FAIL  %s\n    %s\n", test.name, e.what());
      ++failures;
    }
  }
  if (selected == 0) {
    std::printf("no test matched \"%s\"\n", filter.c_str());
    return 1;
  }
  std::printf("\n%d/%d passed\n", selected - failures, selected);
  return failures == 0 ? 0 : 1;
}

}  // namespace testing

// An optional argument filters tests by substring: `tests Lz77` runs only those.
int main(int argc, char** argv) {
  return testing::runAll(argc > 1 ? argv[1] : "");
}
