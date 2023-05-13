import QUnit from "qunit";
import { Text } from "Datum";
import NewDependency from "~/js/table/NewDependency";

QUnit.module('new dependency');

QUnit.test('list dependency options', async assert => {

	const getTables = () => new Promise(resolve => resolve([new Text(() => "first table"), new Text(() => "second table")]));
	const dependency = new NewDependency(0, getTables, () => {}, () => {});

	await dependency.onBind();

	assert.equal(dependency.options.length, 2);
	assert.equal(dependency.options[0]().text(), "first table");
	assert.equal(dependency.options[1]().text(), "second table");
});

QUnit.test('add dependency', assert => {

	let newDependency = null;

	const addDependency = (dependency) => {

		newDependency = dependency;
	}

	const getTables = () => [new Text(() => "first table"), new Text(() => "second table")];
	const dependency = new NewDependency(0, getTables, addDependency);

	assert.equal(dependency.label().text(), "first dependency");

	dependency.select().value("second table");
	dependency.add().click();

	assert.equal(newDependency.title().text(), "second table");
});

QUnit.test('cannot add empty dependency', assert => {

	let newDependency = null;

	const addDependency = (dependency) => {

		newDependency = dependency;
	}

	const getTables = () => [new Text(() => "first table"), new Text(() => "second table")];
	const dependency = new NewDependency(0, getTables, addDependency);

	dependency.select().value("");
	dependency.add().click();

	assert.ok(!newDependency);
});

QUnit.test('add second dependency', assert => {

	const getTables = () => [new Text(() => "first table"), new Text(() => "second table")];
	const dependency = new NewDependency(1, getTables, () => {});

	assert.equal(dependency.label().text(), "second dependency");
});

QUnit.test('remove dependency', assert => {

	let removedDependency = null;

	const removeDependency = (name) => {

		removedDependency = name;
	}

	const dependency = new NewDependency(0, () => [], () => {}, removeDependency);

	dependency.select().value("first table");
	dependency.remove().click();

	assert.equal(removedDependency, "first table");
});
