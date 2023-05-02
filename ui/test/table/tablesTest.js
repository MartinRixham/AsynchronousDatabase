import QUnit from "qunit";
import Tables from "~/js/table/Tables";
import DatabaseClient from "../FakeDatabaseClient";

QUnit.module('tables');

QUnit.test('get tables', async assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "first table", dependencies: [] });
	client.postTable({ name: "second table", dependencies: ["dependency one"] });

	const tables = new Tables(() => {}, client);

	await tables.onBind();

	assert.equal(tables.tables.length, 2);
	assert.equal(tables.tables[0].title().text(), "first table");
	assert.equal(tables.tables[1].title().text(), "second table");
	assert.equal(tables.tables[1].dependencies[0]().text(), "dependency one");
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
