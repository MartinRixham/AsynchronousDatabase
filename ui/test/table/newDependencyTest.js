import QUnit from "qunit";
import { Text } from "Datum";
import NewDependency from "~/js/table/NewDependency";

QUnit.module('new dependency');

QUnit.test('list dependency options', async assert => {

	const getTables = () => new Promise(resolve => resolve([new Text(() => "first table"), new Text(() => "second table")]));
	const dependency = new NewDependency(() => {}, getTables);

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
	const dependency = new NewDependency(addDependency, getTables);

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
	const dependency = new NewDependency(addDependency, getTables);

	dependency.select().value("");
	dependency.add().click();

	assert.ok(!newDependency);
});
