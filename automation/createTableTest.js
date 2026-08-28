import { test, expect } from "@playwright/test";
import FakeDatabase from "./FakeDatabase";
import { open, box, row } from "./app";

async function newTable(page, name) {

	await page.locator("button.new-table").click();
	await expect(page.locator(".create-table")).toBeVisible();

	const title = page.locator("#tableName");

	await title.fill(name);
	await title.blur();
}

// The journey of somebody adding a table to the database, and the one error the database answers
// them with when the name is already taken.
test.describe("create table", () => {

	test("adds a table that depends on another and sees it in the graph", async ({ page }) => {

		const database = new FakeDatabase().table("account").table("transaction", "account");

		await open(page, database);
		await newTable(page, "ledger");

		await page.locator(".create-table select").selectOption("transaction");

		await expect(page.locator(".create-table .dependency")).toHaveText(/transaction/);

		await page.locator("button.submit").click();

		// The form closes and the new table joins the graph, in the row under the one it depends
		// on and with an arrow of its own.
		await expect(page.locator(".create-table")).toHaveCount(0);

		expect(await row(page, "ledger")).toBeGreaterThan(await row(page, "transaction"));

		await expect(page.locator("svg g[data-bind='dependencies'] path")).toHaveCount(2);

		// The name and the dependency reached the database, and it is the new table the side bar
		// opens on.
		expect(database.tables).toContainEqual({ name: "ledger", dependencies: ["transaction"] });

		await box(page, "ledger").click();

		await expect(page.locator(".table-detail")).toContainText("ledger");
	});

	test("shows the error a name that is taken is answered with", async ({ page }) => {

		const database = new FakeDatabase().table("account").table("transaction", "account");

		await open(page, database);

		// The name is taken by a table that has a dependency, so creating it without one is 409.
		await newTable(page, "transaction");
		await page.locator("button.submit").click();

		await expect(page.locator(".create-table .error"))
			.toHaveText("A table named \"transaction\" exists with different options.");

		expect(database.tables).toHaveLength(2);
	});
});
