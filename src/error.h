#ifndef ERROR_H
#define ERROR_H

#define ERROR(error) "" + std::string(__FILE__) + " " + std::string(__func__) + "(line:" + std::to_string(__LINE__) + ") " + (error)

#endif
