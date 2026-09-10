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

	// The health of whichever instance served the page, which behind nginx is the one it is
	// proxying to. Every field of it is that node's own view: the membership is what it can see,
	// and write_stalled and incomplete are true of it and of no other node.
	async getHealth() {

		return fetch("asyncdb/health")
			.then(response => response.json())
	}
}
