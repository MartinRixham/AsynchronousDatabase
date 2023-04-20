import Qunit from "qunit";
import { Text } from "Datum";
import Dependency from "~/js/table/Dependency";

QUnit.module('dependency');

QUnit.test('list dependency options', assert => {

	const getTables = callback => callback([new Text(() => "first table"), new Text(() => "second table")]);
	const dependency = new Dependency(() => {}, getTables);

	assert.equal(dependency.options.length, 2);
	assert.equal(dependency.options[0]().text(), "first table");
	assert.equal(dependency.options[1]().text(), "second table");
});

QUnit.test('add dependency', assert => {

	let newDependency = null;

	const addDependency = (dependency) => {

		newDependency = dependency;
	}

	const getTables = callback => callback([new Text(() => "first table"), new Text(() => "second table")]);
	const dependency = new Dependency(addDependency, getTables);

	dependency.select().value("second table");
	dependency.add().click();

	assert.equal(newDependency.title().text(), "second table");
});
