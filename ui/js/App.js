import { NavPiece } from "@datumjs/pieces";

import html from "~/html/app.html";
import Node from "./Node";
import NewTable from "./table/NewTable";
import Tables from "./table/Tables";

export default class App {

	currentPage = null;

	sideBar = null;

	nodeDetails = null;

	#client;

	#fetchPage;

	constructor(client, fetchPage) {

		this.#client = client;
		this.#fetchPage = fetchPage;
	}

	onBind(element) {

		this.#fetchPage(element, html);
	
		this.currentPage =
			new NavPiece([
				{
					route: "tables",
					page: new Tables(
						this.#fetchPage,
						this.#client,
						{
							open: this.#setSideBar.bind(this),
							cancel: this.#closeSideBar.bind(this)
						})
				},
				{
					route: "newTable",
					page: new NewTable(
						this.#fetchPage,
						async () => (await this.#client.getTables()).tables.map(table => table.name),
						this.#client,
						() => { this.currentPage.showPage(0); },
						() => { this.currentPage.showPage(0); })
				}
			]);

		this.nodeDetails = new Node(this.#fetchPage);
	}

	#setSideBar(component) {

		this.sideBar = component;
	}

	#closeSideBar() {

		this.sideBar = null;
	}
}
