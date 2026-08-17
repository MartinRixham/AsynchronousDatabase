import { defineConfig } from "vitest/config";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL(".", import.meta.url)).replace(/\/$/, "");

// @datumjs/pieces is a UMD bundle that still asks for the pre-rename "Datum"
// package.  The alias below redirects it for the dev server and the build,
// which resolve it through Vite.  Under Vitest the bundle is loaded as
// CommonJS by Node, which ignores the alias, so its two references to "Datum"
// are rewritten in place as well.
const renameDatum = {
	name: "rename-datum",
	enforce: "pre",
	transform(code, id) {

		if (id.includes("@datumjs/pieces")) {

			return code.replace(/(["'])Datum\1/g, "\"@datumjs/datum\"");
		}
	}
};

export default defineConfig({
	plugins: [renameDatum],
	resolve: {
		alias: [
			{ find: /^~/, replacement: root },
			{ find: /^Datum$/, replacement: "@datumjs/datum" }
		]
	},
	test: {
		environment: "jsdom",
		include: ["test/**/*Test.js"],
		server: {
			deps: {
				inline: ["@datumjs/pieces"]
			}
		}
	}
});
