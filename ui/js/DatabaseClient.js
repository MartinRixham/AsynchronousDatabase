export default class {

	async postTable(table) {

		return fetch("asyncdb/table",
			{
				method: "POST",
				body: JSON.stringify(table)
			})
	}

	async getTables() {

		return fetch("asyncdb/tables")
			.then(response => response.json())
	}
}
