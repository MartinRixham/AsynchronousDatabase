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

	getTables() {
		return { tables: [...this.#tables] };
	}
}
