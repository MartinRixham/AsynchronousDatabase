import { Binding, Click } from "Datum";
import html from "~/html/table/newTable.html";

import NewDependency from "./NewDependency";

export default class {

	name = "";

	newDependency = null;

	dependencies = [];

	serverError;

	updated;

	#fetchPage;

	#getTables;

	#client;

	#onNewTable;

	#onCancel;

	#tables;

	constructor(fetchPage, getTables, client, onNewTable, onCancel) {

		this.#fetchPage = fetchPage;
		this.#getTables = getTables;
		this.#client = client;
		this.#onNewTable = onNewTable;
		this.#onCancel = onCancel;
	}

	async onBind(element) {

		this.#fetchPage(element, html);
		this.#tables = await this.#getTables();

		this.newDependency =
			new NewDependency(
				0,
				this.#tables,
				this.#addDependency.bind(this),
				this.#removeDependency.bind(this));
	}

	title = new Binding({
		value: value => {

			if (value != undefined) {

				this.name = value;
				this.updated = true;
			}

			return this.name;
		},
		classes: {
			"input-error": () => !this.#isValid() && this.updated
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

			this.updated = true;
		},
		classes: {
			disabled: () => !this.#isValid()
		}
	});

	cancel = new Click(() => this.#onCancel());

	error = new Binding({
		text: () => this.serverError
	});

	#isValid() {

		return this.name.length > 0;
	}

	#addDependency(dependency) {

		this.dependencies.push(dependency);
		this.#resetNewDependency();
	}

	#removeDependency(name) {

		for (let i = this.dependencies.length - 1; i >= 0; i--) {

			if (this.dependencies[i].name == name) {

				this.dependencies.splice(i, 1);
			}
		}

		this.#resetNewDependency();
	}

	#resetNewDependency() {

		if (this.dependencies.length < 2) {

			this.newDependency =
				new NewDependency(
					this.dependencies.length,
					this.#tables.filter(table => !this.dependencies.map(dependency => dependency.name).includes(table)),
					this.#addDependency.bind(this),
					this.#removeDependency.bind(this));
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
