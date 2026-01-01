// Minimal native (Windows) demo build entry point.
// This environment is intended as a quick compile sanity-check on non-POSIX hosts.

#include <cstdio>

int main(int /*argc*/, char** /*argv*/) {
	std::puts("OpenSprinkler demo build: OK");
	return 0;
}
