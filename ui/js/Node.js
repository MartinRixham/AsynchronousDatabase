import { Text, Visible } from "@datumjs/datum";

import html from "~/html/node.html?url";
import Zone from "./Zone";

export default class Node {

	// Public, because a private field is one datum does not watch: the panel is drawn before the
	// node has answered, and what redraws it is this being assigned.
	health = null;

	zones = [];

	#fetchPage;

	#client;

	constructor(fetchPage, client) {

		this.#fetchPage = fetchPage;
		this.#client = client;
	}

	async onBind(element) {

		this.#fetchPage(element, html);

		// A node that does not answer is a state to draw and not an error to throw: it is what
		// this panel shows while the instance is starting, or once it has stopped.
		try {

			this.health = await this.#client.getHealth();
		}
		catch {

			this.health = null;
		}

		this.zones = Object.entries(this.health?.zones ?? {})
			.map(([name, nodes]) => new Zone(name, nodes));
	}

	// An answer without a status is not an answer from the database: the nginx in front of it
	// serves its own error document, so a node that is not up is a document naming a code rather
	// than a request that failed.
	answered = new Visible(() => !!this.health?.status);

	status = new Text(() => this.health?.status ?? "not answering");

	// A node that can order no write takes none at all, whatever the store underneath it is
	// doing, so it is the first of the three this line can say.
	writes = new Text(() => {

		if (this.health?.unled) {

			return "unled";
		}

		return this.health?.write_stalled ? "stalled" : "flowing";
	});

	// A node holding less than it owns serves what it has and answers health like any other, so
	// this is the field that says what the status cannot.
	store = new Text(() => this.health?.incomplete ? "short" : "whole");

	// The membership, the partitions this node orders the writes of, and the zones it is grouped
	// into are all absent from an instance that was never clustered.
	clustered = new Visible(() => !!this.health?.nodes);

	alone = new Visible(() => !!this.health?.status && !this.health.nodes);

	nodes = new Text(() => String(this.health?.nodes?.length ?? ""));

	leads = new Text(() => this.health?.nodes ? this.health.leads + " of 256" : "");

	zoned = new Visible(() => this.zones.length > 0);
}
