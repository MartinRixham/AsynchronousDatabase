import { Value, Click, Text } from "Datum";

export default class {

	name = "";

	options = [];

	#index;

	#getTables;

	#addDependency;

	#removeDependency;

	constructor(index, getTables, addDependency, removeDependency) {

		this.#index = index;
		this.#getTables = getTables;
		this.#addDependency = addDependency;
		this.#removeDependency = removeDependency;
	}

	async onBind() {

		this.options = await this.#getTables();
	}

	select = new Value(value => {

		if (value != undefined) {

			this.name = value;
		}

		return value;
	});

	add = new Click(() => {

		if (this.name) {

			this.#addDependency(this)
		}
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
