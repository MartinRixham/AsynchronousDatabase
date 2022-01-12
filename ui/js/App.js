import Datum from "Datum";
import Pieces from "@datumjs/pieces"

import html from "~/html/app.html"
import fetchPage from "~/js/fetchPage";

export default class App {

	currentPage =
		new Pieces.NavPiece([{ route: "wat", page: {} }]);

	onBind(element) {

		fetchPage(element, html);
	}
}
