import QUnit from "qunit";
import NewTable from "~/js/table/NewTable";
import DatabaseClient from "../FakeDatabaseClient";

QUnit.module("new table");

QUnit.test("set table value", assert => {

	const newTable = new NewTable(() => {}, () => []);

	newTable.onBind();

	newTable.title().value("my new table");

	assert.equal(newTable.title().value(), "my new table");
});

QUnit.test("save table", async assert => {

	let savedTable = null;

	const onNewTable = (table) => {

		savedTable = table;
	};

	const client = new DatabaseClient();
	const newTable = new NewTable(() => {}, () => [], client, onNewTable);

	await newTable.onBind();

	newTable.title().value("my new table");
	await newTable.save().click();

	assert.equal(savedTable.name, "my new table");

	const tables = await client.getTables();

	assert.equal(tables.tables.length, 1);
	assert.equal(tables.tables[0].name, "my new table");
});

QUnit.test("fail to save table with no name", async assert => {

	const client = new DatabaseClient();
	const newTable = new NewTable(() => {}, () => [], client, () => {});

	await newTable.onBind();

	newTable.title().value("");
	newTable.save().click();

	const tables = await client.getTables();

	assert.equal(tables.tables.length, 0);
});

QUnit.test("no new dependency when no options", async assert => {

	const client = new DatabaseClient();

	client.postTable("\name\":\"my dependency\"")

	const newTable = new NewTable(() => {}, () => [], client, () => {});

	await newTable.onBind();

	assert.ok(!newTable.dependencyTitle().visible());
	assert.ok(!newTable.newDependency)
});

QUnit.test("save table with one dependency", async assert => {

	const client = new DatabaseClient();

	client.postTable("\name\":\"my dependency\"")

	const newTable = new NewTable(() => {}, () => ["my dependency"], client, () => {});

	await newTable.onBind();

	newTable.title().value("my new table");
	newTable.newDependency.select().value("my dependency")
	newTable.newDependency.add().click();

	assert.ok(newTable.dependencyTitle().visible());
	assert.equal(newTable.dependencies[0].title().text(), "my dependency");
	assert.deepEqual(newTable.toJSON(), { name: "my new table", dependencies: ["my dependency"] });

	newTable.save().click();

	const tables = await client.getTables();

	assert.equal(tables.tables.length, 2);

	const firstTable = tables.tables[1];

	assert.equal(firstTable.name, "my new table");
	assert.equal(firstTable.dependencies.length, 1);
	assert.equal(firstTable.dependencies[0].name, "my dependency");
});

QUnit.test("no second dependency when only one option", async assert => {

	const client = new DatabaseClient();
	const tables = () => ["my dependency"];
	const newTable = new NewTable(() => {}, tables, client, () => {});

	await newTable.onBind();

	newTable.title().value("my new table");

	assert.equal(newTable.newDependency.label().text(), "first dependency")

	newTable.newDependency.select().value("my dependency")
	newTable.newDependency.add().click();

	assert.ok(!newTable.newDependency)
});

QUnit.test("save table with two dependencies", async assert => {

	const client = new DatabaseClient();
	const options = () => ["my dependency", "my other dependency"];
	const newTable = new NewTable(() => {}, options, client, () => {});

	await newTable.onBind();

	newTable.title().value("my new table");

	assert.ok(newTable.dependencyTitle().visible());
	assert.equal(newTable.newDependency.label().text(), "first dependency")

	newTable.newDependency.select().value("my dependency")
	newTable.newDependency.add().click();

	assert.equal(newTable.newDependency.label().text(), "second dependency")

	newTable.newDependency.select().value("my other dependency")
	newTable.newDependency.add().click();

	assert.equal(newTable.dependencies[0].title().text(), "my dependency");
	assert.equal(newTable.dependencies[1].title().text(), "my other dependency");
	assert.deepEqual(newTable.toJSON(), { name: "my new table", dependencies: ["my dependency", "my other dependency"] });

	newTable.save().click();

	const tables = await client.getTables();
	const firstTable = tables.tables[0];

	assert.equal(firstTable.dependencies.length, 2);
	assert.equal(firstTable.dependencies[0].name, "my dependency");
	assert.equal(firstTable.dependencies[1].name, "my other dependency");
});

QUnit.test("cannot add third dependency", async assert => {

	const client = new DatabaseClient();
	const options = () => ["my dependency", "my other dependency", "even another one"];
	const newTable = new NewTable(() => {}, options, client, () => {});

	await newTable.onBind();

	newTable.newDependency.select().value("my dependency")
	newTable.newDependency.add().click();

	newTable.newDependency.select().value("my other dependency")
	newTable.newDependency.add().click();

	assert.ok(!newTable.newDependency);
});

QUnit.test("fail to save table with duplicate", async assert => {

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

	assert.equal(newTable.error().text(), "A table with the name \"a table\" already exists.");
	assert.equal(savedTables.length, 1);
});

QUnit.test("remove dependency", async assert => {

	const newTable = new NewTable(() => {}, () => ["my dependency"], new DatabaseClient(), () => {});

	await newTable.onBind();

	newTable.title().value("my new table");
	newTable.newDependency.select().value("my dependency")
	newTable.newDependency.add().click();

	assert.equal(newTable.dependencies.length, 1);

	newTable.dependencies[0].remove().click();

	assert.equal(newTable.dependencies.length, 0);
	assert.ok(!newTable.newDependency.select().value());
	assert.equal(newTable.newDependency.label().text(), "first dependency")
});

QUnit.test("remove second dependency", async assert => {

	const options = () => ["my dependency", "my other dependency"];
	const newTable = new NewTable(() => {}, options, new DatabaseClient(), () => {});

	await newTable.onBind();

	newTable.title().value("my new table");
	newTable.newDependency.select().value("my dependency")
	newTable.newDependency.add().click();
	newTable.newDependency.select().value("my other dependency")
	newTable.newDependency.add().click();

	assert.equal(newTable.dependencies.length, 2);

	newTable.dependencies[1].remove().click();

	assert.equal(newTable.dependencies.length, 1);
	assert.ok(!newTable.newDependency.select().value());
	assert.equal(newTable.newDependency.label().text(), "second dependency")
});

QUnit.test("cannot add same dependency twice", async assert => {

	const client = new DatabaseClient();
	const options = () => ["first table", "second table"];
	const newTable = new NewTable(() => {}, options, client, () => {});

	await newTable.onBind();

	assert.equal(newTable.newDependency.options.length, 2);

	newTable.newDependency.select().value("first table")
	newTable.newDependency.add().click();

	assert.equal(newTable.newDependency.options.length, 1);
});
