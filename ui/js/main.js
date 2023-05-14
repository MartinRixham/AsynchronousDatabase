import { BindingRoot } from "Datum";
import DatabaseClient from "./DatabaseClient";
import App from "~/js/App";

new BindingRoot(new App(new DatabaseClient()));
