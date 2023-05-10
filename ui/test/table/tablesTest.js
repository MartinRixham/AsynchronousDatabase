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
	assert.equal(tableOne.dependencyDepth(), 0);

	const tableTwo = tables.tables[1];

	assert.equal(tableTwo.title().text(), "second table");
	assert.equal(tableTwo.dependencyDepth(), 1);
	assert.equal(tableTwo.dependencies[0]().text(), "first table");
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

	assert.equal(tables.tables[0].dependencyDepth(), 2);
	assert.equal(tables.tables[1].dependencyDepth(), 1);
	assert.equal(tables.tables[2].dependencyDepth(), 0);
});
