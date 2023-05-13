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

		this.tables.push(new Table(table, (name) => {

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
		}));

		this.#buildGraph();
	}

	#buildGraph() {

		let tables = [...this.tables];
		let tableGraph = [[]]

		for (let i = tables.length - 1; i >= 0; i--) {

			if (!tables[i].dependencies.length) {

				let table = tables.splice(i, 1);
				tableGraph[0].push(table[0].name);
			}
		}

		let totalWidth = tableGraph[0].length;

		while (tables.length > 0) {

			const lastRow = tableGraph[tableGraph.length - 1];
			let row = [];

			for (let i = tables.length - 1; i >= 0; i--) {

				if (this.#intersect(tables[i].dependencies, lastRow)) {

					const table = tables.splice(i, 1);
					row.push(table[0].name);
				}
			}

			if (!row.length) {

				const table = tables.splice(0, 1);
				row.push(table[0].name);
			}

			row.sort((a, b) => this.#averageWidth(a, lastRow) - this.#averageWidth(b, lastRow))

			totalWidth = Math.max(totalWidth, row.length);
			tableGraph.push(row);
		}

		this.#totalWidth = totalWidth;
		this.tableGraph = tableGraph;
	}

	#intersect(dependencies, row) {

		for (let i = 0; i < dependencies.length; i++) {

			if (row.includes(dependencies[i].name)) {

				return true;
			}
		}

		return false;
	}

	#averageWidth(name, dependencies) {

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
