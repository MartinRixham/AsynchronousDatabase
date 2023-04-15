export default class {

	postTable(table) {

		fetch("asyncdb/table",
			{
				method: "POST",
				body: JSON.stringify(table)
			});
	}

	getTables(callback) {

		fetch("asyncdb/tables")
			.then(response => response.json())
			.then(json => callback(json));
	}
}
