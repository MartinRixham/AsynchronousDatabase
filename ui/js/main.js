import { BindingRoot } from "Datum";
import App from "~/js/App";

function main() {

	console.log("Running!!");

	new BindingRoot(new App());
}

main();
