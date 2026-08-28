// Loads the page with the fake database behind it, and finds a table in the graph the page draws.

export async function open(page, database) {

	await database.serve(page);
	await page.goto("/");

	// The page fetches its own html at runtime, so a test starts once the tables page is there.
	await page.locator(".tables .new-table").waitFor();
}

// The box of one table: the group holding a rect is the one the table is drawn in, and the only
// text in it is the table's name.
export function box(page, name) {

	return page
		.locator("svg g:has(> rect)")
		.filter({ hasText: new RegExp("^\\s*" + name + "\\s*$") })
		.locator("rect");
}

// Which row of the graph a table is drawn in. A row is 180 apart from the next one, so the y of the
// box says which one it is without a test having to know the coordinate itself.
export async function row(page, name) {

	return Math.round((Number(await box(page, name).getAttribute("y")) - 25) / 180);
}
