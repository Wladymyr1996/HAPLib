#include <HAPITransport/HAPITransport.hpp>

// The destructor is defined here and nowhere else so the vtable has exactly one
// home. Without it every translation unit that sees this header emits its own.
HAPITransport::~HAPITransport() = default;
