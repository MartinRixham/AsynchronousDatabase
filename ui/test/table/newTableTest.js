import QUnit from "qunit";
import NewTable from "~/js/table/NewTable";
import DatabaseClient from "../FakeDatabaseClient";

QUnit.module('new table');

QUnit.test('set table value', assert => {

	const newTable = new NewTable(() => {}, () => []);

	newTable.title().value("my new table");

	assert.equal(newTable.title().value(), "my new table");
});

QUnit.test('save table', async assert => {

	let savedTable = null;

	const onNewTable = (table) => {

		savedTable = table;
	};

	const client = new DatabaseClient();
	const newTable = new NewTable(() => {}, () => [], client, onNewTable);

	newTable.title().value("my new table");
	await newTable.save().click();

	assert.equal(savedTable.name, "my new table");

	const tables = await client.getTables();

	assert.equal(tables.tables.length, 1);
	assert.equal(tables.tables[0].name, "my new table");
});

QUnit.test('fail to save table with no name', async assert => {

	const client = new DatabaseClient();
	const newTable = new NewTable(() => {}, () => [], client, () => {});

	newTable.title().value("");
	newTable.save().click();

	const tables = await client.getTables();

	assert.equal(tables.tables.length, 0);
});

QUnit.test('save table with one dependency', async assert => {

	const client = new DatabaseClient();

	client.postTable("\name\":\"my dependency\"")

	const newTable = new NewTable(() => {}, () => [], client, () => {});

	newTable.title().value("my new table");
	newTable.newDependency.select().value("my dependency")
	newTable.newDependency.add().click();

	assert.ok(!newTable.newDependency.select().value());
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

QUnit.test('fail to save table with duplicate', async assert => {

	let savedTables = [];

	const onNewTable = (table) => {

		savedTables.push(table);
	};

	const client = new DatabaseClient();
	const newTable = new NewTable(() => {}, () => [], client, onNewTable);

	newTable.title().value("a table");
	newTable.save().click();
	await newTable.save().click();

	assert.equal(newTable.error().text(), "A table with the name \"a table\" already exists.");
	assert.equal(savedTables.length, 1);
});
