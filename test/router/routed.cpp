#include <exception>
#include <optional>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include "routed.h"

router::response router::routed(router &serving, const request &asked)
{
	boost::asio::io_context context;
	std::optional<response> answered;
	std::exception_ptr failed;

	boost::asio::co_spawn(
		context,
		serving.route(asked),
		[&answered, &failed](const std::exception_ptr &thrown, response answer)
		{
			failed = thrown;
			answered = std::move(answer);
		});

	context.run();

	if (failed)
	{
		std::rethrow_exception(failed);
	}

	return answered.value();
}
