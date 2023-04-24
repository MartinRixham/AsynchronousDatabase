import { Binding } from "Datum";
import html from "~/html/table/newTable.html";

import Dependency from "./Dependency";

export default class {

	name = "";

	newDependency = null;

	dependencies = [];

	#fetchPage;

	#client;

	#onNewTable;

	#nameChanged;

	#getTables;

	constructor(fetchPage, getTables, client, onNewTable) {

		this.newDependency = new Dependency(this.#addDependency.bind(this), getTables);
		this.#fetchPage = fetchPage;
		this.#client = client;
		this.#onNewTable = onNewTable;
		this.#getTables = getTables;
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

				this.#client.postTable(newTable);
				this.#onNewTable(newTable)
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
		this.newDependency = new Dependency(this.#addDependency.bind(this), this.#getTables);
	}
}
