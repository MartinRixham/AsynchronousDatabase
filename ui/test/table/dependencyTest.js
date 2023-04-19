import Qunit from "qunit";
import Dependency from "~/js/table/Dependency";

QUnit.module('dependency');

QUnit.test('add dependency', assert => {

	let newDependency = null;

	const addDependency = (dependency) => {

		newDependency = dependency;
	}

	const getTables = () => ["first table", "second table"];
	const dependency = new Dependency(addDependency, getTables);

	dependency.select().value("second table");
	dependency.add().click();

	assert.equal(newDependency.title().text(), "second table");
});
