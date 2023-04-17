import { Value, Binding, Text } from "Datum";

export default class {

	name = "";

	#addDependency;

	constructor(addDependency) {

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
