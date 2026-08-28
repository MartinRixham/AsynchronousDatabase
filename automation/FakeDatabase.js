// The table API, served to the browser rather than injected into the page, so that the real
// DatabaseClient is what the test drives — its URLs, its verbs, the body it sends and the error
// shape it reads. What each response is comes from doc/database/tables.md.
export default class FakeDatabase {

	#tables = [];

	// Seeds a table the page finds when it loads.
	table(name, ...dependencies) {

		this.#tables.push({ name: name, dependencies: dependencies });

		return this;
	}

	// What the database holds now, which is what the page has written to it.
	get tables() {

		return this.#tables;
	}

	async serve(page) {

		await page.route(/\/asyncdb\/table/, route => this.#handle(route));
	}

	#handle(route) {

		const request = route.request();
		const path = new URL(request.url()).pathname;
		const name = decodeURIComponent(path.replace(/^.*\/asyncdb\/table\/?/, ""));

		if (!name) {

			return this.#json(route, 200, { tables: this.#tables });
		}

		if (request.method() == "PUT") {

			const body = JSON.parse(request.postData() || "{}");

			return this.#putTable(route, name, body.dependencies || []);
		}

		const table = this.#tables.find(table => table.name == name);

		return table ?
			this.#json(route, 200, table) :
			this.#error(route, 404, "table_not_found", "No table named \"" + name + "\".");
	}

	#putTable(route, name, dependencies) {

		const existing = this.#tables.find(table => table.name == name);

		if (existing) {

			// The same options again are 200, and different ones are 409, which is what makes a
			// create safe to run at every start up.
			return JSON.stringify(existing.dependencies) == JSON.stringify(dependencies) ?
				this.#json(route, 200, existing) :
				this.#error(
					route,
					409,
					"table_exists",
					"A table named \"" + name + "\" exists with different options.");
		}

		const missing =
			dependencies.find(dependency => !this.#tables.some(table => table.name == dependency));

		if (missing) {

			return this.#error(
				route, 400, "dependency_not_found", "No table named \"" + missing + "\".");
		}

		const table = { name: name, dependencies: dependencies };

		this.#tables.push(table);

		return this.#json(route, 201, table);
	}

	#json(route, status, body) {

		return route.fulfill({
			status: status,
			contentType: "application/json",
			body: JSON.stringify(body)
		});
	}

	#error(route, status, code, message) {

		return this.#json(route, status, { error: { code: code, message: message } });
	}
}
