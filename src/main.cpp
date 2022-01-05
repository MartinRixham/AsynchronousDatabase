#include <memory>
#include <iostream>

#include "server/server.h"

int main(void)
{
    std::make_shared<server::server>("127.0.0.1", 8080)->serve();

    return EXIT_SUCCESS;
}
