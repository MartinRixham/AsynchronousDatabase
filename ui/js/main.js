import Datum from "Datum";
import App from "~/js/App";

function main() {

	console.log("Running!!");

	new Datum.BindingRoot(new App());
}

main();
