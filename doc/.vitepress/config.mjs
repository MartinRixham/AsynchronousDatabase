import { defineConfig } from "vitepress"

export default defineConfig({
	title: "asyncdb",
	description: "A database for asynchronous data processing",
	lang: "en-GB",
	cleanUrls: true,
	themeConfig: {
		nav: [
			{ text: "Home", link: "/" },
			{ text: "Database API", link: "/database/" }
		],
		sidebar: [
			{
				text: "The project",
				items: [
					{ text: "Overview", link: "/" }
				]
			},
			{
				text: "Database API",
				items: [
					{ text: "Overview", link: "/database/" },
					{ text: "Tables", link: "/database/tables" },
					{ text: "Records", link: "/database/records" },
					{ text: "Scans", link: "/database/scans" },
					{ text: "Reference", link: "/database/reference" }
				]
			}
		],
		outline: [2, 3],
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
