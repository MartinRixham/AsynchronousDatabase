// The tables of the instance the tests are running against, over the same API the UI itself uses,
// so what a create really answers is what the assertions see. doc/database/tables.md is the spec.
export default class Database {

	#request;

	constructor(request) {

		this.#request = request;
	}

	// Drops every table there is, so a test starts on an empty graph. The graph the page draws is
	// every table the instance holds, which is why a test owns the whole database.
	async reset() {

		for (const table of await this.tables()) {

			await this.#send(this.#request.delete(this.#url(table.name)), 204);
		}

		return this;
	}

	// Seeds a table the page finds when it loads. Every dependency has to be a table already, so
	// they are seeded in the order they depend on each other.
	async table(name, ...dependencies) {

		await this.#send(
			this.#request.put(this.#url(name), { data: { dependencies: dependencies } }), 201);

		return this;
	}

	// What the database holds now, which is what the page has written to it.
	async tables() {

		return (await this.#send(this.#request.get("asyncdb/table"), 200)).tables;
	}

	#url(name) {

		return "asyncdb/table/" + encodeURIComponent(name);
	}

	// The fixture is not the thing under test, so an unexpected answer from it is a failure of the
	// instance and not of the journey.
	async #send(pending, status) {

		const response = await pending;

		if (response.status() != status) {

			throw new Error(
				response.url() + " answered " + response.status() + " rather than " + status +
				": " + await response.text());
		}

		return status == 204 ? null : response.json();
	}
}
