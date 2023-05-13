import QUnit from "qunit";
import Tables from "~/js/table/Tables";
import DatabaseClient from "../FakeDatabaseClient";

QUnit.module('tables');

QUnit.test('get tables', async assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "first table", dependencies: [] });
	client.postTable({ name: "second table", dependencies: ["first table"] });

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	assert.equal(tables.tables.length, 2);

	const tableOne = tables.tables[0];

	assert.equal(tableOne.title().text(), "first table");
	assert.deepEqual(tableOne.graphPosition(), { depth: 0, width: 0 });

	const tableTwo = tables.tables[1];

	assert.equal(tableTwo.title().text(), "second table");
	assert.deepEqual(tableTwo.graphPosition(), { depth: 1, width: 0 });
	assert.equal(tableTwo.dependencies[0].title().text(), "first table");
});

QUnit.test('new table', async assert => {

	const client = new DatabaseClient();

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	tables.newTableButton().click();

	await tables.newTable.onBind();
	await tables.newTable.newDependency.onBind();

	assert.ok(tables.newTable);
});

QUnit.test('add tables in wrong order', async assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "third table", dependencies: ["second table"] });
	client.postTable({ name: "second table", dependencies: ["first table"] });
	client.postTable({ name: "first table", dependencies: [] });

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	assert.equal(tables.tables.length, 3);

	assert.deepEqual(tables.tables[0].graphPosition(), { depth: 2, width: 0 });
	assert.deepEqual(tables.tables[1].graphPosition(), { depth: 1, width: 0 });
	assert.deepEqual(tables.tables[2].graphPosition(), { depth: 0, width: 0 });
});

QUnit.test('add tables with same dependency', async assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "third table", dependencies: ["first table"] });
	client.postTable({ name: "first table", dependencies: [] });
	client.postTable({ name: "second table", dependencies: ["first table"] });

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	assert.equal(tables.tables.length, 3);

	assert.deepEqual(tables.tables[0].graphPosition(), { depth: 1, width: 1 });
	assert.deepEqual(tables.tables[1].graphPosition(), { depth: 0, width: 0.5 });
	assert.deepEqual(tables.tables[2].graphPosition(), { depth: 1, width: 0 });
});

QUnit.test('add tables with no dependencies', async assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "first table", dependencies: [] });
	client.postTable({ name: "second table", dependencies: [] });

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	assert.equal(tables.tables.length, 2);

	assert.deepEqual(tables.tables[0].graphPosition(), { depth: 0, width: 1 });
	assert.deepEqual(tables.tables[1].graphPosition(), { depth: 0, width: 0 });
});

QUnit.test('add tables nearest dependency', async assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "first table", dependencies: [] });
	client.postTable({ name: "second table", dependencies: [] });
	client.postTable({ name: "third table", dependencies: ["first table"] });
	client.postTable({ name: "fourth table", dependencies: ["second table"] });

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	assert.equal(tables.tables.length, 4);

	assert.deepEqual(tables.tables[0].graphPosition(), { depth: 0, width: 1 });
	assert.deepEqual(tables.tables[1].graphPosition(), { depth: 0, width: 0 });
	assert.deepEqual(tables.tables[2].graphPosition(), { depth: 1, width: 1 });
	assert.deepEqual(tables.tables[3].graphPosition(), { depth: 1, width: 0 });
});

QUnit.test('add tables nearest dependency the other way round', async assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "first table", dependencies: [] });
	client.postTable({ name: "second table", dependencies: [] });
	client.postTable({ name: "third table", dependencies: ["second table"] });
	client.postTable({ name: "fourth table", dependencies: ["first table"] });

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	assert.equal(tables.tables.length, 4);

	assert.deepEqual(tables.tables[0].graphPosition(), { depth: 0, width: 1 });
	assert.deepEqual(tables.tables[1].graphPosition(), { depth: 0, width: 0 });
	assert.deepEqual(tables.tables[2].graphPosition(), { depth: 1, width: 0 });
	assert.deepEqual(tables.tables[3].graphPosition(), { depth: 1, width: 1 });
});

QUnit.test('add table with two dependencies', async assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "first table", dependencies: [] });
	client.postTable({ name: "second table", dependencies: ["first table"] });
	client.postTable({ name: "third table", dependencies: ["first table", "second table"] });

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	assert.equal(tables.tables.length, 3);

	assert.deepEqual(tables.tables[0].graphPosition(), { depth: 0, width: 0 });
	assert.deepEqual(tables.tables[1].graphPosition(), { depth: 1, width: 0 });
	assert.deepEqual(tables.tables[2].graphPosition(), { depth: 2, width: 0 });
});
