import { Text } from "@datumjs/datum";

// One zone of the membership as the host node sees it. The number of zones is the number of
// copies of the keyspace, and the nodes of a zone are what that copy is split between.
export default class Zone {

	#name;

	#nodes;

	constructor(name, nodes) {

		this.#name = name;
		this.#nodes = nodes;
	}

	title = new Text(() => this.#name);

	count = new Text(() => this.#nodes.length + (this.#nodes.length == 1 ? " node" : " nodes"));
}
