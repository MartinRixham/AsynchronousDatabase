import QUnit from "qunit";
import DatabaseClient from "./FakeDatabaseClient";
import App from "~/js/App";

QUnit.module("app");

QUnit.test("cancel new table", async assert => {

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

	assert.equal(app.currentPage.datumPiecesCurrentPage.tables.length, 2);
	assert.equal(app.currentPage.datumPiecesCurrentPage.tableGraph.length, 2);
});

QUnit.test("open and close side bar", async assert => {

	const client = new DatabaseClient();
	const app = new App(client, () => {});

	client.postTable({ name: "first table", dependencies: [] });
	client.postTable({ name: "second table", dependencies: ["first table"] });

	app.onBind();
	app.currentPage.onBind(document.createElement("DIV"));
	await app.currentPage.datumPiecesCurrentPage.onBind();

	await app.currentPage.datumPiecesCurrentPage.tables[0].box().click();

	assert.equal(app.sideBar.title().text(), "first table");

	app.sideBar.cancel().click();

	assert.ok(!app.sideBar);
});
