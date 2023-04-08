export default class {

	#tables = [];

	postTable(table) {
		this.#tables.push(table);
	}

	getTables() {
		return [...this.#tables]
	}
}
