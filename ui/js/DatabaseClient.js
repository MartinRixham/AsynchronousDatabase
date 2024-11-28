export default class DatabaseClient {

	async postTable(table) {

		return fetch("asyncdb/table",
			{
				method: "POST",
				body: JSON.stringify(table)
			})
			.then(response => response.json())
	}

	async getTable(name) {

		return fetch("asyncdb/table?name=" + name)
			.then(response => response.json())
	}

	async getTables() {

		return fetch("asyncdb/tables")
			.then(response => response.json())
	}
}
