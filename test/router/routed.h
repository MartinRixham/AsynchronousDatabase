#pragma once

#include "router/request.h"
#include "router/response.h"
#include "router/router.h"

namespace router
{
	// What route() answers, run to its end on an io_context of the caller's own.
	response routed(router &serving, const request &asked);
}
