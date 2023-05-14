import { Binding, Update } from "Datum";
import Dependency from "./Dependency";

export default class {

	name = "";

	dependencies = [];

	graphPosition;

	constructor(table, graphPosition) {

		this.name = table.name;
		this.graphPosition = () => graphPosition(table.name);

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

			element.setAttribute("y", 150 + depth * 180);
			element.setAttribute("x", 50 + width * 180);
		}
	});

	box = new Update(element => {

		const { depth, width } = this.graphPosition();

		element.setAttribute("y", 125 + depth * 180);
		element.setAttribute("x", 20 + width * 180);
	});
}
