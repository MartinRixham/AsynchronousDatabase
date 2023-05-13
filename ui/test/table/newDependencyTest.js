import QUnit from "qunit";
import { Text } from "Datum";
import NewDependency from "~/js/table/NewDependency";

QUnit.module('new dependency');

QUnit.test('list dependency options', async assert => {

	const getTables = () => new Promise(resolve => resolve([new Text(() => "first table"), new Text(() => "second table")]));
	const dependency = new NewDependency(() => {}, getTables, 0);

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
	const dependency = new NewDependency(addDependency, getTables, 0);

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
	const dependency = new NewDependency(addDependency, getTables, 0);

	dependency.select().value("");
	dependency.add().click();

	assert.ok(!newDependency);
});

QUnit.test('add second dependency', assert => {

	const getTables = () => [new Text(() => "first table"), new Text(() => "second table")];
	const dependency = new NewDependency(() => {}, getTables, 1);

	assert.equal(dependency.label().text(), "second dependency");
});
