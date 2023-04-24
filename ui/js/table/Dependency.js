import { Value, Binding, Text } from "Datum";

export default class {

	name = "";

	options = [];

	#addDependency;

	#getTables;

	constructor(addDependency, getTables) {

		this.#addDependency = addDependency;
		this.#getTables = getTables;
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
}
