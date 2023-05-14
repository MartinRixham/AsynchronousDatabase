import { NavPiece } from "@datumjs/pieces"

import html from "~/html/app.html"
import fetchPage from "./fetchPage";
import NewTable from "./table/NewTable";
import Tables from "./table/Tables";

export default class App {

	#client;

	constructor(client) {

		this.#client = client;

		this.currentPage =
			new NavPiece([
				{
					route: "tables", page: new Tables(fetchPage, this.#client)
				},
				{
					route: "newTable",
					page: new NewTable(
						fetchPage,
						async () => (await this.#client.getTables()).tables.map(table => table.name),
						this.#client,
						() => { this.currentPage.showPage(0); },
						() => { this.currentPage.showPage(0); })
				}
			]);
	}

	onBind(element) {

		fetchPage(element, html);
	}
}
