import { Value, Binding, Text } from "Datum";

export default class {

	name = "";

	options = [];

	#addDependency;

	#getTables;

	#index;

	constructor(addDependency, getTables, index) {

		this.#addDependency = addDependency;
		this.#getTables = getTables;
		this.#index = index;
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

	add = new Binding({

		click: () => {

			if (this.name) {

				this.#addDependency(this)
			}
		}
	});

	title = new Text(() => this.name);

	label = new Text(() => this.#index < 1 ? "first dependency" : "second dependency");

	toJSON() {
	
		return this.name;
	}
}
