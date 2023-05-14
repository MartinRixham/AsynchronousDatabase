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

		this.tables = [];
		this.#fetchPage(element, html);

		const tables = await this.#client.getTables();

		this.#insertTables(tables.tables);
	}

	newTableButton = new Binding({
		click: () => {
			this.newTable = new NewTable(
				this.#fetchPage,
				() => this.tables.map(table => table.name),
				this.#client,
				this.#onNewTable.bind(this),
				this.#onCancelNewTable.bind(this));
		},
		visible: () => !this.newTable
	});

	svg = new Update(element => {

		element.setAttribute("height", 10 + this.tableGraph.length * 180);
		element.setAttribute("width", 25 + this.#totalWidth * 180);
	})

	#onNewTable(newTable) {

		this.newTable = null;
		this.#insertTables([newTable.toJSON()]);
	}

	#onCancelNewTable() {

		this.newTable = null;
	}

	#insertTables(tables) {

		this.tables.push(...tables.map(table => new Table(table, this.#graphPosition.bind(this))));

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

		let firstRow = tableGraph[0];

		return this.#buildRow(tableGraph, firstRow.length, tables, [...firstRow]);
	}

	#buildRow(tableGraph, totalWidth, tables, placedTables) {

		if (!tables.length) {

			return { tableGraph, totalWidth, tables }
		}

		const lastRow = tableGraph[tableGraph.length - 1];
		let row = [];

		for (let i = tables.length - 1; i >= 0; i--) {

			if (tables[i].dependencies.every(dependency => placedTables.includes(dependency.name)) &&
				row.length < 6) {

				const table = tables.splice(i, 1)[0].name;

				row.push(table);
			}
		}

		if (!row.length) {

			const table = tables.splice(0, 1);
			row.push(table[0].name);
		}

		const rowBeforeLast = tableGraph.length > 1 ? tableGraph[tableGraph.length - 2] : [];

		row.sort(this.#compareWidth(lastRow, rowBeforeLast).bind(this));
		tableGraph.push(row);
		placedTables.push(...row);

		return this.#buildRow(tableGraph, Math.max(totalWidth, row.length), tables, placedTables);
	}

	#compareWidth(lastRow, rowBeforeLast) {

		return (a, b) =>
			(this.#averageWidth(a, lastRow) || this.#averageWidth(a, rowBeforeLast)) -
			(this.#averageWidth(b, lastRow) || this.#averageWidth(b, rowBeforeLast));
	}

	#averageWidth(name, dependencies) {

		const table = this.tables.find(table => table.name == name)
		let total = 0;
		let count = 0;

		for (let i = 0; i < dependencies.length; i++) {

			if (table.dependencies.some(dependency => dependency.name == dependencies[i])) {

				total += i;
				count += 1;
			}
		}

		return count > 0 ? (total / count) / dependencies.length : 0;
	}
}
