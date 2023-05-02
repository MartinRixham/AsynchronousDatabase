import QUnit from "qunit";
import Table from "~/js/table/Table";

QUnit.module('table');

QUnit.test('table has name and dependencies', async assert => {

	const table = new Table({
		name: "table name",
		dependencies: ["dependency one", "dependency two"]
	});

	assert.equal(table.title().text(), "table name");

	assert.equal(table.dependencies[0]().text(), "dependency one");
	assert.equal(table.dependencies[1]().text(), "dependency two");
});

