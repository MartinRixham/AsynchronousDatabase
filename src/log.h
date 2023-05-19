#ifndef LOG_H
#define LOG_H

#include <iostream>

#if LOG == debug
#define DEBUG(message) std::cerr << "DEBUG: " << message << std::endl;
#else
#define DEBUG(message)
#endif

#endif
