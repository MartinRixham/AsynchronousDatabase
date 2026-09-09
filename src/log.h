#ifndef LOG_H
#define LOG_H

#include <iostream>
#include <syncstream>

#define LEVEL_DEBUG 1

#if LOG == LEVEL_DEBUG
// Every thread of the pool logs to the one stream, and a line written in several inserts is one
// another thread splices itself into the middle of. The line is assembled here and handed over
// whole.
#define DEBUG(message) std::osyncstream(std::cerr) << "DEBUG: " << message << std::endl;
#else
#define DEBUG(message)
#endif

#endif
