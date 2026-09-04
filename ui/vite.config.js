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

// `npm start` serves the UI on its own, without the nginx that fronts it in the
// image, so `/asyncdb` is proxied to a server run by `cmk run` instead.  The
// prefix is stripped the way `server/server.conf` strips it, off the raw target
// rather than the decoded path, so a key holding a %2F stays one segment: Vite
// hands `rewrite` the untouched `req.url`, query string and all.
const proxy = {
	"/asyncdb": {
		target: process.env.ASYNCDB_URL || "http://localhost:8080",
		changeOrigin: true,
		rewrite: path => path.replace(/^\/asyncdb\/*/, "/")
	}
};

export default defineConfig({
	plugins: [renameDatum],
	server: { proxy },
	preview: { proxy },
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
