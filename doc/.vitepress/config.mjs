import { defineConfig } from "vitepress"

export default defineConfig({
	title: "asyncdb",
	description: "A database for asynchronous data processing",
	lang: "en-GB",
	cleanUrls: true,
	themeConfig: {
		nav: [
			{ text: "Home", link: "/" }
		],
		socialLinks: [
			{ icon: "github", link: "https://github.com/MartinRixham/AsynchronousDatabase" }
		],
		search: {
			provider: "local"
		},
		footer: {
			message: "Released under the AGPL-3.0 licence."
		}
	}
})
