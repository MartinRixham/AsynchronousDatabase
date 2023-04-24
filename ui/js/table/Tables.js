import { Text, Binding } from "Datum";
import html from "~/html/table/tables.html";

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

	async onBind(element) {

		this.#fetchPage(element, html);

		this.tables = (await this.#client.getTables()).tables.map((table) => new Text(() => table.name));
	}

	newTableButton = new Binding({
		click: () => {
			this.newTable = new NewTable(this.#fetchPage, () => this.tables, this.#client, newTable => {
				this.newTable = null;
				this.tables.push(new Text(() => newTable.name));
			});
		},
		visible: () => !this.newTable
	});
}
