import Qunit from "qunit";
import Tables from "~/js/table/Tables";
import DatabaseClient from "../FakeDatabaseClient";

QUnit.module('tables');

QUnit.test('get tables', assert => {
	
	const client = new DatabaseClient();

	client.postTable({ name: "first table" });
	client.postTable({ name: "second table" });

	const tables = new Tables(() => {}, client);
	tables.onBind();

	assert.equal(tables.tables.length, 2);
	assert.equal(tables.tables[0]().text(), "first table");
	assert.equal(tables.tables[1]().text(), "second table");
});
