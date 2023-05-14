import { Binding, Update } from "Datum";

export default class {

	name;

	#dependencyPosition;

	#dependantPosition;

	constructor(name, dependencyPosition, dependantPosition) {

		this.name = name;
		this.#dependencyPosition = dependencyPosition;
		this.#dependantPosition = dependantPosition;
	}

	title = 
		new Binding({
			text: () => this.name,
			update: element => {

				const { depth, width } = this.#dependantPosition();

				element.setAttribute("y", 50 + depth * 180);
				element.setAttribute("x", 50 + width * 180);
			}
		});

	pointer = new Update(element => {

		const from = this.#dependencyPosition();
		const to = this.#dependantPosition();

		element.setAttribute("y1", 165 + from.depth * 180);
		element.setAttribute("x1", 90 + from.width * 180);
		element.setAttribute("y2", 125 + to.depth * 180);
		element.setAttribute("x2", 90 + to.width * 180);
	});

}
