export default class {

	#tables = [];

	#health = { status: "ok", write_stalled: false, incomplete: false };

	putTable(table) {

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

	setHealth(health) {

		this.#health = health;
	}

	getHealth() {

		if (!this.#health) {

			throw new Error("The node did not answer.");
		}

		return this.#health;
	}
}
