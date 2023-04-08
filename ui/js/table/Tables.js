import { Click, Value } from "Datum";
import html from "~/html/table/tables.html";

import fetchPage from "~/js/fetchPage";

export default class {

    onBind(element) {

        fetchPage(element, html);
    }
}
