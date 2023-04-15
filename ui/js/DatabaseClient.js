export default class {

	postTable(table, callback) {

		fetch("asyncdb/table",
			{
				method: "POST",
				body: JSON.stringify(table)
			})
			.then(() => callback());
	}

	getTables(callback) {

		fetch("asyncdb/tables")
			.then(response => response.json())
			.then(json => callback(json));
	}
}
