import QUnit from "qunit";
import Table from "~/js/table/Table";

QUnit.module('table');

QUnit.test('table has name and dependencies', async assert => {

	const table = new Table({
		name: "table name",
		dependencies: ["dependency one", "dependency two"]
	},
	() => ({ width: 3, depth: 0 }));

	assert.equal(table.title().text(), "table name");
	assert.deepEqual(table.graphPosition(), { width: 3, depth: 0 });

	assert.equal(table.dependencies[0].name, "dependency one");
	assert.equal(table.dependencies[1].name, "dependency two");
});

