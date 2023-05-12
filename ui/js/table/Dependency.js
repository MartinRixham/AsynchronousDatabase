import { Binding } from "Datum";

export default class {

	name = "";

	#graphPosition;

	constructor(name, graphPosition) {

		this.name = name;
		this.#graphPosition = graphPosition;
	}

	title = 
		new Binding({
			text: () => this.name,
			update: element => {
				let { depth, width } = this.#graphPosition();
				element.setAttribute("y", 50 + depth * 180);
				element.setAttribute("x", 50 + width * 180);
			}
		});
}
