import { Text } from "Datum";
import html from "~/html/table/tables.html";

import fetchPage from "~/js/fetchPage";

export default class {

	#fetchPage

	#client

	tables = [];

	constructor(fetchPage, client) {

		this.#fetchPage = fetchPage;
		this.#client = client;
	}

	onBind(element) {

		this.#fetchPage(element, html);

		this.#client.getTables(tables => {
			this.tables = tables.tables.map((table) => new Text(() => table.name))
		});
	}
}
