export default class {

	#tables = [];

	postTable(table) {

		if (this.#tables.some(t => t.name == table.name)) {

			return { error: "A table with the name \"" + table.name + "\" already exists." };
		}
		else {

			this.#tables.push(table);
		}
	}

	getTable(name) {

		return this.#tables.find(table => table.name == name);
	}

	getTables() {

		return { tables: [...this.#tables] };
	}
}
