import { Binding } from "Datum";
import html from "~/html/table/newTable.html";

import Dependency from "./Dependency";

export default class {

	name = "";

	newDependency = new Dependency(this.#addDependency.bind(this));

	dependencies = [];

	#fetchPage;

	#client;

	#onNewTable;

	#nameChanged;

	constructor(fetchPage, client, onNewTable) {

		this.#fetchPage = fetchPage;
		this.#client = client;
		this.#onNewTable = onNewTable;
	}

	onBind(element) {

		this.#fetchPage(element, html);
	}

	title = new Binding({
		value: value => {

			if (value != undefined) {

				this.name = value;
				this.#nameChanged = true;
			}

			return this.name;
		},
		classes: {
			"input-error": () => !this.#isValid() && this.#nameChanged
		}
	});

	save = new Binding({
		click: () => {

			if (this.#isValid()) {

				const newTable = { name: this.name, dependencies: this.dependencies };
				this.#client.postTable(newTable, () => this.#onNewTable(newTable));
			}
		},
		update: (element) => {

			element.disabled = !this.#isValid();
		}
	});

	#isValid() {

		return this.name.length > 0;
	}

	#addDependency(dependency) {

		this.dependencies.push(dependency);
		this.newDependency = new Dependency(this.#addDependency.bind(this));
	}
}
