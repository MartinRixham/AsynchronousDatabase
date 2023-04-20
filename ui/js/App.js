import { Text } from "Datum"
import { NavPiece } from "@datumjs/pieces"

import html from "~/html/app.html"
import fetchPage from "./fetchPage";
import DatabaseClient from "./DatabaseClient";
import NewTable from "./table/NewTable";
import Tables from "./table/Tables";

export default class App {

	#client = new DatabaseClient();

	currentPage =
		new NavPiece([
			{
				route: "tables", page: new Tables(fetchPage, this.#client)
			},
			{
				route: "newTable",
				page: new NewTable(
					fetchPage,
					callback => this.#client.getTables(tables => callback(tables.tables.map((table) => new Text(() => table.name)))),
					this.#client,
					() => { this.currentPage.setPage("tables")})
			}
		]);

	onBind(element) {

		fetchPage(element, html);
	}
}
