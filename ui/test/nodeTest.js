import { describe, test, expect } from "vitest";
import DatabaseClient from "./FakeDatabaseClient";
import Node from "~/js/Node";

describe("node", () => {

	const bound = async client => {

		const node = new Node(() => {}, client);

		await node.onBind(document.createElement("DIV"));

		return node;
	};

	test("the health of a node in a cluster", async () => {

		const client = new DatabaseClient();

		client.setHealth({
			status: "ok",
			write_stalled: false,
			incomplete: false,
			nodes: ["http://one:8080", "http://two:8080", "http://three:8080"],
			zones: { one: ["http://one:8080", "http://two:8080"], two: ["http://three:8080"] },
			leads: 128
		});

		const node = await bound(client);

		expect(node.status().text()).toBe("ok");
		expect(node.writes().text()).toBe("flowing");
		expect(node.store().text()).toBe("whole");
		expect(node.nodes().text()).toBe("3");
		expect(node.leads().text()).toBe("128 of 256");
		expect(node.clustered().visible()).toBe(true);
		expect(node.alone().visible()).toBe(false);
	});

	test("a zone for every copy of the keyspace", async () => {

		const client = new DatabaseClient();

		client.setHealth({
			status: "ok",
			write_stalled: false,
			incomplete: false,
			nodes: ["http://one:8080", "http://two:8080", "http://three:8080"],
			zones: { one: ["http://one:8080", "http://two:8080"], two: ["http://three:8080"] },
			leads: 128
		});

		const node = await bound(client);

		expect(node.zoned().visible()).toBe(true);
		expect(node.zones.length).toBe(2);
		expect(node.zones[0].title().text()).toBe("one");
		expect(node.zones[0].count().text()).toBe("2 nodes");
		expect(node.zones[1].title().text()).toBe("two");
		expect(node.zones[1].count().text()).toBe("1 node");
	});

	// Back pressure is not failure, and a node applying it still says its status is ok, so the
	// panel says it where the status cannot.
	test("writes that are stalled", async () => {

		const client = new DatabaseClient();

		client.setHealth({ status: "ok", write_stalled: true, incomplete: false });

		expect((await bound(client)).writes().text()).toBe("stalled");
	});

	// A node in a membership too small to claim a leader orders no write, so it takes none —
	// which is a different thing from a store pushing back, and says so.
	test("writes that nothing can order", async () => {

		const client = new DatabaseClient();

		client.setHealth({ status: "ok", write_stalled: false, incomplete: false, unled: true });

		expect((await bound(client)).writes().text()).toBe("unled");
	});

	// A node holding less than it owns serves what it has and answers health like any other.
	test("a store that is short of what the node owns", async () => {

		const client = new DatabaseClient();

		client.setHealth({ status: "ok", write_stalled: false, incomplete: true });

		const node = await bound(client);

		expect(node.status().text()).toBe("ok");
		expect(node.store().text()).toBe("short");
	});

	// Where the membership comes from, and whether this node is still in it: a lease it no longer
	// holds is a node that cannot reach etcd, which nothing else in the panel says.
	test("the etcd a node reads its membership from", async () => {

		const client = new DatabaseClient();

		client.setHealth({
			status: "ok",
			write_stalled: false,
			incomplete: false,
			nodes: ["http://one:8080", "http://two:8080"],
			leads: 128,
			etcd: { registered: true, endpoint: "http://etcd:2379" }
		});

		const node = await bound(client);

		expect(node.registered().visible()).toBe(true);
		expect(node.lease().text()).toBe("held");
		expect(node.endpoint().text()).toBe("http://etcd:2379");
	});

	// A node that cannot reach etcd goes on serving what it holds, and says so where the
	// membership it can no longer read cannot.
	test("an etcd that stopped answering", async () => {

		const client = new DatabaseClient();

		client.setHealth({
			status: "ok",
			write_stalled: false,
			incomplete: false,
			nodes: ["http://one:8080"],
			leads: 0,
			etcd: { registered: false, endpoint: "http://etcd:2379" }
		});

		const node = await bound(client);

		expect(node.lease().text()).toBe("lost");
		expect(node.endpoint().text()).toBe("http://etcd:2379");
	});

	// An instance told no etcd owns the whole keyspace, and names no membership at all.
	test("an instance that was never clustered", async () => {

		const client = new DatabaseClient();

		client.setHealth({ status: "ok", write_stalled: false, incomplete: false });

		const node = await bound(client);

		expect(node.alone().visible()).toBe(true);
		expect(node.clustered().visible()).toBe(false);
		expect(node.zoned().visible()).toBe(false);
		expect(node.registered().visible()).toBe(false);
		expect(node.leads().text()).toBe("");
	});

	// The nginx in front of the database serves its own error document, so a node that is not up
	// answers a document naming a code rather than failing the request. It is still not an answer
	// about a node.
	test("the proxy answering instead of the database", async () => {

		const client = new DatabaseClient();

		client.setHealth({ error: { code: "unavailable", message: "The database is not up." } });

		const node = await bound(client);

		expect(node.status().text()).toBe("not answering");
		expect(node.answered().visible()).toBe(false);
		expect(node.alone().visible()).toBe(false);
	});

	// The panel is drawn by the instance it is asking about, so a node that has stopped is a page
	// that is still up with nothing to say — which is the state worth naming rather than throwing.
	test("a node that does not answer", async () => {

		const client = new DatabaseClient();

		client.setHealth(null);

		const node = await bound(client);

		expect(node.status().text()).toBe("not answering");
		expect(node.answered().visible()).toBe(false);
	});
});
