import { Binding } from "Datum";
import html from "~/html/table/tables.html";

import NewTable from "./NewTable";
import Table from "./Table";

export default class {

	tables = [];

	newTable = null;

	#fetchPage;

	#client;

	constructor(fetchPage, client) {

		this.#fetchPage = fetchPage;
		this.#client = client;
	}

	async onBind(element) {

		this.#fetchPage(element, html);

		(await this.#client.getTables()).tables.map(this.#insertTable.bind(this));
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
		this.#insertTable(newTable.toJSON());
	}

	#insertTable(table) {

		this.tables.push(new Table(table, (dependencies) => {
			this.tables.subscribableLength;
			return this.tables
				.filter(table => dependencies.includes(table.name))
				.reduce((max, table) => Math.max(max, table.dependencyDepth() + 1), 0);
		}));
	}
}
