import { defineConfig, devices } from "@playwright/test";

// The vite dev server the tests drive. Nothing else listens on it, and it serves ui/index.html and
// the html fragments fetchPage asks for at runtime.
const port = Number(process.env.UI_PORT || 4173);

export default defineConfig({
	testDir: ".",
	testMatch: "**/*Test.js",
	fullyParallel: true,
	forbidOnly: !!process.env.CI,
	retries: process.env.CI ? 1 : 0,
	reporter: process.env.CI ? "list" : [["list"], ["html", { open: "never" }]],
	use: {
		baseURL: "http://localhost:" + port,
		trace: "on-first-retry"
	},
	projects: [
		{
			name: "chromium",
			use: { ...devices["Desktop Chrome"] }
		}
	],
	webServer: {
		command: "npm start -- --port " + port + " --strictPort",
		cwd: "../ui",
		url: "http://localhost:" + port,
		reuseExistingServer: !process.env.CI,
		timeout: 60000
	}
});
