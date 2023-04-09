import { Click, Value } from "Datum";
import html from "~/html/table/newTable.html";

export default class {

	#title = "";

	constructor(fetchPage, client) {

		this.fetchPage = fetchPage;
		this.client = client;
	}

	onBind(element) {

		this.fetchPage(element, html);
	}

	title = new Value((value) => {

		if (value) {

			this.#title = value;
		}

		return this.#title;
	});

	save = new Click(() => {

		this.client.postTable({ title: this.#title });
	});
}
