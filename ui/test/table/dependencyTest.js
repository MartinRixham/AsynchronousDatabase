import Qunit from "qunit";
import Dependency from "~/js/table/Dependency";

QUnit.module('dependency');

QUnit.test('populate options', assert => {

	const getTables = () => ["first table", "second table"];
	const dependency = new Dependency(() => {}, getTables);
	const select = document.createElement("select");

	dependency.select().init(select);

	assert.equal(select.children.length, 2);
});
