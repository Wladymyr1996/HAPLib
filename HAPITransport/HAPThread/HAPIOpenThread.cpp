#include <HAPITransport/HAPThread/HAPIOpenThread.hpp>

// The destructor is defined here and nowhere else so the vtable has exactly one
// home, the way HAPITransport.cpp does it.
HAPIOpenThread::~HAPIOpenThread() = default;
