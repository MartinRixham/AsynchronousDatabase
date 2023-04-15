import { Click, Value } from "Datum";
import html from "~/html/table/newTable.html";

export default class {

	#title = "";

	#fetchPage

	#client

	#onNewTable

	constructor(fetchPage, client, onNewTable) {

		this.#fetchPage = fetchPage;
		this.#client = client;
		this.#onNewTable = onNewTable;
	}

	onBind(element) {

		this.#fetchPage(element, html);
	}

	title = new Value((value) => {

		if (value) {

			this.#title = value;
		}

		return this.#title;
	});

	save = new Click(() => {

		const newTable = { name: this.#title };
		this.#client.postTable(newTable, () => this.#onNewTable(newTable));
	});
}
