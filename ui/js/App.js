import { NavPiece } from "@datumjs/pieces"

import html from "~/html/app.html"
import fetchPage from "~/js/fetchPage";
import CreateTable from "~/js/table/CreateTable";

export default class App {

	currentPage =
		new NavPiece([{ route: "createTable", page: new CreateTable() }]);

	onBind(element) {

		fetchPage(element, html);
	}
}
