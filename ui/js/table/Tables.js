import { Binding } from "Datum";
import html from "~/html/table/tables.html";

import NewTable from "./NewTable";
import Table from "./Table";

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

		this.tables = (await this.#client.getTables()).tables.map(table => new Table(table));
	}

	newTableButton = new Binding({
		click: () => {
			this.newTable = new NewTable(
				this.#fetchPage,
				() => this.tables.map(table => table.title),
				this.#client,
				this.#onNewTable.bind(this));
		},
		visible: () => !this.newTable
	});

	#onNewTable(newTable) {

		this.newTable = null;
		this.tables.push(new Table(newTable.toJSON()));
	}
}
