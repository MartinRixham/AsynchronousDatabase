import html from "~/html/node.html";

export default class Node {

	#fetchPage;

	constructor(fetchPage) {

		this.#fetchPage = fetchPage;
	}

	onBind(element) {

		this.#fetchPage(element, html);
	}
}
