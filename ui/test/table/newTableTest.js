import { describe, test, expect } from "vitest";
import NewTable from "~/js/table/NewTable";
import DatabaseClient from "../FakeDatabaseClient";

describe("new table", () => {

	test("set table value", () => {

		const newTable = new NewTable(() => {}, () => []);

		newTable.onBind();

		newTable.title().value("my new table");

		expect(newTable.title().value()).toBe("my new table");
	});

	test("save table", async () => {

		let savedTable = null;

		const onNewTable = (table) => {

			savedTable = table;
		};

		const client = new DatabaseClient();
		const newTable = new NewTable(() => {}, () => [], client, onNewTable);

		await newTable.onBind();

		newTable.title().value("my new table");
		await newTable.save().click();

		expect(savedTable.name).toBe("my new table");

		const tables = await client.getTables();

		expect(tables.tables.length).toBe(1);
		expect(tables.tables[0].name).toBe("my new table");
	});

	test("fail to save table with no name", async () => {

		const client = new DatabaseClient();
		const newTable = new NewTable(() => {}, () => [], client, () => {});

		await newTable.onBind();

		newTable.title().value("");
		newTable.save().click();

		const tables = await client.getTables();

		expect(tables.tables.length).toBe(0);
	});

	test("no new dependency when no options", async () => {

		const client = new DatabaseClient();

		client.postTable("\name\":\"my dependency\"")

		const newTable = new NewTable(() => {}, () => [], client, () => {});

		await newTable.onBind();

		expect(newTable.dependencyTitle().visible()).toBeFalsy();
		expect(newTable.newDependency).toBeFalsy();
	});

	test("save table with one dependency", async () => {

		const client = new DatabaseClient();

		client.postTable("\name\":\"my dependency\"")

		const newTable = new NewTable(() => {}, () => ["my dependency"], client, () => {});

		await newTable.onBind();

		newTable.title().value("my new table");
		newTable.newDependency.select().value("my dependency")

		expect(newTable.dependencyTitle().visible()).toBeTruthy();
		expect(newTable.dependencies[0].title().text()).toBe("my dependency");
		expect(newTable.toJSON()).toEqual({ name: "my new table", dependencies: ["my dependency"] });

		newTable.save().click();

		const tables = await client.getTables();

		expect(tables.tables.length).toBe(2);

		const firstTable = tables.tables[1];

		expect(firstTable.name).toBe("my new table");
		expect(firstTable.dependencies.length).toBe(1);
		expect(firstTable.dependencies[0].name).toBe("my dependency");
	});

	test("no second dependency when only one option", async () => {

		const client = new DatabaseClient();
		const tables = () => ["my dependency"];
		const newTable = new NewTable(() => {}, tables, client, () => {});

		await newTable.onBind();

		newTable.title().value("my new table");

		expect(newTable.newDependency.label().text()).toBe("first dependency");

		newTable.newDependency.select().value("my dependency")

		expect(newTable.newDependency).toBeFalsy();
	});

	test("save table with two dependencies", async () => {

		const client = new DatabaseClient();
		const options = () => ["my dependency", "my other dependency"];
		const newTable = new NewTable(() => {}, options, client, () => {});

		await newTable.onBind();

		newTable.title().value("my new table");

		expect(newTable.dependencyTitle().visible()).toBeTruthy();
		expect(newTable.newDependency.label().text()).toBe("first dependency");

		newTable.newDependency.select().value("my dependency")

		expect(newTable.newDependency.label().text()).toBe("second dependency");

		newTable.newDependency.select().value("my other dependency")

		expect(newTable.dependencies[0].title().text()).toBe("my dependency");
		expect(newTable.dependencies[1].title().text()).toBe("my other dependency");
		expect(newTable.toJSON()).toEqual({ name: "my new table", dependencies: ["my dependency", "my other dependency"] });

		newTable.save().click();

		const tables = await client.getTables();
		const firstTable = tables.tables[0];

		expect(firstTable.dependencies.length).toBe(2);
		expect(firstTable.dependencies[0].name).toBe("my dependency");
		expect(firstTable.dependencies[1].name).toBe("my other dependency");
	});

	test("cannot add third dependency", async () => {

		const client = new DatabaseClient();
		const options = () => ["my dependency", "my other dependency", "even another one"];
		const newTable = new NewTable(() => {}, options, client, () => {});

		await newTable.onBind();

		newTable.newDependency.select().value("my dependency")

		newTable.newDependency.select().value("my other dependency")

		expect(newTable.newDependency).toBeFalsy();
	});

	test("fail to save table with duplicate", async () => {

		let savedTables = [];

		const onNewTable = (table) => {

			savedTables.push(table);
		};

		const client = new DatabaseClient();
		const newTable = new NewTable(() => {}, () => [], client, onNewTable);

		await newTable.onBind();

		newTable.title().value("a table");
		newTable.save().click();
		await newTable.save().click();

		expect(newTable.error().text()).toBe("A table with the name \"a table\" already exists.");
		expect(savedTables.length).toBe(1);
	});

	test("remove dependency", async () => {

		const newTable = new NewTable(() => {}, () => ["my dependency"], new DatabaseClient(), () => {});

		await newTable.onBind();

		newTable.title().value("my new table");
		newTable.newDependency.select().value("my dependency")

		expect(newTable.dependencies.length).toBe(1);

		newTable.dependencies[0].remove().click();

		expect(newTable.dependencies.length).toBe(0);
		expect(newTable.newDependency.select().value()).toBeFalsy();
		expect(newTable.newDependency.label().text()).toBe("first dependency");
	});

	test("remove second dependency", async () => {

		const options = () => ["my dependency", "my other dependency"];
		const newTable = new NewTable(() => {}, options, new DatabaseClient(), () => {});

		await newTable.onBind();

		newTable.title().value("my new table");
		newTable.newDependency.select().value("my dependency")
		newTable.newDependency.select().value("my other dependency")

		expect(newTable.dependencies.length).toBe(2);

		newTable.dependencies[1].remove().click();

		expect(newTable.dependencies.length).toBe(1);
		expect(newTable.newDependency.select().value()).toBeFalsy();
		expect(newTable.newDependency.label().text()).toBe("second dependency");
	});

	test("cannot add same dependency twice", async () => {

		const client = new DatabaseClient();
		const options = () => ["first table", "second table"];
		const newTable = new NewTable(() => {}, options, client, () => {});

		await newTable.onBind();

		expect(newTable.newDependency.options.length).toBe(2);

		newTable.newDependency.select().value("first table")

		expect(newTable.newDependency.options.length).toBe(1);
	});
});
