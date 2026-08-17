import { describe, test, expect } from "vitest";
import DatabaseClient from "./FakeDatabaseClient";
import App from "~/js/App";

describe("app", () => {

	test("cancel new table", async () => {

		const client = new DatabaseClient();
		const app = new App(client, () => {});

		client.postTable({ name: "first table", dependencies: [] });
		client.postTable({ name: "second table", dependencies: ["first table"] });

		app.onBind();
		app.currentPage.onBind(document.createElement("DIV"));
		await app.currentPage.datumPiecesCurrentPage.onBind();

		app.currentPage.showPage(1);

		await app.currentPage.datumPiecesCurrentPage.onBind();

		app.currentPage.datumPiecesCurrentPage.cancel().click();

		await app.currentPage.datumPiecesCurrentPage.onBind();

		expect(app.currentPage.datumPiecesCurrentPage.tables.length).toBe(2);
		expect(app.currentPage.datumPiecesCurrentPage.tableGraph.length).toBe(2);
	});

	test("open and close side bar", async () => {

		const client = new DatabaseClient();
		const app = new App(client, () => {});

		client.postTable({ name: "first table", dependencies: [] });
		client.postTable({ name: "second table", dependencies: ["first table"] });

		app.onBind();
		app.currentPage.onBind(document.createElement("DIV"));
		await app.currentPage.datumPiecesCurrentPage.onBind();

		await app.currentPage.datumPiecesCurrentPage.tables[0].box().click();

		expect(app.sideBar.title().text()).toBe("first table");

		app.sideBar.cancel().click();

		expect(app.sideBar).toBeFalsy();
	});
});
