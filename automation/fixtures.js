import { test as base, expect } from "@playwright/test";
import Database from "./Database";

// The graph the page draws is every table the instance holds, so a test takes the database empty
// and leaves it empty again. That is also why the tests run one at a time (`workers` in
// playwright.config.js).
export const test = base.extend({

	database: async ({ request }, use) => {

		const database = await new Database(request).reset();

		await use(database);

		await database.reset();
	}
});

export { expect };
