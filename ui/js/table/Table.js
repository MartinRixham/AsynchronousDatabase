import { Binding } from "Datum";

export default class {

	name = "";

	dependencies = [];

	dependencyDepth;

	constructor(table, dependencyDepth) {

		this.name = table.name;
		this.dependencyDepth = () => dependencyDepth(table.dependencies);

		this.dependencies = table.dependencies.map((dependency, index) =>
			new Binding({
				text: () => dependency,
				init: element => {
					element.setAttribute("y", 50 + this.dependencyDepth() * 180);
					element.setAttribute("x", 50 + index * 180);
				}
			}));
	}

	title = new Binding({
		text: () => this.name,
		init: element => { element.setAttribute("y", 150 + this.dependencyDepth() * 180); }
	});

	arrow = new Binding({
		init: element => { element.setAttribute("y", 100 + this.dependencyDepth() * 180); }
	});
}
