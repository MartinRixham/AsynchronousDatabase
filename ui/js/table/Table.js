import { Text } from "Datum";

export default class {

	name = "";

	dependencies = [];

	constructor(table) {

		this.name = table.name;
		this.dependencies = table.dependencies.map(dependency => new Text(() => dependency));
	}

	title = new Text(() => this.name);
}
