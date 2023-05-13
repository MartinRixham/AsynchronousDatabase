import { Binding, Update } from "Datum";
import html from "~/html/table/tables.html";

import NewTable from "./NewTable";
import Table from "./Table";

export default class {

	tables = [];

	tableGraph = [[]];

	newTable = null;

	#fetchPage;

	#client;

	#totalWidth;

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

	svg = new Update(element => {

		element.setAttribute("height", 100 + this.tableGraph.length * 180);
		element.setAttribute("width", 100 + this.#totalWidth * 180);
	})

	#onNewTable(newTable) {

		this.newTable = null;
		this.#insertTable(newTable.toJSON());
	}

	#insertTable(table) {

		this.tables.push(new Table(table, this.#graphPosition.bind(this)));

		const { tableGraph, totalWidth } = this.#buildGraph();

		this.#totalWidth = totalWidth;
		this.tableGraph = tableGraph;
	}

	#graphPosition(name) {

		for (let i = 0; i < this.tableGraph.length; i++) {

			for (let j = 0; j < this.tableGraph[i].length; j++) {

				if (this.tableGraph[i][j] == name) {

					const blockSize = this.#totalWidth / this.tableGraph[i].length;
					const offset = 0.5;

					return { depth: i, width: ((j + offset) * blockSize) - offset };
				}
			}
		}

		return { depth: 0, width: 0 }
	}

	#buildGraph() {

		let tables = [...this.tables];
		let tableGraph = [[]]

		for (let i = tables.length - 1; i >= 0; i--) {

			if (!tables[i].dependencies.length) {

				tableGraph[0].push(tables.splice(i, 1)[0].name);
			}
		}

		let totalWidth = tableGraph[0].length;

		return this.#buildRow(tableGraph, totalWidth, tables);
	}

	#buildRow(tableGraph, totalWidth, tables) {

		if (!tables.length) {

			return { tableGraph, totalWidth, tables }
		}

		const lastRow = tableGraph[tableGraph.length - 1];
		let row = [];

		for (let i = tables.length - 1; i >= 0; i--) {

			if (this.#intersect(tables[i].dependencies, lastRow)) {

				row.push(tables.splice(i, 1)[0].name);
			}
		}

		if (!row.length) {

			const table = tables.splice(0, 1);
			row.push(table[0].name);
		}

		row.sort((a, b) => this.#widthSum(a, lastRow) - this.#widthSum(b, lastRow));
		tableGraph.push(row);

		return this.#buildRow(tableGraph, Math.max(totalWidth, row.length), tables);
	}

	#intersect(dependencies, row) {

		return dependencies.some(dependency => row.includes(dependency.name));
	}

	#widthSum(name, dependencies) {

		const table = this.tables.find(table => table.name == name)
		let total = 0;

		for (let i = 0; i < dependencies.length; i++) {

			if (table.dependencies.some(dependency => dependency.name == dependencies[i])) {

				total += i;
			}
		}

		return total;
	}
}
