import { describe, test, expect } from "vitest";
import NewDependency from "~/js/table/NewDependency";

describe("new dependency", () => {

	test("list dependency options", () => {

		const tables = ["first table", "second table"];
		const dependency = new NewDependency(0, tables, () => {}, () => {});

		expect(dependency.options.length).toBe(2);
		expect(dependency.options[0]().text()).toBe("first table");
		expect(dependency.options[1]().text()).toBe("second table");
	});

	test("add dependency", () => {

		let newDependency = null;

		const addDependency = (dependency) => {

			newDependency = dependency;
		}

		const tables = ["first table", "second table"];
		const dependency = new NewDependency(0, tables, addDependency);

		expect(dependency.label().text()).toBe("first dependency");

		dependency.select().value("second table");

		expect(newDependency.title().text()).toBe("second table");
	});

	test("cannot add empty dependency", () => {

		let newDependency = null;

		const addDependency = (dependency) => {

			newDependency = dependency;
		}

		const tables = ["first table", "second table"];
		const dependency = new NewDependency(0, tables, addDependency);

		dependency.select().value("");

		expect(newDependency).toBeFalsy();
	});

	test("add second dependency", () => {

		const tables = ["first table", "second table"];
		const dependency = new NewDependency(1, tables, () => {});

		expect(dependency.label().text()).toBe("second dependency");
	});

	test("remove dependency", () => {

		let removedDependency = null;

		const removeDependency = (name) => {

			removedDependency = name;
		}

		const dependency = new NewDependency(0, [], () => {}, removeDependency);

		dependency.select().value("first table");
		dependency.remove().click();

		expect(removedDependency).toBe("first table");
	});
});
