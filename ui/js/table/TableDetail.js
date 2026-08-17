import html from "~/html/table/tableDetail.html?url";
import { Text, Click } from "@datumjs/datum";

export default class TableDetail {

	#fetchPage;

	#name;

	#cancel;

	constructor(table, fetchPage, cancel) {

		this.#fetchPage = fetchPage;
		this.#name = table.name;
		this.#cancel = cancel;
	}

	onBind(element) {

		this.#fetchPage(element, html);
	}

	title = new Text(() => this.#name);

	cancel = new Click(() => this.#cancel());
}
