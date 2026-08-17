import { describe, test, expect } from "vitest";
import Table from "~/js/table/Table";

describe("table", () => {

	test("table has name and dependencies", () => {

		const table = new Table({
			name: "table name",
			dependencies: ["dependency one", "dependency two"]
		},
		() => ({ width: 3, depth: 0 }));

		expect(table.title().text()).toBe("table name");
		expect(table.graphPosition()).toEqual({ width: 3, depth: 0 });

		expect(table.dependencies[0].name).toBe("dependency one");
		expect(table.dependencies[1].name).toBe("dependency two");
	});
});
