import { Value, Click, Text } from "Datum";

export default class {

	name = "";

	options = [];

	#index;

	#addDependency;

	#removeDependency;

	constructor(index, tables, addDependency, removeDependency) {

		this.#index = index;
		this.options = tables.map(table => new Text(() => table));
		this.#addDependency = addDependency;
		this.#removeDependency = removeDependency;
	}

	select = new Value(value => {

		if (value) {

			this.name = value;
			this.#addDependency(this)
		}

		return value;
	});

	remove = new Click(() => {

		this.#removeDependency(this.name);
	});

	title = new Text(() => this.name);

	label = new Text(() => this.#index < 1 ? "first dependency" : "second dependency");

	toJSON() {
	
		return this.name;
	}
}
