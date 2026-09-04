import { test as base, expect } from "@playwright/test";
import Database from "./Database";

// Every test drives one instance that is already running, and the graph the page draws is every
// table that instance holds — so a test takes the database empty and leaves it empty again rather
// than reading another test's tables out of the SVG. That is also why the tests run one at a time
// (`workers` in playwright.config.js), and why a retry starts from the same clean database the
// first attempt did.
export const test = base.extend({

	database: async ({ request }, use) => {

		const database = await new Database(request).reset();

		await use(database);

		await database.reset();
	}
});

export { expect };
