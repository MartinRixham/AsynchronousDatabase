import { Binding } from "Datum";
import Dependency from "./Dependency";

export default class {

	name = "";

	dependencies = [];

	graphPosision;

	constructor(table, graphPosision) {

		this.name = table.name;
		this.graphPosition = () => graphPosision(table.name);
		this.dependencies = table.dependencies.map(dependency => new Dependency(dependency, this.graphPosition));
	}

	title = new Binding({
		text: () => this.name,
		update: element => {
			let { depth, width } = this.graphPosition();
			element.setAttribute("y", 150 + depth * 180);
			element.setAttribute("x", 50 + width * 180);
		}
	});

	arrow = new Binding({
		update: element => {
			let { depth, width } = this.graphPosition();
			element.setAttribute("y", 100 + depth * 180);
			element.setAttribute("x", 50 + width * 180);
		}
	});
}
