import { Binding } from "Datum";
import html from "~/html/table/newTable.html";

import NewDependency from "./NewDependency";

export default class {

	name = "";

	newDependency = null;

	dependencies = [];

	serverError;

	#fetchPage;

	#client;

	#onNewTable;

	#nameChanged;

	#getTables;

	constructor(fetchPage, getTables, client, onNewTable) {

		this.newDependency = new NewDependency(this.#addDependency.bind(this), getTables, this.dependencies.length);
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
		click: async () => {

			if (this.#isValid()) {

				const result = await this.#client.postTable(this);

				if (result && result.error) {

					this.serverError = result.error;
				}
				else {

					this.#onNewTable(this)
				}
			}
		},
		update: (element) => {

			element.disabled = !this.#isValid();
		}
	});

	error = new Binding({
		text: () => this.serverError
	});

	#isValid() {

		return this.name.length > 0;
	}

	#addDependency(dependency) {

		this.dependencies.push(dependency);

		if (this.dependencies.length < 2) {

			this.newDependency = new NewDependency(this.#addDependency.bind(this), this.#getTables);
		}
		else {

			this.newDependency = null;
		}
	}

	toJSON() {

		return {
			name: this.name,
			dependencies: this.dependencies.map(dependency => dependency.toJSON())
		};
	}
}
