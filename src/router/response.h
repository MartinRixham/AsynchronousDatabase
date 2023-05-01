#include <boost/json.hpp>
#include <boost/beast/http.hpp>

namespace router
{
	struct response
	{
		boost::beast::http::status status;
		
		boost::json::object body;
	};
}
