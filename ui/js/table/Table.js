import { Binding } from "Datum";

export default class {

	name = "";

	dependencies = [];

	#index;

	constructor(table, index) {

		this.name = table.name;
		this.dependencies = table.dependencies.map(dependency => new Text(() => dependency));
		this.#index = index;
	}

	title = new Binding({
		text: () => this.name,
		init: element => { element.setAttribute("y", this.#index * 100); }
	});
}
