import { describe, test, expect } from "vitest";
import Tables from "~/js/table/Tables";
import DatabaseClient from "../FakeDatabaseClient";

describe("tables", () => {

	test("get tables", async () => {

		const client = new DatabaseClient();

		client.postTable({ name: "first table", dependencies: [] });
		client.postTable({ name: "second table", dependencies: ["first table"] });

		const tables = new Tables(() => {}, client);

		await tables.onBind();

		expect(tables.tables.length).toBe(2);

		const tableOne = tables.tables[0];

		expect(tableOne.title().text()).toBe("first table");
		expect(tableOne.graphPosition()).toEqual({ depth: 0, width: 0 });

		const tableTwo = tables.tables[1];

		expect(tableTwo.title().text()).toBe("second table");
		expect(tableTwo.graphPosition()).toEqual({ depth: 1, width: 0 });
		expect(tableTwo.dependencies[0].name).toBe("first table");
	});

	test("new table", async () => {

		const client = new DatabaseClient();

		const tables = new Tables(() => {}, client);

		await tables.onBind();

		tables.newTableButton().click();

		await tables.newTable.onBind();

		expect(tables.newTable).toBeTruthy();
	});

	test("add tables in wrong order", async () => {

		const client = new DatabaseClient();

		client.postTable({ name: "third table", dependencies: ["second table"] });
		client.postTable({ name: "second table", dependencies: ["first table"] });
		client.postTable({ name: "first table", dependencies: [] });

		const tables = new Tables(() => {}, client);

		await tables.onBind();

		expect(tables.tables.length).toBe(3);

		expect(tables.tables[0].graphPosition()).toEqual({ depth: 2, width: 0 });
		expect(tables.tables[1].graphPosition()).toEqual({ depth: 1, width: 0 });
		expect(tables.tables[2].graphPosition()).toEqual({ depth: 0, width: 0 });
	});

	test("add tables with same dependency", async () => {

		const client = new DatabaseClient();

		client.postTable({ name: "third table", dependencies: ["first table"] });
		client.postTable({ name: "first table", dependencies: [] });
		client.postTable({ name: "second table", dependencies: ["first table"] });

		const tables = new Tables(() => {}, client);

		await tables.onBind();

		expect(tables.tables.length).toBe(3);

		expect(tables.tables[0].graphPosition()).toEqual({ depth: 1, width: 1 });
		expect(tables.tables[1].graphPosition()).toEqual({ depth: 0, width: 0.5 });
		expect(tables.tables[2].graphPosition()).toEqual({ depth: 1, width: 0 });
	});

	test("add tables with no dependencies", async () => {

		const client = new DatabaseClient();

		client.postTable({ name: "first table", dependencies: [] });
		client.postTable({ name: "second table", dependencies: [] });

		const tables = new Tables(() => {}, client);

		await tables.onBind();

		expect(tables.tables.length).toBe(2);

		expect(tables.tables[0].graphPosition()).toEqual({ depth: 0, width: 1 });
		expect(tables.tables[1].graphPosition()).toEqual({ depth: 0, width: 0 });
	});

	test("add tables nearest dependency", async () => {

		const client = new DatabaseClient();

		client.postTable({ name: "first table", dependencies: [] });
		client.postTable({ name: "second table", dependencies: [] });
		client.postTable({ name: "third table", dependencies: ["first table"] });
		client.postTable({ name: "fourth table", dependencies: ["second table"] });

		const tables = new Tables(() => {}, client);

		await tables.onBind();

		expect(tables.tables.length).toBe(4);

		expect(tables.tables[0].graphPosition()).toEqual({ depth: 0, width: 1 });
		expect(tables.tables[1].graphPosition()).toEqual({ depth: 0, width: 0 });
		expect(tables.tables[2].graphPosition()).toEqual({ depth: 1, width: 1 });
		expect(tables.tables[3].graphPosition()).toEqual({ depth: 1, width: 0 });
	});

	test("add tables nearest dependency the other way round", async () => {

		const client = new DatabaseClient();

		client.postTable({ name: "first table", dependencies: [] });
		client.postTable({ name: "second table", dependencies: [] });
		client.postTable({ name: "third table", dependencies: ["second table"] });
		client.postTable({ name: "fourth table", dependencies: ["first table"] });

		const tables = new Tables(() => {}, client);

		await tables.onBind();

		expect(tables.tables.length).toBe(4);

		expect(tables.tables[0].graphPosition()).toEqual({ depth: 0, width: 1 });
		expect(tables.tables[1].graphPosition()).toEqual({ depth: 0, width: 0 });
		expect(tables.tables[2].graphPosition()).toEqual({ depth: 1, width: 0 });
		expect(tables.tables[3].graphPosition()).toEqual({ depth: 1, width: 1 });
	});

	test("add table with two dependencies", async () => {

		const client = new DatabaseClient();

		client.postTable({ name: "first table", dependencies: [] });
		client.postTable({ name: "second table", dependencies: ["first table"] });
		client.postTable({ name: "third table", dependencies: ["first table", "second table"] });

		const tables = new Tables(() => {}, client);

		await tables.onBind();

		expect(tables.tables.length).toBe(3);

		expect(tables.tables[0].graphPosition()).toEqual({ depth: 0, width: 0 });
		expect(tables.tables[1].graphPosition()).toEqual({ depth: 1, width: 0 });
		expect(tables.tables[2].graphPosition()).toEqual({ depth: 2, width: 0 });
	});
});
