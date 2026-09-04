import { test, expect } from "./fixtures";
import { open, box, row } from "./app";

// The journey of somebody who opens the page to read the database that is already there.
test.describe("browse tables", () => {

	test("reads the dependency graph and opens a table in the side bar",
		async ({ page, database }) => {

			await database.table("account");
			await database.table("customer");
			await database.table("transaction", "account");

			await open(page);

			await expect(page.locator("svg rect")).toHaveCount(3);

			// The two tables that depend on nothing share the top row, and the one that depends on
			// account is drawn in the row under it, with an arrow from the one to the other.
			expect(await row(page, "customer")).toEqual(await row(page, "account"));
			expect(await row(page, "transaction")).toBeGreaterThan(await row(page, "account"));

			await expect(page.locator("svg g[data-bind='dependencies'] path")).toHaveCount(1);

			await box(page, "transaction").click();

			const detail = page.locator(".table-detail");

			await expect(detail).toContainText("transaction");

			await detail.locator("button.cancel").click();

			await expect(detail).toHaveCount(0);
		});
});
