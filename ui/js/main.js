import { BindingRoot } from "Datum";
import DatabaseClient from "./DatabaseClient";
import fetchPage from "./fetchPage";
import App from "~/js/App";

new BindingRoot(new App(new DatabaseClient(), fetchPage));
