import { Binding } from "Datum";
import html from "~/html/table/newTable.html";

export default class {

	name = "";

	#fetchPage

	#client

	#onNewTable

	#nameChanged

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
			"uk-form-danger": () => !this.#isValid() && this.#nameChanged
		}
	});

	save = new Binding({
		click: () => {

			if (this.#isValid()) {

				const newTable = { name: this.name };
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
}
