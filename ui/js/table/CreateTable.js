import { Click, Value } from "Datum";
import html from "~/html/table/createTable.html";

import fetchPage from "~/js/fetchPage";

export default class {

    #title = "";

    onBind(element) {

        fetchPage(element, html);
    }

    title = new Value((value) => {

        if (value) {

            this.#title = value;
        }

        return this.#title;
    });

    create = new Click(() => {

        fetch("asyncdb/table",
            {
                method: "POST",
                body: JSON.stringify({ title: this.#title })
            });
    });
}
