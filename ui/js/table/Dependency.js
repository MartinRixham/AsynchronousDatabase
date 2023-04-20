import { Value, Binding, Text } from "Datum";

export default class {

	name = "";

	options = [];

	#addDependency;

	constructor(addDependency, getTables) {

		getTables(tables => {

			this.options = tables;
		});

		this.#addDependency = addDependency;
	}

	select = new Value(value => {

		if (value != undefined) {

			this.name = value;
		}

		return value;
	});

	add = new Binding({

		click: () => this.#addDependency(this)
	});

	title = new Text(() => this.name);
}
