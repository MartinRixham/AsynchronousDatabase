import { Text, Binding } from "Datum";
import html from "~/html/table/tables.html";

import fetchPage from "~/js/fetchPage";
import NewTable from "./NewTable";

export default class {

	#fetchPage;

	#client;

	tables = [];

	newTable = null;

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

	newTableButton = new Binding({
		click: () => {
			this.newTable = new NewTable(this.#fetchPage, this.#client, newTable => {
				this.newTable = null;
				this.tables.push(new Text(() => newTable.name));
			});
		},
		visible: () => !this.newTable
	});
}
