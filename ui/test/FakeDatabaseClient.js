export default class {

	#tables = [];

	postTable(table) {
		this.#tables.push(table);
	}

	getTables(callback) {
		callback({ tables: [...this.#tables] });
	}
}
