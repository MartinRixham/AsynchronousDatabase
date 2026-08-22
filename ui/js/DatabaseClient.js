export default class DatabaseClient {

	async putTable(table) {

		// Through JSON, so that a table built by the page is sent as the name and the dependencies
		// the API takes and nothing else.
		const { name, dependencies } = JSON.parse(JSON.stringify(table))

		const response = await fetch("asyncdb/table/" + encodeURIComponent(name),
			{
				method: "PUT",
				headers: { "Content-Type": "application/json" },
				body: JSON.stringify({ dependencies: dependencies })
			})

		const body = await response.json()

		// An error is { error: { code, message } }, and the page shows the message.
		return body.error ? { error: body.error.message } : body
	}

	async getTable(name) {

		return fetch("asyncdb/table/" + encodeURIComponent(name))
			.then(response => response.json())
	}

	async getTables() {

		return fetch("asyncdb/table")
			.then(response => response.json())
	}
}
