export default class {

	postTable(table) {

		fetch("asyncdb/table",
			{
				method: "POST",
				body: JSON.stringify(table)
			});
	}
}
