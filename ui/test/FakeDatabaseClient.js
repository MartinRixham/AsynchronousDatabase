export default class {

	#tables = [];

	postTable(table) {
		this.#tables.push(table);
	}

	getTables() {
		return { tables: [...this.#tables] };
	}
}
