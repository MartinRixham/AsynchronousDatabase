import { request } from "@playwright/test";

// Nothing is started for the tests, so the run stops here — with the address it tried and how to
// give it one — rather than in a browser that was served nothing. The short wait is what makes
// `podman-compose up -d && npm test` work: compose returns before the database has opened RocksDB.
export default async function globalSetup(config) {

	const baseURL = config.projects[0].use.baseURL;
	const context = await request.newContext({ baseURL: baseURL });
	const deadline = Date.now() + 60000;

	try {

		for (;;) {

			if (await answers(context)) {

				return;
			}

			if (Date.now() > deadline) {

				throw new Error(
					"No instance answered /asyncdb/health at " + baseURL + ". Start one with " +
					"`podman-compose up -d` in the repository root, or point ASYNCDB_URL at one.");
			}

			await new Promise(resolve => setTimeout(resolve, 1000));
		}
	}
	finally {

		await context.dispose();
	}
}

// A node that can order no write answers this 503 rather than 200, and it is still a node that
// answered: waiting the whole minute out for it would report an instance that is not there instead
// of an instance that cannot take the writes a journey starts with. What is waited for is a health
// document, whatever status carried it.
async function answers(context) {

	try {

		const response = await context.get("asyncdb/health");

		return typeof (await response.json()).status == "string";
	}
	catch {

		// Nothing listening yet, or nginx answering its own error document for a database that has
		// not opened RocksDB, which is what the wait is for.
		return false;
	}
}
