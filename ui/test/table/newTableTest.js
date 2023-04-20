import Qunit from "qunit";
import NewTable from "~/js/table/NewTable";
import DatabaseClient from "../FakeDatabaseClient";

QUnit.module('new table');

QUnit.test('set table value', assert => {

	const newTable = new NewTable(() => {}, () => []);

	newTable.title().value("my new table");

	assert.equal(newTable.title().value(), "my new table");
});

QUnit.test('save table', assert => {

	const client = new DatabaseClient();
	const newTable = new NewTable(() => {}, () => [], client);

	newTable.title().value("my new table");
	newTable.save().click();

	client.getTables((tables) => {

		assert.equal(tables.tables.length, 1);
		assert.equal(tables.tables[0].name, "my new table");
	}); 
});

QUnit.test('fail to save table with no name', assert => {

	const client = new DatabaseClient();
	const newTable = new NewTable(() => {}, () => [], client);

	newTable.title().value("");
	newTable.save().click();

	client.getTables((tables) => {

		assert.equal(tables.tables.length, 0);
	});
});

QUnit.test('save table with one dependency', assert => {

	const client = new DatabaseClient();

	client.postTable("\name\":\"my dependency\"")

	const newTable = new NewTable(() => {}, () => [], client);

	newTable.title().value("my new table");
	newTable.newDependency.select().value("my dependency")
	newTable.newDependency.add().click();

	assert.ok(!newTable.newDependency.select().value());
	assert.equal(newTable.dependencies[0].title().text(), "my dependency");

	newTable.save().click();

	client.getTables((tables) => {

		assert.equal(tables.tables.length, 2);

		const firstTable = tables.tables[1];

		assert.equal(firstTable.name, "my new table");
		assert.equal(firstTable.dependencies.length, 1);
		assert.equal(firstTable.dependencies[0].name, "my dependency");
	}); 
});
