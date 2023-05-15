import { NavPiece } from "@datumjs/pieces"

import html from "~/html/app.html"
import fetchPage from "./fetchPage";
import NewTable from "./table/NewTable";
import Tables from "./table/Tables";

export default class App {

	currentPage = null;

	sideBar = null;

	#client;

	constructor(client) {

		this.#client = client;
	}

	onBind(element) {

		fetchPage(element, html);
	
		this.currentPage =
			new NavPiece([
				{
					route: "tables", page: new Tables(fetchPage, this.#client, this.#setSideBar.bind(this))
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

	#setSideBar(component) {

		this.sideBar = component;
	}
}
