import { Binding } from "Datum";
import Dependency from "./Dependency";
import TableDetail from "./TableDetail";
import fetchPage from "~/js/fetchPage";

export default class {

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

			element.setAttribute("y", 150 + depth * 180);
			element.setAttribute("x", 50 + width * 180);
		}
	});

	box = new Binding({
		click: () =>this.#edit.open(new TableDetail(this.#toJSON(), fetchPage, this.#edit.cancel)),
		update: element => {

			const { depth, width } = this.graphPosition();

			element.setAttribute("y", 125 + depth * 180);
			element.setAttribute("x", 20 + width * 180);
		}
	});

	#toJSON() {

		return {
			name: this.name,
			dependencies: [...this.dependencies]
		};
	}
}
