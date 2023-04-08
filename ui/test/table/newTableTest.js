import Qunit from "qunit";
import NewTable from "~/js/table/NewTable";
import DatabaseClient from "../FakeDatabaseClient";

QUnit.module('new table');

QUnit.test('set table value', assert => {

	const newTable = new NewTable(() => {});
	const title = newTable.title();

	title.value("my new table");

	assert.equal(title.value(), "my new table");
});

QUnit.test('save table', assert => {

	const client = new DatabaseClient();
	const newTable = new NewTable(() => {}, client);
	const title = newTable.title();
	const save = newTable.save();

	title.value("my new table");
	save.click();

	const tables = client.getTables(); 

	assert.equal(tables.length, 1);
	assert.equal(tables[0].title, "my new table");
});
