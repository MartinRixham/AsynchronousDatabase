import { test, expect } from "./fixtures";
import { open } from "./app";

// The panel the page draws for the instance serving it. Nothing here is stubbed: what the side bar
// shows is what that node answered /health with, which is the only test anywhere that the names the
// markup binds are the names the component exposes.
test.describe("node health", () => {

	test("reads the health of the instance serving the page", async ({ page }) => {

		await open(page);

		const panel = page.locator(".node-detail");

		// A node that is answering the page at all is a node whose status is ok, so this is the
		// binding and not the state: an empty value here is a data-bind naming nothing.
		await expect(panel).toContainText("ok");

		// Back pressure and a store short of its share are both false of an instance the suite is
		// running against, and both are drawn whatever they say.
		await expect(panel).toContainText("flowing");
		await expect(panel).toContainText("whole");
	});

	// The compose cluster is three nodes in two zones and the deployed stack is six in three, so
	// what is asserted is that the membership is drawn at all rather than how large it is.
	test("names the zones the instance can see", async ({ page }) => {

		await open(page);

		// The panel draws both and hides one, so what says which it is drawing is whether the
		// standing-alone line is visible and never whether it is in the document.
		if (await page.locator(".node-detail .alone").isVisible()) {

			test.skip(true, "The instance under test is not in a cluster.");
		}

		await expect(page.locator(".node-detail")).toContainText(/\d+ of 256/);
		await expect(page.locator(".node-detail").getByText(/^\d+ nodes?$/).first()).toBeVisible();
	});

	// Where the instance read that membership. The member it names is an address of the
	// deployment rather than of this database, so what is asserted is that it holds its lease
	// there and names one at all.
	test("names the etcd it reads its membership from", async ({ page }) => {

		await open(page);

		if (await page.locator(".node-detail .alone").isVisible()) {

			test.skip(true, "The instance under test is not in a cluster.");
		}

		const panel = page.locator(".node-detail");

		await expect(panel).toContainText("held");
		await expect(panel.getByText(/^https?:\/\/\S+$/).first()).toBeVisible();
	});
});
