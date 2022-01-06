#include <memory>
#include <iostream>

#include "server/server.h"

int main(void)
{
	std::make_shared<server::server>(8080)->serve();

	return EXIT_SUCCESS;
}
