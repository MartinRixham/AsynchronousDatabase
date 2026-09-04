import { request } from "@playwright/test";

// Nothing is started for the tests, so the run stops here — with the address it tried and how to
// give it one — rather than in a browser that was served nothing. The instance is allowed a short
// while to answer, which is what makes `podman-compose up -d && npm test` work: compose returns as
// soon as the containers are made, and the database still has to open RocksDB and register itself
// with etcd.
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

async function answers(context) {

	try {

		return (await context.get("asyncdb/health")).ok();
	}
	catch {

		// Nothing listening yet, which is what the wait is for.
		return false;
	}
}
