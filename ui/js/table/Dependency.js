import { Update } from "Datum";

export default class Dependency {

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
			" " + (65 + from.depth * 180) +
			" C " + (90 + from.width * 180) +
			" " + (110 + from.depth * 180) +
			" " + (90 + to.width * 180) +
			" " + (-20 + to.depth * 180) +
			" " + (90 + to.width * 180) +
			" " + (20 + to.depth * 180));
	});
}
