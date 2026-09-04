import { defineConfig, devices } from "@playwright/test";

// The instance the tests drive, which is already running and is not started or stopped by them.
// One address is the whole thing: the nginx in front of every instance serves the UI and proxies
// /asyncdb to the database behind it, so localhost:8080 is the first node of `podman-compose up`
// and the `Url` output of the CloudFormation stack is the load balancer in front of all three.
const baseURL = (process.env.ASYNCDB_URL || "http://localhost:8080").replace(/\/+$/, "");

export default defineConfig({
	testDir: ".",
	testMatch: "**/*Test.js",
	globalSetup: "./globalSetup.js",
	// One database behind them all, and each test empties it, so they take it in turn.
	fullyParallel: false,
	workers: 1,
	forbidOnly: !!process.env.CI,
	retries: process.env.CI ? 1 : 0,
	// The html report is written on every run, failed or not: a run against the stack a build
	// stood up cannot be repeated afterwards, so it travels as an artifact of that build.
	reporter: [["list"], ["html", { open: "never" }]],
	use: {
		baseURL: baseURL,
		trace: "on-first-retry"
	},
	projects: [
		{
			name: "chromium",
			use: { ...devices["Desktop Chrome"] }
		}
	]
});
