#include <algorithm>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include "log.h"
#include "base64/base64.h"
#include "url/url.h"
#include "relay.h"
#include "transfer.h"

namespace
{
	// One piece of a share: the keys after `from` up to and including `to`, either of which may be
	// missing at the ends of the table.
	struct piece
	{
		std::string from;

		bool has_from = false;

		std::string to;

		bool has_to = false;

		// Whether there is another file of this piece to ask for, and how the asking ended.
		bool going = true;

		bool whole = false;

		bool refused = false;
	};

	// What one answer does to the piece it belongs to: the file goes to the thread taking them in,
	// and the piece either moves on to the next one or stops.
	void carry(
		piece &walking,
		const router::response &answer,
		const transfer::share &wanted,
		transfer::relay &carrying)
	{
		if (answer.status != boost::beast::http::status::ok)
		{
			DEBUG("Node " + wanted.node + " did not answer for a file of \"" + wanted.table + "\".");

			walking.going = false;
			walking.refused = true;

			return;
		}

		carrying.put(answer.text);

		// Nowhere to resume is a walk that reached the end of its piece.
		if (answer.file.next.empty())
		{
			walking.going = false;
			walking.whole = true;

			return;
		}

		std::optional<std::string> next = base64::decode(answer.file.next);

		// A file that ends where the one before it did is a walk that would ask for ever.
		if (!next || (walking.has_from && *next == walking.from))
		{
			DEBUG("A file of \"" + wanted.table + "\" on " + wanted.node + " made no progress.");

			walking.going = false;
			walking.refused = true;

			return;
		}

		walking.from = *next;
		walking.has_from = true;
	}

	router::request file_request(const transfer::share &wanted, const piece &walking, size_t bytes)
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table", wanted.table, "file" };
		request.query = "partitions=" + cluster::encode_partitions(wanted.partitions);
		request.query += std::string("&values=") + (wanted.values ? "true" : "false");
		request.query += "&bytes=" + std::to_string(bytes);

		if (walking.has_from)
		{
			request.query += "&from=" + url::encode(base64::encode(walking.from));
		}

		if (walking.has_to)
		{
			request.query += "&to=" + url::encode(base64::encode(walking.to));
		}

		return request;
	}

	router::request split_request(const transfer::share &wanted)
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table", wanted.table, "split" };
		request.query = "ways=" + std::to_string(wanted.workers);

		return request;
	}

	// Where the node being read from would cut the table up. A node that will not say is a table
	// walked in one piece, which is slower and not wrong.
	std::vector<std::string> split_points(const cluster::cluster &nodes, const transfer::share &wanted)
	{
		std::vector<std::string> points;

		if (wanted.workers < 2)
		{
			return points;
		}

		router::response answer = nodes.send(wanted.node, split_request(wanted));

		if (answer.status != boost::beast::http::status::ok ||
			!answer.json.contains("keys") || !answer.json.at("keys").is_array())
		{
			DEBUG("Node " + wanted.node + " did not say where to cut \"" + wanted.table + "\" up.");

			return points;
		}

		const boost::json::array &keys = answer.json.at("keys").as_array();

		for (size_t i = 0; i < keys.size(); i++)
		{
			std::optional<std::string> key = keys[i].is_string()
				? base64::decode(std::string(keys[i].as_string()))
				: std::nullopt;

			if (!key)
			{
				return std::vector<std::string>();
			}

			points.push_back(*key);
		}

		return points;
	}

	std::vector<piece> pieces_of(const std::vector<std::string> &points)
	{
		std::vector<piece> pieces;

		for (size_t i = 0; i <= points.size(); i++)
		{
			piece walking;

			if (i > 0)
			{
				walking.from = points[i - 1];
				walking.has_from = true;
			}

			if (i < points.size())
			{
				walking.to = points[i];
				walking.has_to = true;
			}

			pieces.push_back(walking);
		}

		return pieces;
	}

	// Takes files in until the relay is closed and empty, and never stops early: a taker that gave
	// up would leave the thread asking for files waiting on a slot nothing empties.
	void take_in(transfer::relay &carrying, const transfer::taking &take, std::atomic<bool> &failed)
	{
		std::optional<std::string> file = carrying.take();

		while (file)
		{
			if (!failed)
			{
				try
				{
					take(*file);
				}
				catch (const std::exception &caught)
				{
					DEBUG(std::string("A file could not be taken in: ") + caught.what());

					failed = true;
				}
				catch (...)
				{
					DEBUG("A file could not be taken in.");

					failed = true;
				}
			}

			file = carrying.take();
		}
	}
}

transfer::outcome transfer::walk(
	const cluster::cluster &nodes,
	const share &wanted,
	const std::atomic<bool> &running,
	progress::patience &waiting,
	const taking &take)
{
	outcome done;

	if (!running)
	{
		return done;
	}

	std::vector<piece> pieces = pieces_of(split_points(nodes, wanted));

	// The budget is the whole walk's, so a share read in several pieces at once holds no more of
	// itself in memory than one read in one piece.
	size_t bytes = std::max<size_t>(wanted.bytes / pieces.size(), 1);

	// **Every request a walk makes is made on this thread**, in one fan out, so the connections it
	// holds to the node it is reading are the ones this thread already has. Threads made for a walk
	// and dropped after it would be a handshake to that node for every walk of every table.
	std::vector<relay> carrying(pieces.size());
	std::vector<std::thread> takers;
	std::atomic<bool> failed = false;

	takers.reserve(pieces.size());

	for (size_t i = 0; i < pieces.size(); i++)
	{
		takers.emplace_back([&carrying, &take, &failed, i]() { take_in(carrying[i], take, failed); });
	}

	while (running && !waiting.spent())
	{
		std::vector<cluster::enquiry> asking;
		std::vector<size_t> asked;

		for (size_t i = 0; i < pieces.size(); i++)
		{
			if (pieces[i].going)
			{
				asking.push_back(cluster::enquiry { wanted.node, file_request(wanted, pieces[i], bytes) });
				asked.push_back(i);
			}
		}

		if (asking.empty())
		{
			break;
		}

		std::vector<router::response> answers = nodes.send_each(asking);

		for (size_t i = 0; i < asked.size() && i < answers.size(); i++)
		{
			carry(pieces[asked[i]], answers[i], wanted, carrying[asked[i]]);
		}

		// A fan out that brought anything back is a walk getting somewhere, whatever was in it.
		waiting.renew();
	}

	for (size_t i = 0; i < carrying.size(); i++)
	{
		carrying[i].close();
	}

	for (size_t i = 0; i < takers.size(); i++)
	{
		takers[i].join();
	}

	// The share was read only if every piece of it was, and it was refused if any piece was: a node
	// that stopped answering one piece has stopped answering.
	done.whole = !failed;

	for (size_t i = 0; i < pieces.size(); i++)
	{
		done.whole = done.whole && pieces[i].whole;
		done.refused = done.refused || pieces[i].refused;
	}

	return done;
}
