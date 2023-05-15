import { Update } from "Datum";

export default class {

	name;

	#dependencyPosition;

	#dependantPosition;

	constructor(name, dependencyPosition, dependantPosition) {

		this.name = name;
		this.#dependencyPosition = dependencyPosition;
		this.#dependantPosition = dependantPosition;
	}

	pointer = new Update(element => {

		const from = this.#dependencyPosition();
		const to = this.#dependantPosition();

		element.setAttribute(
			"d",
			"M " + (90 + from.width * 180) +
			" " + (165 + from.depth * 180) +
			" C " + (90 + from.width * 180) +
			" " + (210 + from.depth * 180) +
			" " + (90 + to.width * 180) +
			" " + (80 + to.depth * 180) +
			" " + (90 + to.width * 180) +
			" " + (120 + to.depth * 180));
	});
}
