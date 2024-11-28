import { Binding } from "Datum";
import Dependency from "./Dependency";

export default class Table {

	name = "";

	dependencies = [];

	graphPosition;

	#edit;

	constructor(table, graphPosition, edit) {

		this.name = table.name;
		this.graphPosition = () => graphPosition(table.name);
		this.#edit = edit;

		this.dependencies =
			table.dependencies.map(dependency =>
				new Dependency(
					dependency,
					() => graphPosition(dependency),
					this.graphPosition));
	}

	title = new Binding({
		text: () => this.name,
		update: element => {

			const { depth, width } = this.graphPosition();

			element.setAttribute("y", 50 + depth * 180);
			element.setAttribute("x", 50 + width * 180);
		}
	});

	box = new Binding({
		click: () => this.#edit(),
		update: element => {

			const { depth, width } = this.graphPosition();

			element.setAttribute("y", 25 + depth * 180);
			element.setAttribute("x", 20 + width * 180);
		}
	});
}
